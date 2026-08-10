# splitgrade-controller-sensecap

Primary-tier **SenseCAP** controller for an open-source splitgrade darkroom:
touchscreen UI, optional light-sensor assist, and **ESP-NOW** control of the
shared enlarger LED head.

**License: [MIT](LICENSE)** — Copyright © 2026 Dragan Bojovic.

Companion / budget tier (phone over BLE):
[splitgrade-controller-android](https://github.com/Draganito/splitgrade-controller-android)

Shared enlarger-head firmware:
[darkroom-enlarger-head](https://github.com/Draganito/darkroom-enlarger-head)

## What this is

Three PlatformIO projects in one repo (open **each subfolder** as the
PlatformIO root, not this parent folder):

| Folder | Board | Role |
|--------|--------|------|
| `darkroom-sender/` | SenseCAP Indicator D1 (ESP32-S3 + touchscreen) | Operator UI, exposure state machine, dose calibration |
| `darkroom-rp2040/` | SenseCAP Indicator RP2040 | Sensor bridge (TSL2591 / AS7343) over UART to the sender — see `examples/darkroom_bridge/` |
| `darkroom-receiver/` | Second SenseCAP (optional) | Bench stand-in head (full-screen colour, no LEDs) for ESP-NOW testing without a panel |

The real LED head is **not** in this repo — flash
[darkroom-enlarger-head](https://github.com/Draganito/darkroom-enlarger-head)
onto a XIAO ESP32-S3.

`protocol.h` in the sender (and copies in receiver / head firmware) is the
ESP-NOW wire format. **Sender is the source of truth** — if it changes,
re-copy into the head firmware.

## Beta scope

**For:** hobbyists with a SenseCAP Indicator who want wireless splitgrade
control and optional sensor-assisted starting exposures.

**Includes:** ESP-NOW head control, hard/soft workflow, calibration UI,
RP2040 sensor bridge sketch, German/English user docs under `docs/`.

**Not this repo:** Android BLE app, panel Gerbers, Play Store / SenseCAP
factory firmware. Sensor assist is optional — the head works with manual
times alone.

## Requirements

- [PlatformIO Core](https://platformio.org/)
- SenseCAP Indicator D1 (sender); RP2040 co-processor for sensors
- Enlarger head running [darkroom-enlarger-head](https://github.com/Draganito/darkroom-enlarger-head)
- Optional second SenseCAP as `darkroom-receiver` for bench tests

## Build / flash

```bash
cd darkroom-sender
pio run
pio run -t upload
pio device monitor
```

Set `upload_port` / `monitor_port` in each `platformio.ini` to your serial
device. The RP2040 bridge uses a **UF2** flash flow (BOOTSEL) — see
`docs/TECHNICAL_CONCEPT.md` § Build & flash notes.

## Docs

| File | Contents |
|------|----------|
| `docs/TECHNICAL_CONCEPT.md` | Architecture, protocol, calibration math |
| `docs/USER_GUIDE.md` / `docs/BEDIENUNGSANLEITUNG_DE.md` | Operator guide EN / DE |
| `docs/HARDWARE_BRINGUP_NOTES.md` | Bring-up notes |
| `docs/CROSSOVER_EXPLAINED_DE.md` | Cross-channel correction (DE) |

## Status

Primary-tier stack developed and exercised on real SenseCAP hardware;
ESP-NOW path confirmed against the real LED enlarger head. Pair with the
published head firmware and (optionally) the Android budget controller.
