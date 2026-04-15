/**
 * denver_voice.h — Atom Echo → Denver voice_server
 *
 * Modo STREAMING: el audio se envía al servidor en chunks de 512B
 * mientras se graba, sin acumular el PCM completo en el Atom.
 * Memoria ahorrada: ~128KB (PCM_MAX_SAMPLES × 2 bytes).
 *
 * Protocolo:
 *   POST /voice HTTP/1.1  (sin Content-Length)
 *   Connection: close
 *   [chunks PCM 512B mientras el botón esté presionado]
 *   → shutdown(SHUT_WR)   señaliza EOF al servidor
 *   → leer respuesta WAV y reproducir (igual que antes)
 *
 * Pines verificados contra YAML oficial M5Stack Atom Echo:
 *   Mic PDM SPM1423: CLK=GPIO33, DATA=GPIO23, gain×4
 *   Spk NS4168:      BCLK=GPIO19, LRCLK=GPIO33, DOUT=GPIO22, PA_EN=GPIO21
 *   Botón:           GPIO39 (activo LOW, input-only)
 */

#pragma once
#include "esphome.h"
#include <esp_heap_caps.h>
#include <driver/i2s_pdm.h>
#include <driver/i2s_std.h>
#include <driver/gpio.h>
#include <lwip/sockets.h>
#include <lwip/netdb.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <sys/socket.h>

// ─── Pines ────────────────────────────────────────────────────────────────────
#define MIC_CLK      GPIO_NUM_33
#define MIC_DATA     GPIO_NUM_23
#define SPK_BCLK     GPIO_NUM_19
#define SPK_WS       GPIO_NUM_33
#define SPK_DOUT     GPIO_NUM_22
#define PA_EN        GPIO_NUM_21
#define BTN_PIN      GPIO_NUM_39

// ─── Config ───────────────────────────────────────────────────────────────────
#define MIC_RATE          16000
#define MIC_GAIN          4

// Configuración del servidor (inyectada desde YAML si es posible)
#ifndef SERVER_HOST
#define SERVER_HOST       "192.168.10.104"
#endif
#ifndef SERVER_PORT
#define SERVER_PORT       8001
#endif

// Chunk de grabación: 512 samples × 2 bytes = 1KB por envío
// Ajustá hacia arriba (1024) si el servidor se queja de demasiados paquetes
#define REC_CHUNK_SAMPLES 512
#define REC_CHUNK_BYTES   (REC_CHUNK_SAMPLES * 2)

// Chunk de reproducción (igual que antes)
#define PLAY_CHUNK        4096
#define WAV_HEADER_SIZE   44
#define AUDIO_GAIN        1.1f

// Warmup del micrófono: descartar primeros 100ms
#define MIC_WARMUP_SAMPLES (MIC_RATE / 10)

// ─── Estado global ────────────────────────────────────────────────────────────
// Solo necesitamos el chunk de grabación (1KB) y el de reproducción (4KB)
// Los 128KB de _pcm_buf estático ya no existen.
static int16_t          _rec_chunk[REC_CHUNK_SAMPLES];  // 1KB en BSS
static volatile bool    _recording    = false;
static volatile bool    _denver_busy  = false;
static i2s_chan_handle_t _mic_chan     = nullptr;
static i2s_chan_handle_t _spk_chan     = nullptr;

// ─── LED ──────────────────────────────────────────────────────────────────────
static esphome::light::LightState *_led = nullptr;

static void led_set(float r, float g, float b) {
  if (!_led)
    for (auto *l : App.get_lights()) {
      char buf[128];
      auto ref = l->get_object_id_to(std::span<char, 128>(buf, 128));
      if (std::string(buf, ref.size()) == "led") { _led = l; break; }
    }
  if (!_led) return;
  auto c = _led->turn_on();
  c.set_red(r); c.set_green(g); c.set_blue(b);
  c.set_brightness(0.6f); c.perform();
}

// ─── PA_EN ────────────────────────────────────────────────────────────────────
static void pa_enable() {
  gpio_set_direction(PA_EN, GPIO_MODE_OUTPUT);
  gpio_set_level(PA_EN, 1);
  vTaskDelay(pdMS_TO_TICKS(50));
}
static void pa_disable() { gpio_set_level(PA_EN, 0); }

// ─── I2S helpers ─────────────────────────────────────────────────────────────
static void mic_deinit() {
  if (!_mic_chan) return;
  i2s_channel_disable(_mic_chan);
  i2s_del_channel(_mic_chan);
  _mic_chan = nullptr;
}

static esp_err_t mic_init() {
  mic_deinit();
  i2s_chan_config_t cc = I2S_CHANNEL_DEFAULT_CONFIG(I2S_NUM_0, I2S_ROLE_MASTER);
  cc.dma_desc_num = 8; cc.dma_frame_num = 256;
  esp_err_t r = i2s_new_channel(&cc, nullptr, &_mic_chan);
  if (r != ESP_OK) return r;
  i2s_pdm_rx_config_t cfg = {
    .clk_cfg  = I2S_PDM_RX_CLK_DEFAULT_CONFIG(MIC_RATE),
    .slot_cfg = I2S_PDM_RX_SLOT_DEFAULT_CONFIG(I2S_DATA_BIT_WIDTH_16BIT, I2S_SLOT_MODE_MONO),
    .gpio_cfg = { .clk = MIC_CLK, .din = MIC_DATA, .invert_flags = {false} },
  };
  r = i2s_channel_init_pdm_rx_mode(_mic_chan, &cfg);
  if (r != ESP_OK) return r;
  r = i2s_channel_enable(_mic_chan);
  if (r == ESP_OK) ESP_LOGI("denver", "PDM mic OK @ %dHz", MIC_RATE);
  return r;
}

static void spk_deinit() {
  if (!_spk_chan) return;
  i2s_channel_disable(_spk_chan);
  i2s_del_channel(_spk_chan);
  _spk_chan = nullptr;
  pa_disable();
}

static esp_err_t spk_init(uint32_t sr) {
  spk_deinit();
  i2s_chan_config_t cc = I2S_CHANNEL_DEFAULT_CONFIG(I2S_NUM_1, I2S_ROLE_MASTER);
  esp_err_t r = i2s_new_channel(&cc, &_spk_chan, nullptr);
  if (r != ESP_OK) return r;
  i2s_std_config_t sc = {
    .clk_cfg  = I2S_STD_CLK_DEFAULT_CONFIG(sr),
    .slot_cfg = I2S_STD_MSB_SLOT_DEFAULT_CONFIG(I2S_DATA_BIT_WIDTH_16BIT, I2S_SLOT_MODE_STEREO),
    .gpio_cfg = { .mclk = I2S_GPIO_UNUSED, .bclk = SPK_BCLK, .ws = SPK_WS,
                  .dout = SPK_DOUT, .din = I2S_GPIO_UNUSED,
                  .invert_flags = {false, false, false} },
  };
  sc.slot_cfg.slot_mask = I2S_STD_SLOT_BOTH;
  r = i2s_channel_init_std_mode(_spk_chan, &sc);
  if (r != ESP_OK) return r;
  r = i2s_channel_enable(_spk_chan);
  pa_enable();
  return r;
}

// ─── Reproducción PCM mono → stereo con gain ─────────────────────────────────
static void IRAM_ATTR play_pcm(const uint8_t *data, int len) {
  if (len <= 0 || !_spk_chan) return;
  const int16_t *src = (const int16_t *)data;
  const int BLK = 128;
  int16_t st[BLK * 2];
  for (int b = 0, s = len / 2; b < s; b += BLK) {
    int n = (b + BLK <= s) ? BLK : s - b;
    for (int i = 0; i < n; i++) {
      int32_t v = (int32_t)(src[b + i] * AUDIO_GAIN);
      v = v > 32767 ? 32767 : v < -32768 ? -32768 : v;
      st[i * 2] = st[i * 2 + 1] = (int16_t)v;
    }
    size_t wr = 0;
    i2s_channel_write(_spk_chan, st, n * 4, &wr, pdMS_TO_TICKS(2000));
  }
}

// ─── Socket helpers ───────────────────────────────────────────────────────────
static bool sock_send_all(int fd, const uint8_t *data, size_t len) {
  for (size_t sent = 0; sent < len; ) {
    int n = ::send(fd, data + sent, len - sent, 0);
    if (n <= 0) return false;
    sent += n;
  }
  return true;
}

static int sock_read_line(int fd, char *buf, int maxlen) {
  int i = 0; uint8_t b;
  while (i < maxlen - 1 && ::recv(fd, &b, 1, 0) == 1) {
    if (b == '\n') break;
    if (b != '\r') buf[i++] = (char)b;
  }
  buf[i] = '\0';
  return i;
}

static size_t sock_chunk_size(int fd) {
  char hex[16] = {};
  sock_read_line(fd, hex, sizeof(hex));
  return (size_t)strtoul(hex, nullptr, 16);
}

static bool sock_read_exact(int fd, uint8_t *buf, size_t len) {
  size_t got = 0;
  while (got < len) {
    int n = ::recv(fd, buf + got, len - got, 0);
    if (n <= 0) return got > 0;
    got += n;
  }
  return true;
}

static void sock_skip_to_lf(int fd) {
  uint8_t b;
  while (::recv(fd, &b, 1, 0) == 1 && b != '\n') {}
}

// ─── Chunked Transfer-Encoding helpers ───────────────────────────────────────
// Formato: "<tamaño en hex>\r\n<datos>\r\n"
// Chunk final (fin de body): "0\r\n\r\n"

static bool sock_send_chunk(int fd, const uint8_t *data, size_t len) {
  // Línea de tamaño
  char sz_line[16];
  int sz_len = snprintf(sz_line, sizeof(sz_line), "%X\r\n", (unsigned)len);
  if (!sock_send_all(fd, (uint8_t *)sz_line, sz_len)) return false;
  // Datos
  if (!sock_send_all(fd, data, len)) return false;
  // CRLF de cierre del chunk
  if (!sock_send_all(fd, (uint8_t *)"\r\n", 2)) return false;
  return true;
}

static bool sock_send_final_chunk(int fd) {
  return sock_send_all(fd, (uint8_t *)"0\r\n\r\n", 5);
}

// ─── Abrir socket TCP al servidor ─────────────────────────────────────────────
static int open_socket() {
  struct addrinfo hints{}, *res = nullptr;
  hints.ai_family   = AF_INET;
  hints.ai_socktype = SOCK_STREAM;
  char port[8];
  snprintf(port, sizeof(port), "%d", SERVER_PORT);
  if (getaddrinfo(SERVER_HOST, port, &hints, &res) != 0 || !res) return -1;

  int fd = ::socket(res->ai_family, res->ai_socktype, res->ai_protocol);
  if (fd < 0) { freeaddrinfo(res); return -1; }

  // Timeout generoso: grabación sin límite + procesamiento STT + TTS
  const struct timeval tv{.tv_sec = 120, .tv_usec = 0};
  setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
  setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));

  // Sin Nagle: enviar chunks pequeños sin esperar buffering del stack TCP
  const int flag = 1;
  setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &flag, sizeof(flag));

  if (::connect(fd, res->ai_addr, res->ai_addrlen) != 0) {
    freeaddrinfo(res); ::close(fd); return -1;
  }
  freeaddrinfo(res);
  return fd;
}

// ─── Tarea principal (streaming) ──────────────────────────────────────────────
static void record_task(void *) {
  int      fd          = -1;
  uint8_t *play_buf    = nullptr;
  uint32_t total_bytes = 0;   // para log de duración
  bool     mic_ok      = false;

  // ── 1. Inicializar micrófono ──────────────────────────────────────────────
  led_set(1, 0, 0);  // rojo = preparando

  if (mic_init() != ESP_OK) {
    ESP_LOGE("denver", "mic_init falló");
    goto done;
  }
  mic_ok = true;

  // Warmup: descartar primeros 100ms para que el SPM1423 se estabilice
  {
    int16_t tmp[64];
    for (uint32_t disc = 0; disc < MIC_WARMUP_SAMPLES; ) {
      size_t got = 0;
      i2s_channel_read(_mic_chan, tmp, sizeof(tmp), &got, pdMS_TO_TICKS(100));
      disc += got / 2;
    }
  }

  // ── 2. Conectar al servidor ───────────────────────────────────────────────
  // Conectamos ANTES de empezar a grabar para minimizar latencia posterior.
  // En LAN la conexión tarda < 5ms, así que el LED rojo es casi imperceptible.
  led_set(0, 0, 1);  // azul = conectando
  fd = open_socket();
  if (fd < 0) {
    ESP_LOGE("denver", "No se pudo conectar a %s:%d", SERVER_HOST, SERVER_PORT);
    goto done;
  }

  // ── 3. Enviar headers HTTP ────────────────────────────────────────────────
  // Transfer-Encoding: chunked → mecanismo estándar HTTP/1.1 para body de
  // longitud desconocida. uvicorn/FastAPI lo soporta nativamente.
  // Cada chunk de PCM se envía como: "<hex_size>\r\n<datos>\r\n"
  // Fin del body: "0\r\n\r\n"
  {
    char hdr[256];
    int hlen = snprintf(hdr, sizeof(hdr),
      "POST /voice HTTP/1.1\r\n"
      "Host: %s:%d\r\n"
      "Content-Type: application/octet-stream\r\n"
      "Transfer-Encoding: chunked\r\n"
      "X-Sample-Rate: %d\r\n"
      "X-Stream: 1\r\n"
      "Connection: close\r\n"
      "\r\n",
      SERVER_HOST, SERVER_PORT, MIC_RATE);
    if (!sock_send_all(fd, (uint8_t *)hdr, hlen)) {
      ESP_LOGE("denver", "Error enviando headers");
      goto done;
    }
  }

  // ── 4. Grabar y enviar en streaming ──────────────────────────────────────
  led_set(1, 0, 0);  // rojo = grabando
  ESP_LOGI("denver", "Grabando + enviando en streaming...");

  while (_recording) {
    size_t got = 0;
    esp_err_t err = i2s_channel_read(
      _mic_chan, _rec_chunk, REC_CHUNK_BYTES, &got, pdMS_TO_TICKS(200));

    if (err != ESP_OK || got == 0) continue;

    const int n_samples = got / 2;

    // Aplicar gain en el chunk antes de enviar
    for (int i = 0; i < n_samples; i++) {
      int32_t s = (int32_t)_rec_chunk[i] * MIC_GAIN;
      _rec_chunk[i] = (int16_t)(s > 32767 ? 32767 : s < -32768 ? -32768 : s);
    }

    if (!sock_send_chunk(fd, (uint8_t *)_rec_chunk, got)) {
      ESP_LOGE("denver", "Error enviando chunk PCM (bytes=%u)", (unsigned)total_bytes);
      goto done;
    }
    total_bytes += got;
  }

  mic_deinit();
  mic_ok = false;

  ESP_LOGI("denver", "Grabación terminada: %.1fs (%u bytes PCM)",
           (float)total_bytes / (MIC_RATE * 2), total_bytes);

  // ── 5. Enviar chunk final → señaliza fin del body al servidor ────────────
  // "0\r\n\r\n" es el terminador estándar de Transfer-Encoding: chunked.
  // El servidor detecta el fin del body sin necesidad de cerrar el socket.
  if (!sock_send_final_chunk(fd)) {
    ESP_LOGW("denver", "Error enviando chunk final");
  }

  // ── 6. Leer respuesta HTTP ────────────────────────────────────────────────
  led_set(0, 0, 1);  // azul = esperando respuesta
  {
    char line[128]; int status = 0;
    sock_read_line(fd, line, sizeof(line));
    sscanf(line, "HTTP/%*s %d", &status);
    if (status != 200) {
      ESP_LOGE("denver", "HTTP %d", status);
      led_set(1, .3f, 0);
      vTaskDelay(pdMS_TO_TICKS(2000));
      goto done;
    }
    // Consumir headers de respuesta
    while (sock_read_line(fd, line, sizeof(line)) > 0) {}
    ESP_LOGI("denver", "HTTP 200 OK — recibiendo WAV");
  }

  // ── 7. Reproducir WAV streaming ───────────────────────────────────────────
  led_set(0, 1, 0);  // verde = reproduciendo
  play_buf = (uint8_t *)heap_caps_aligned_alloc(
    32, PLAY_CHUNK, MALLOC_CAP_INTERNAL | MALLOC_CAP_DMA);
  if (!play_buf) { ESP_LOGE("denver", "Sin RAM para play_buf"); goto done; }

  {
    uint8_t  wav_hdr[WAV_HEADER_SIZE];
    size_t   wav_got  = 0;
    bool     spk_on   = false;
    size_t   total_rx = 0;

    while (true) {
      const size_t http_sz = sock_chunk_size(fd);
      if (!http_sz) break;
      size_t remain = http_sz;

      while (remain > 0) {
        const size_t ask = remain < PLAY_CHUNK ? remain : PLAY_CHUNK;
        if (!sock_read_exact(fd, play_buf, ask)) { remain = 0; break; }
        remain -= ask;

        uint8_t *p = play_buf; int plen = (int)ask;

        // Acumular header WAV (44 bytes) para extraer sample rate
        if (wav_got < WAV_HEADER_SIZE) {
          const size_t need = WAV_HEADER_SIZE - wav_got;
          const size_t copy = (size_t)plen < need ? (size_t)plen : need;
          memcpy(wav_hdr + wav_got, p, copy);
          wav_got += copy; p += copy; plen -= (int)copy;

          if (wav_got == WAV_HEADER_SIZE) {
            uint32_t sr = 0;
            memcpy(&sr, wav_hdr + 24, 4);
            if (!sr || sr > 48000) sr = 22050;
            const uint32_t spk_sr = (sr == 22050 || sr == 44100) ? 16000 : sr;
            ESP_LOGI("denver", "WAV sr=%d→%d", (int)sr, (int)spk_sr);
            spk_init(spk_sr);
            spk_on = true;
          }
        }
        if (spk_on && plen > 0) { play_pcm(p, plen); total_rx += plen; }
      }
      sock_skip_to_lf(fd);
    }

    ESP_LOGI("denver", "Stream reproducido: %d bytes", (int)total_rx);
    vTaskDelay(pdMS_TO_TICKS(300));
    spk_deinit();
  }

done:
  if (mic_ok) mic_deinit();
  if (fd >= 0) ::close(fd);
  if (play_buf) free(play_buf);
  led_set(0, .1f, 0);  // verde tenue = idle
  _denver_busy = false;
  vTaskDelete(nullptr);
}

// ─── Control botón ────────────────────────────────────────────────────────────
static void denver_btn_press() {
  if (_denver_busy && !_recording) { ESP_LOGW("denver", "Ocupado"); return; }
  if (!_recording) {
    if (_denver_busy) return;
    _recording = _denver_busy = true;
    led_set(1, 0, 0);
    ESP_LOGI("denver", "Grabando (streaming)...");
    xTaskCreatePinnedToCore(record_task, "denver", 32768, nullptr, 5, nullptr, 0);
  } else {
    _recording = false;
    ESP_LOGI("denver", "Stop (press)");
  }
}

static void denver_btn_release() {
  if (_recording) { _recording = false; ESP_LOGI("denver", "Stop (release)"); }
}
