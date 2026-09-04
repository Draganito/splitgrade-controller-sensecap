# splitgrade-controller-sensecap

Primary-tier **SenseCAP** controller for an open-source splitgrade darkroom:
touchscreen UI, optional light-sensor assist, and **ESP-NOW** control of the
shared enlarger LED head.

**License: [MIT](LICENSE)** — Copyright © 2026 Dragan Bojovic.

Companion / budget tier (phone over BLE):
[splitgrade-controller-android](https://github.com/Draganito/splitgrade-controller-android)

Shared enlarger-head firmware:
[darkroom-enlarger-head](https://github.com/Draganito/darkroom-enlarger-head)

## Start here (Debian)

Do not install PlatformIO for a first flash. Grab the sender `.bin` and
the RP2040 `.uf2` from
**[Releases](https://github.com/Draganito/splitgrade-controller-sensecap/releases)**
and follow **[FLASH.md](FLASH.md)** (`esptool` on the CH340 port, UF2 copy
onto `RPI-RP2`).

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
RP2040 sensor bridge, German/English user docs under `docs/`, prebuilt
`.bin` / `.uf2` on GitHub Releases.

**Not this repo:** Android BLE app, panel Gerbers, Play Store / SenseCAP
factory firmware. Sensor assist is optional — the head works with manual
times alone.

## Docs

| File | Contents |
|------|----------|
| `FLASH.md` | Beginner flash on Debian (no PlatformIO) |
| `docs/TECHNICAL_CONCEPT.md` | Architecture, protocol, calibration math |
| `docs/USER_GUIDE.md` / `docs/BEDIENUNGSANLEITUNG_DE.md` | Operator guide EN / DE |
| `docs/HARDWARE_BRINGUP_NOTES.md` | Bring-up notes |
| `docs/CROSSOVER_EXPLAINED_DE.md` | Cross-channel correction (DE) |

## Status

Primary-tier stack developed and exercised on real SenseCAP hardware;
ESP-NOW path confirmed against the real LED enlarger head. Pair with the
published head firmware and (optionally) the Android budget controller.
