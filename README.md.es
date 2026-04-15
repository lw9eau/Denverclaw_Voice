# Denverclaw Voice - Cliente Atom Echo

Este proyecto implementa un cliente de asistente de voz de alto rendimiento para el **M5Stack Atom Echo** utilizando **ESPHome**. Está diseñado para funcionar con el servidor Denver Voice, proporcionando una experiencia de interacción por voz basada en streaming de baja latencia.

## Características

- **Audio en Streaming**: Utiliza Chunked Transfer-Encoding para enviar el audio PCM directamente al servidor mientras se graba. Esto ahorra ~128KB de RAM en el ESP32 y reduce la latencia inicial.
- **Baja Latencia**: Gestión optimizada de sockets TCP y buffering mínimo.
- **Indicadores de Estado LED**:
  - 🔴 **Rojo**: Grabando / Inicializando.
  - 🔵 **Azul**: Conectando al servidor / Esperando respuesta.
  - 🟢 **Verde**: Reproduciendo respuesta de audio / Inactivo.
- **Configurable**: Cambia fácilmente los ajustes del servidor mediante substituciones de ESPHome o archivos de secretos.

## Integración con el Servidor

Este cliente se comunica con el **Denverclaw Voice Server** utilizando un protocolo de streaming personalizado sobre HTTP/1.1:
- **Petición**: `POST /voice`
- **Cabeceras**:
  - `Transfer-Encoding: chunked`: Permite el envío de audio en tiempo real.
  - `X-Sample-Rate: 16000`: Informa al servidor sobre la frecuencia del audio.
  - `X-Stream: 1`: Indica que se trata de una petición de streaming.
- **Protocolo**: El cliente envía fragmentos PCM de 512 muestras (1KB). El servidor procesa el audio y devuelve una respuesta WAV también en chunks, que el cliente reproduce instantáneamente.

## Requisitos de Hardware

- **M5Stack Atom Echo**: Una interfaz de voz todo-en-uno basada en ESP32 con micrófono, altavoz y LED RGB integrados.

## Primeros Pasos

### 1. Prerrequisitos
- [ESPHome](https://esphome.io/) instalado en tu equipo.
- Una instancia en ejecución del **Denver Voice Server**.

### 2. Configuración
1. Clona este repositorio.
2. Copia `secrets.yaml.example` a `secrets.yaml`.
3. Completa tus credenciales de WiFi y la dirección IP de tu servidor Denver Voice en `secrets.yaml`.
4. (Opcional) Ajusta el `voice_server_port` en `atom-echo-denver.yaml` si es necesario.

### 3. Instalación
Conecta tu Atom Echo mediante USB y ejecuta:

```bash
esphome run atom-echo-denver.yaml
```

## Uso

- **Push-to-Talk**: Mantén presionado el botón principal para grabar. Suéltalo para detener y enviar.
- **Feedback Visual**: Observa el LED para conocer el estado actual (Grabando, Esperando o Reproduciendo).

## Personalización

La lógica principal se encuentra en `denver_voice.h`. Se encarga de:
- Inicialización del micrófono I2S (PDM).
- Inicialización del altavoz I2S (Estándar).
- Streaming HTTP/1.1 mediante sockets crudos.
- Análisis de cabeceras WAV para ajuste dinámico de la frecuencia de muestreo (sample rate).

## Licencia

Este proyecto es de código abierto. Siéntete libre de contribuir o modificarlo para tus propias necesidades.
