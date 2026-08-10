# Hardware Bring-Up Notes (SenseCAP Indicator D1/D2)

Hard-won hardware quirks discovered during bring-up on the SenseCAP Indicator
units. These are things that will silently break again if "simplified" or
"cleaned up" by someone who doesn't know why they're there.

For software architecture, protocol, state machines, and exposure math, see
`TECHNICAL_CONCEPT.md` instead — this file is deliberately narrow: hardware
bring-up tribal knowledge only, nothing that duplicates that document.

## Display bring-up

- Use the expander-aware display bus (`Indicator_SWSPI`) for the LCD's
  CS/RST lines. Switching to plain software SPI for CS caused cold-boot
  failures: backlight on, panel blank / no UI at all.
- Keep the retry-based display init (`gfx->begin()` attempted a few times
  with a short delay between tries, see `initDisplay()` in both `main.cpp`
  files) — a single attempt is not reliable enough on cold boot.

## Display flicker on value redraw (2026-07-27)

- Root cause: value text (CAL step count, CAL/LEDCFG result rows, XOVER
  factor, main-screen HARD/SOFT readout, state pill) was updated with a
  two-step "fillRect to erase, then print the new text" sequence. This
  panel's framebuffer is continuously scanned out (no vsync-timed double
  buffer), so the moment between the erase and the new text being drawn is a
  real, visible frame — that gap was the flicker, independent of the
  dirty-flag/partial-redraw discipline in item 4 below (that only limits
  *how often* a redraw happens, not whether each redraw itself flickers).
- Fix: `printOpaque()` in `main.cpp` uses `Arduino_GFX`'s two-argument
  `setTextColor(fg, bg)` ("opaque" mode). `drawChar()` then fills each glyph
  cell's background and draws its foreground pixels in the same write, so
  there's no erased-but-empty state to catch mid-scan. Callers pass text
  pre-padded to the field's max width (`"%-Ns"`) so a shorter new string's
  background-colored space glyphs still cover a longer previous string
  ("ghosting" from an old fillRect-sized clear no longer applies since there
  is no separate clear at all).
- Also split "static chrome" (card frames, outlines, row labels — drawn once
  on `fullRedraw`) from "dynamic values" (redrawn every call via
  `printOpaque()`), instead of refilling the whole card on every touch. Do
  not reintroduce a per-value `fillRect` clear before `print()` — that
  reintroduces exactly this flicker.

## Touch bring-up (sender only; receiver has no touch input)

- On this hardware, the touchscreen controller is FT5x06 at I2C address
  `0x48`.
- At `0x48`, force FT5x06 parsing mode; do not use AUTO format-detection —
  AUTO occasionally mis-parsed CST-format frames, causing split left/right
  touch behavior.
- FT5x06 already reports pixel-like coordinates for this panel — do not add
  a custom 4-point linear scaling calibration on top of that. An earlier
  attempt at this caused half-screen coordinate corruption.
- The anti-phantom-touch strategy in `touch_touched()` (`touch.h`) must be
  preserved: multi-sample read + jitter rejection + a press-edge latch so
  each physical press produces exactly one logical touch event, not several.

## If something regresses, check in this order

1. Display init path still goes through `Indicator_SWSPI` (not plain SPI).
2. Touch at `0x48` still uses the FT5x06 parser, not `TOUCH_READ_AUTO`.
3. Sender and receiver are still both pinned to the same ESP-NOW protocol
   channel (`kEspNowChannel` in `protocol.h`).
4. UI redraw is still partial / dirty-flag-based (`displayDirty`) — a full
   unconditional redraw every loop iteration causes visible flicker once a
   countdown timer is ticking.
5. Value text still uses `printOpaque()` (opaque `setTextColor(fg, bg)`),
   not a `fillRect` erase followed by a plain `print()` — see "Display
   flicker on value redraw" above.
