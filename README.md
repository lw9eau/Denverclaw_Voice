# Denverclaw Voice - Atom Echo Client

This project implements a high-performance voice assistant client for the **M5Stack Atom Echo** using **ESPHome**. It is designed to work with the Denver Voice Server, providing a low-latency, streaming-based voice interaction experience.

## Features

- **Audio Streaming**: Uses Chunked Transfer-Encoding to stream PCM audio directly to the server as it's recorded. This saves ~128KB of RAM on the ESP32 and reduces initial latency.
- **Low Latency**: Optimized TCP socket handling and minimal buffering.
- **LED Status Indicators**:
  - 🔴 **Red**: Recording / Initializing.
  - 🔵 **Blue**: Connecting to server / Waiting for response.
  - 🟢 **Green**: Playing audio response / Idle.
## Server Integration

This client communicates with the **Denverclaw Voice Server** using a custom streaming protocol over HTTP/1.1:
- **Request**: `POST /voice`
- **Headers**:
  - `Transfer-Encoding: chunked`: Allows streaming audio in real-time.
  - `X-Sample-Rate: 16000`: Informs the server of the audio frequency.
  - `X-Stream: 1`: Signals that this is a streaming request.
- **Protocol**: The client sends 512-sample (1KB) PCM chunks. The server processes the audio and returns a WAV response using Chunked Transfer-Encoding, which the client plays back immediately.

## Hardware Requirements

- **M5Stack Atom Echo**: An all-in-one ESP32-based voice interface with a built-in microphone, speaker, and RGB LED.

## Getting Started

### 1. Prerequisites
- [ESPHome](https://esphome.io/) installed on your machine.
- A running instance of the **Denver Voice Server**.

### 2. Configuration
1. Clone this repository.
2. Copy `secrets.yaml.example` to `secrets.yaml`.
3. Fill in your WiFi credentials and the IP address of your Denver Voice Server in `secrets.yaml`.
4. (Optional) Adjust the `voice_server_port` in `atom-echo-denver.yaml` if necessary.

### 3. Installation
Connect your Atom Echo via USB and run:

```bash
esphome run atom-echo-denver.yaml
```

## Usage

- **Push-to-Talk**: Press and hold the main button to record. Release to stop and send.
- **Visual Feedback**: Watch the LED to know the current state (Recording, Waiting, or Playing).

## Customization

The core logic is contained in `denver_voice.h`. It handles:
- I2S Microphone initialization (PDM).
- I2S Speaker initialization (Standard).
- HTTP/1.1 Streaming via raw sockets.
- WAV header parsing for dynamic sample rate adjustment.

## License

This project is open-source. Feel free to contribute or modify it for your own needs.
