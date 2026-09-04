# Flash the SenseCAP controller (Debian)

You do **not** need PlatformIO. Download both files from
[Releases](https://github.com/Draganito/splitgrade-controller-sensecap/releases)
and write them onto a **SenseCAP Indicator D1**.

The D1 has **two USB-C ports and two chips**. Flash both.

| Chip | Release file | How you know the port |
| --- | --- | --- |
| ESP32-S3 (touchscreen / ESP-NOW) | `splitgrade-sensecap-sender-0.2.0.bin` | `USB-SERIAL CH340` → usually `/dev/ttyUSB0` |
| RP2040 (sensor bridge, optional) | `splitgrade-sensecap-rp2040-0.2.0.uf2` | `USB Serial Device` → `/dev/ttyACM0`, or a disk named `RPI-RP2` |

The real LED head is a separate board:
[darkroom-enlarger-head](https://github.com/Draganito/darkroom-enlarger-head/releases).

Sensor assist is optional. Manual hard/soft times work with the sender alone;
flash the RP2040 if you use a TSL2591 / AS7343 on the Grove port.

## 1. Tools

```bash
sudo apt install esptool
sudo usermod -aG dialout "$USER"
```

Log out and back in so `dialout` takes effect.

## 2. ESP32-S3 (sender)

Plug USB-C into the **CH340** port (Device Manager / `dmesg` shows
`USB-SERIAL CH340`).

```bash
esptool --chip esp32s3 --port /dev/ttyUSB0 --baud 921600 write_flash 0x0 \
  splitgrade-sensecap-sender-0.2.0.bin
```

If the command is `esptool.py` on your machine, use that name instead.

No extra packages: Chrome/Chromium at
[esptool-js](https://espressif.github.io/esptool-js/) — chip **ESP32-S3**,
address `0x0`, same `.bin`.

## 3. RP2040 (sensor bridge)

1. Unplug the D1.
2. With a pin, **hold the internal BOOT button**, plug USB-C into the
   **RP2040** port, then release BOOT.
3. A disk named **`RPI-RP2`** appears (e.g. `/media/$USER/RPI-RP2/`).
4. Copy the UF2 onto it:

   ```bash
   cp splitgrade-sensecap-rp2040-0.2.0.uf2 /media/$USER/RPI-RP2/
   ```

The disk unmounts by itself after 1–2 seconds. That is success.

## 4. After the first flash

The touchscreen UI should boot. Operator steps:
[USER_GUIDE.md](docs/USER_GUIDE.md).

Set the LED head from the hidden **LEDCFG** screen (hold **MEAS LIT** 3
seconds on the main screen, then **SAVE**):

| Panel | LEDs | Data GPIO |
| --- | --- | --- |
| 4″×5″ (`darkroom-led-panel-4x5`) | 99 | GPIO **5** (XIAO pin **D4**) |
| MILUKA Aristo D2 (109 SK6812) | 109 | GPIO **5** (XIAO pin **D4**) |

GPIO is whichever XIAO pin you wired to the panel **DATA** pad.

## Build from source (optional)

Open **each subfolder** as the PlatformIO root, not this parent:

```bash
cd darkroom-sender && pio run -t upload
cd ../darkroom-rp2040 && pio run   # then copy the .uf2 as in §3
```

`darkroom-receiver/` is a bench stand-in (second SenseCAP, no LEDs). Skip
it unless you are testing ESP-NOW without a panel.
