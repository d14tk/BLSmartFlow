# BL SmartFlow – ESPHome edition

Community fork of [DutchDevelop/BLSmartFlow](https://github.com/DutchDevelop/BLSmartFlow): an ESP32 fan controller that reads the nozzle temperature of a Bambu Lab printer over MQTT and drives exhaust or chamber fans along a configurable temperature curve.

This fork adds a **new firmware based on [ESPHome](https://esphome.io)** with native **Home Assistant** support. It runs on the same SmartFlow hardware, keeps the familiar web interface with the fan-curve chart, and can be installed over WiFi on top of the original firmware without opening the case. The original Arduino firmware stays untouched in this repository (see [Original firmware](#original-arduino-firmware)).

## Why this fork

The original project has not been updated since early 2025. On an X1C, the original firmware showed **0 °C and 0 % forever** and the fans never ran, although printer, LAN mode and access code were fine.

The cause: the printer sends roughly **19 KB of JSON per status report**, but the original firmware limits its MQTT buffer to **1024 bytes** (`mqttClient.setBufferSize(1024)`). PubSubClient silently drops every packet larger than its buffer, so not a single temperature value ever reached the controller.

Instead of patching the old code, the firmware was rebuilt on ESPHome, which also brings OTA updates, Home Assistant integration and a maintained network stack.

## What's new

- **Works with large printer reports.** Uses the ESP-IDF MQTT client and reassembles chunked reports up to 64 KB. Tested on an X1C: no dropped reports.
- **Home Assistant** via the native ESPHome API: nozzle temperature, fan speed, printer connection, WiFi signal and uptime as entities. Auto-discovery, no YAML needed in Home Assistant.
- **Same web UI**, now in English and German (follows the browser language), with a connection status, the current operating point on the curve and system info.
- **Fan curve** with 2 to 12 points, or a fixed fan speed. Settings are stored in flash and survive reboots and updates.
- **Fail-safe behaviour.** If the printer stops reporting for 60 s the controller treats the nozzle as cold and the status LED blinks. A missing Home Assistant connection never reboots the device.
- **Upgrade without USB.** The partition layout matches the original firmware, so the new image can be uploaded through the old `/update` endpoint. On first boot it **migrates WiFi, printer settings and your fan curve** from the original firmware's storage.
- **OTA updates** afterwards through ESPHome or Home Assistant.

## Installation

Download the binaries from the [latest release](https://github.com/d14tk/BLSmartFlow/releases/latest):

| File | Use it for |
|---|---|
| `blsmartflow-esphome-<version>.ota.bin` | Upgrading a device that runs the **original** SmartFlow firmware (over WiFi), or later ESPHome OTA updates |
| `blsmartflow-esphome-<version>.factory.bin` | **Fresh install over USB**, e.g. with [ESPHome Web](https://web.esphome.io) |

The release binaries contain **no credentials**. They are built from [`esphome/release.yaml`](esphome/release.yaml).

### A) Upgrade from the original firmware (no USB)

The original firmware accepts firmware uploads at `http://<device-ip>/update`:

```bash
curl -F "update=@blsmartflow-esphome-2.0.0.ota.bin" http://<device-ip>/update
```

The device answers `OK` and reboots into the ESPHome firmware. It reads the old configuration once and rejoins your WiFi with the same printer settings and fan curve. Its hostname becomes `blsmartflow.local`; the IP address usually stays the same (DHCP).

If the migration finds no usable configuration, the device opens the hotspot described below.

### B) Fresh install over USB

1. Open [web.esphome.io](https://web.esphome.io) in Chrome or Edge, connect the board via USB, choose **Install** and select the `factory.bin`.
2. After flashing, ESPHome Web offers to configure WiFi (Improv). Alternatively connect to the hotspot **`BLSmartFlow Setup`** (password `blsmartflow`) and enter your WiFi in the captive portal.

### C) Build it yourself

Requires [ESPHome](https://esphome.io/guides/installing_esphome) 2026.10 or newer.

```bash
cd esphome
cp secrets.yaml.example secrets.yaml   # fill in WiFi, fallback AP password, API key, OTA password
esphome run blflow.yaml
```

`blflow.yaml` enables API encryption and an OTA password. Board pins can be changed through substitutions in [`esphome/common.yaml`](esphome/common.yaml).

## First setup

1. Open `http://blsmartflow.local/` (or the device IP).
2. Under **Settings**, enter the printer IP address, access code and serial number. You find them on the printer under *Settings → WLAN* and *Settings → General*. **LAN Only Mode** must be enabled on the printer.
3. Adjust the fan curve under **Fan curve**: click a point to edit it, add or remove points. Changes are saved immediately.

## Home Assistant

The device is discovered automatically (*Settings → Devices & services → ESPHome*). Click **Configure**. Devices flashed with your own `blflow.yaml` ask for the API encryption key from your `secrets.yaml`.

Release binaries can also be adopted in the ESPHome dashboard add-on (*Adopt*), which lets you add your own API key and OTA password.

A ready-made dashboard card is in [`homeassistant/dashboard-card.yaml`](homeassistant/dashboard-card.yaml).

Entities (default device name `blsmartflow`):

| Entity | Description |
|---|---|
| `sensor.blsmartflow_nozzle_temperature` | Nozzle temperature reported by the printer (°C) |
| `sensor.blsmartflow_fan_speed` | Current fan output (%) |
| `binary_sensor.blsmartflow_printer_connected` | Printer reports arrive (MQTT connected, last report < 60 s) |
| `sensor.blsmartflow_wifi_signal` | WiFi RSSI |
| `sensor.blsmartflow_uptime` | Uptime |

## Hardware

| Function | GPIO |
|---|---|
| Fan output 1 | GPIO17 (LEDC PWM, 1 kHz) |
| Fan output 2 | GPIO16 (same signal) |
| Status LED | GPIO21 (blinks while the printer is not connected) |

Board: ESP32 DevKit (`esp32dev`), 4 MB flash.

## HTTP endpoints

The web UI uses a small JSON API that you can also script against:

| Method | Path | Purpose |
|---|---|---|
| GET | `/sensorData` | Nozzle temperature, fan speed, connection state, report counters |
| GET | `/getFanConfig` | Fan curve points |
| POST | `/updateFanConfig` | Set curve, form field `p=temp:speed,temp:speed,...` |
| GET | `/getOptions` | Printer settings (access code masked) |
| POST | `/submitOptions` | Save printer settings (`ip`, `code`, `serial`, `staticfan`, `staticfanspeed`) |
| GET | `/api/info` | WiFi, IP, uptime, free RAM, MQTT state |
| POST | `/api/restart` | Reboot |

## Notes and limitations

- **Tested on an X1C.** P1 and A1 printers use the same topic and field; they should work but are untested.
- The printer's TLS certificate is self-signed, so the server certificate is not verified (same as the original firmware). Use the controller in your LAN only.
- The release binaries have no API encryption and no OTA password, so anyone in your LAN can update them. Adopt the device in the ESPHome dashboard or build your own `blflow.yaml` to set both.
- The chart library (Chart.js) and the fan graphics are loaded from the internet. Without internet access the controller keeps regulating and the status works, only the chart is missing.

## License and credits

Released under the original project's license, **Creative Commons Attribution-NonCommercial-ShareAlike 4.0 International (CC BY-NC-SA 4.0)**, see [LICENSE](LICENSE). Non-commercial use only; modifications must be shared under the same license.

- **[DutchDeveloper](https://dutchdevelop.com/)**: original BL SmartFlow hardware and firmware
- **[xps3riments](https://github.com/xps3riments)**: inspiration for the foundation of the original code
- **[longrackslabs](https://github.com/longrackslabs)**: original build process, documentation and community support
- **[d14tk](https://github.com/d14tk)**: ESPHome firmware and Home Assistant integration in this fork

---

## Original Arduino firmware

The sections below describe the original PlatformIO/Arduino firmware in `src/`, unchanged from upstream. It contains the MQTT buffer problem described above.

### Features

- Connects to Bambulab H2D, X1, X1C, P1P or P1S
- Controls fans based on printer temperature

### Tools & Libraries Used

- [Visual Studio Code](https://code.visualstudio.com/)
- [PlatformIO](https://platformio.org/)
- [Python](https://www.python.org/)
- [qpdf](https://qpdf.sourceforge.io/) (`gzip`)

### Building and Running the Project

1. Clone the repository to your local machine.
2. Open the project folder in Visual Studio Code with PlatformIO installed.
3. Connect your SmartFlow ESP32.
4. Build and upload with PlatformIO.

Setup instructions for the original firmware: [dutchdevelop.com/blsmartflow](https://www.dutchdevelop.com/blsmartflow).

### Generating .h Files for Compressed HTML

HTML pages in `src/www/` are compressed into `.h` files by `compress_html.py`, which runs as a pre-build step from `platformio.ini`. The generated files are not checked into git.
