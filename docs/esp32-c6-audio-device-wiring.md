# ESP32-C6 Audio Device Configuration

Updated: 2026-06-16 23:03 +08:00

## Device

- Board: MuseLab nanoESP32-C6 / ESP32-C6 development board
- Chip: ESP32-C6 QFN40 rev v0.2
- Flash: 16 MB
- USB serial: CH343 USB-UART
- Serial port: COM5
- Serial baud rate: 115200
- MAC observed by esptool: 9c:cc:01:ff:fe:40:1c:d8

## Firmware

- Project: `F:\project\esp32s3-chatbot`
- Sketch: `esp32s3_wifi_provision/esp32s3_wifi_provision.ino`
- FQBN: `esp32:esp32:esp32c6`
- Flash options used:
  - `FlashSize=16M`
  - `PartitionScheme=app3M_fat9M_16MB`
  - `UploadSpeed=115200`

Current serial commands:

```text
help
admin <http://host:8766>
ws <ws://host:8766/xiaozhi/v1>
token <auth-token>
ask <prompt>
askstream <prompt>
wshello
mictest
speakertest
```

## Server

- Java service project: `F:\project\chatbot-service-java`
- Remote server: `203.195.202.54`
- HTTP base URL: `http://203.195.202.54:8766`
- Xiaozhi WebSocket URL: `ws://203.195.202.54:8766/xiaozhi/v1`
- WebSocket auth header: `Authorization: Bearer <token>`
- Token stored on device: set
- Token note: use `XIAOZHI_WEBSOCKET_TOKEN`; do not commit or print the full token.

Verified WebSocket hello result:

```text
WS connect ws://203.195.202.54:8766/xiaozhi/v1
WebSocket upgraded.
WebSocket server hello:
{"type":"hello","transport":"websocket","session_id":"...","audio_params":{"format":"opus","sample_rate":16000,"channels":1,"frame_duration":60}}
```

## Audio Module

- Module: LMD2718T MEMS microphone + NS4168 I2S amplifier audio module
- Reference manual: local PDF under `F:\download\1778830308809822\`, named `LMD2718+NS4168 audio module manual` in Chinese.
- Microphone format: PDM, pins `CLK` and `DATA`
- Amplifier format: I2S, pins `LRCLK`, `BCLK`, `SDA/SDATA`
- Power: module `VCC` requires 5V according to the manual

## Wiring

| Audio module pin | ESP32-C6 pin | Purpose |
| --- | --- | --- |
| `VCC` | `5V` / `5VIN` | Audio module power |
| `GND` | `GND` | Ground |
| `CLK` | `GPIO20` | LMD2718T PDM microphone clock |
| `DATA` | `GPIO22` | LMD2718T PDM microphone data |
| `LRCLK` | `GPIO19` | NS4168 I2S word select / left-right clock |
| `BCLK` | `GPIO18` | NS4168 I2S bit clock |
| `SDA` / `SDATA` | `GPIO21` | NS4168 I2S serial audio data input |
| White 2-pin speaker socket | Speaker only | Amplifier output; do not connect to ESP32 GPIO |

Do not use these ESP32-C6 pins for the audio module:

- `GPIO9`: BOOT button
- `GPIO8`: onboard RGB LED
- `TX` / `RX`: serial debug path through CH343

## Verified Tests

### Microphone

Command:

```text
mictest
```

Observed after changing module `VCC` to 5V:

```text
PDM mic test: CLK=GPIO20 DATA=GPIO22 rate=48000Hz duration=3000ms
PDM mic samples=17920 chunks=35 min=-32768 max=32767 peak=32768 mean=3.8 rms=1480.2
```

Result: microphone PDM capture is active.

### Speaker

Command:

```text
speakertest
```

Observed:

```text
Speaker test: BCLK=GPIO18 LRCLK=GPIO19 SDATA=GPIO21 rate=16000Hz tone=880Hz duration=1000ms
Speaker test done.
```

Result: speaker test tone was audible.

## Current Baseline

- WiFi connection: OK
- WebSocket auth and hello: OK
- PDM microphone input: OK
- I2S amplifier output: OK
- Speaker connected through the module speaker connector: OK

Next integration step:

1. Capture microphone audio.
2. Send `listen.start` over WebSocket.
3. Send audio frames to `ws://203.195.202.54:8766/xiaozhi/v1`.
4. Send `listen.stop`.
5. Receive server TTS binary frames.
6. Play TTS audio through NS4168.
