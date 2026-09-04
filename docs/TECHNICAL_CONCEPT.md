# Splitgrade Controller — Technical Concept & Implementation Notes

This project is the **primary-tier** SenseCAP reference implementation
(ESP-NOW + optional sensor metering). Budget tier (Android / BLE):
[splitgrade-controller-android](https://github.com/Draganito/splitgrade-controller-android).
Real LED head:
[darkroom-enlarger-head](https://github.com/Draganito/darkroom-enlarger-head).

This document captures the conceptual model, math, state machine, and design decisions behind the current implementation (`darkroom-sender`, `darkroom-rp2040`, `darkroom-receiver`). It is the reference for anyone extending or debugging the system.

---

## 1) System architecture

Three physical units, wireless-first:

1. **`darkroom-sender`** (ESP32-S3, SenseCAP D1, touchscreen) — the operator-facing controller. Owns UI, exposure state machine, and the dose-based calibration model (§5).
2. **`darkroom-rp2040`** — a light-metering bridge (TSL2591 lux sensor and/or AS7343 spectral sensor) connected to the sender over UART. Provides a single "metric" value per read request. *(The bridge firmware already goes a bit further than that single metric suggests: it runs simple auto-gain on the TSL2591 — one automatic retry at a lower gain on saturation, one step up in gain queued for the next cycle on a weak signal — and reads AS7343 channels F3/475nm ("blue") and F4/515nm ("green"), matching the peak wavelengths of the SK6812RGBW-NW LEDs used on the real enlarger head. This is purely informational: the sender currently still only consumes a single averaged metric from AS7343 as a fallback (see §6), so none of this changes today's behavior — it just means the wiring/firmware groundwork for per-channel use is already in place on the bridge side.)*
3. **`darkroom-receiver`** (ESP32-S3, SenseCAP D2, simulates the enlarger head) — receives exposure commands over ESP-NOW and runs an autonomous countdown, driving colored full-screen output (blue = HARD, green = SOFT, white = FOCUS) as a stand-in for lamp output. It also accepts two steady, non-timed "meter" commands (`CMD_METER_BLUE_ON`/`CMD_METER_GREEN_ON`) that just hold one color on until `CMD_STOP`, used for the dose calibration/metering workflow in §5. Kept running unchanged as a bench-test stand-in — the real LED head (separate `darkroom-enlarger-head` project) can listen to the same broadcasts simultaneously.

```
[Sender touchscreen] <--UART/COBS--> [RP2040 + sensor]
        |
        | ESP-NOW (fire-and-forget, broadcast + 2 known peers)
        v
[Receiver (simulated head) AND/OR darkroom-enlarger-head (real SK6812RGBW-NW head, separate project)]
```

### Why fire-and-forget ESP-NOW

- No pairing handshake required at runtime; every command is broadcast plus sent to two known MAC addresses.
- The receiver is **autonomous**: once it accepts a command, its own `millis()`-based timer drives the exposure to completion even if the sender loses power or Wi-Fi connectivity immediately after sending.
- Trade-off: no delivery acknowledgment. A single dropped `CMD_STOP` or `CMD_EXPOSE_*` packet is not retried. Given three simultaneous transmission attempts (broadcast + 2 peers) over a very short physical distance, this is an accepted risk for the current stage of the project — revisit if real-world drop rates become an issue during regular use.

---

## 2) Protocol (`protocol.h`, shared by sender and receiver)

Fixed 10-byte packet:

```
[magic0][magic1][version][cmd][arg0:2][arg1:2][crc16:2]
```

- `magic0/magic1` — project signature, rejects unrelated ESP-NOW traffic on the same channel.
- `version` — protocol version guard; mismatches are rejected outright, no auto-negotiation.
- `cmd` — one of `CMD_PAIR`, `CMD_FOCUS_ON`, `CMD_METER_BLUE_ON`, `CMD_METER_GREEN_ON`, `CMD_EXPOSE_HARD`, `CMD_EXPOSE_SOFT`, `CMD_EXPOSE_SPLIT`, `CMD_STOP`.
- `arg0` / `arg1` — time values in **half-second units** (`kMaxHalfSeconds = 1998` → 999.0 s ceiling per phase).
- `crc16` — CRC-16/CCITT over the preceding bytes; receiver drops any packet that fails magic, version, or CRC checks (`validatePacket`).

All timing throughout both firmwares uses half-second integer units to avoid floating-point drift in the countdown path; conversion to seconds only happens at display time.

---

## 3) Receiver state machine and command acceptance rules

`RunMode`: `MODE_IDLE, MODE_FOCUS, MODE_METER_BLUE, MODE_METER_GREEN, MODE_HARD, MODE_SOFT, MODE_SPLIT_HARD, MODE_SPLIT_SOFT`.

Acceptance rules per command (`handleCommand`):

- `CMD_EXPOSE_HARD` / `CMD_EXPOSE_SOFT` / `CMD_EXPOSE_SPLIT`: only accepted if `runMode` is none of `MODE_FOCUS`, `MODE_METER_BLUE`, `MODE_METER_GREEN`, and argument(s) are within `1..kMaxHalfSeconds`. This prevents an exposure command from silently overriding an active focus or metering session.
- `CMD_FOCUS_ON`: only accepted if `runMode == MODE_IDLE || runMode == MODE_FOCUS`. **This guard was added specifically to close a safety gap**: previously `CMD_FOCUS_ON` had no guard at all and could interrupt an in-progress `HARD`/`SOFT`/`SPLIT_*` exposure, switching the (future) enlarger head to full white light mid-exposure without ever stopping the original exposure cleanly. The fix makes focus-interruption symmetric with expose-interruption: neither state can silently clobber the other; an explicit `CMD_STOP` is required first.
- `CMD_METER_BLUE_ON` / `CMD_METER_GREEN_ON`: only accepted if `runMode == MODE_IDLE` or `runMode` is already one of the two meter modes (so switching directly from blue to green metering, or re-issuing the same one, is fine). Never accepted while a timed exposure (`HARD/SOFT/SPLIT_*`) or `FOCUS` is running — same symmetry principle as above. These are steady, non-timed states: the light stays on with no countdown until an explicit `CMD_STOP`, used by the sender's `MEAS BLK`/`MEAS LIT` actions (§5) to take a single-color sensor reading.
- `CMD_STOP`: always accepted, unconditionally returns to `MODE_IDLE`. This is the universal safety valve.

Split exposure sequencing (`updateTimer`) is a simple two-phase countdown: `MODE_SPLIT_HARD` counts down `remainingHalfSeconds`, and on reaching zero with `splitSoftHalfSeconds > 0`, transitions to `MODE_SPLIT_SOFT` with the second phase's duration. No re-synchronization signal is sent back to the sender; both sides run independent, self-contained timers seeded from the same command payload.

---

## 4) Sender state machine

`UiScreen`: `UI_MAIN, UI_CAL, UI_XOVER, UI_LEDCFG` (navigation only, does not gate exposure logic by itself). The former hidden `UI_SETTINGS` placeholder screen was removed — `UI_CAL`, `UI_XOVER`, and `UI_LEDCFG` are the three "hidden" screens, each reached via its own 3-second hold gesture (see below): two-channel dose calibration, optional cross-factor tuning (§5.4), and field-configurable LED panel geometry (§12) respectively.

`RunState`: `RUN_IDLE, RUN_FOCUS, RUN_HARD, RUN_SOFT, RUN_SPLIT_HARD, RUN_SPLIT_SOFT` — mirrors the receiver's model on the sender side, driving its own local countdown/display and audible beep cadence, independently of the receiver's timer (both are seeded from the same computed values at the moment `beginExposure()`/`sendCommand()` fire).

Button-level state guards worth documenting explicitly:

- **`EXPOSURE`** is the universal abort: if `runState != RUN_IDLE` when pressed, it always calls `stopAll()` (sends `CMD_STOP`) **immediately on press**, exactly as before — this path is completely untouched by the point below, so aborting an active exposure has zero added latency. Only while `runState == RUN_IDLE` does a press instead become a hold-candidate: a short tap (release before 3 s) starts the exposure via `doStartExposure()` on release; holding for `kExposureHoldMs` (3 s) instead opens the hidden `UI_XOVER` screen (§5.4) without starting anything. Because the hold-candidate is only ever armed while idle, this cannot delay or interfere with the abort case.
- **`FOCUS`**: if any exposure is active (`RUN_HARD/SOFT/SPLIT_*`) or focus is already on, pressing `FOCUS` calls `stopAll()` first. A **second** press (once state is `RUN_IDLE`) is required to actually enter focus mode. This mirrors the receiver-side guard above and was added for the same safety reason — no accidental full-light interruption of a running exposure from a single mis-tap.
- **`MEAS BLK`** (top-left, main screen) — short tap meters blue light and recomputes `hardHalf` (§5.3); a 3-second hold instead opens the hidden `UI_CAL` screen. Both paths are gated on `runState == RUN_IDLE` — metering and calibration entry are deliberately disallowed while any exposure or focus session is active, since the measurement would either be meaningless (light already doing something else) or contaminated.
- **`MEAS LIT`** (bottom-left, main screen) — short tap meters green light and recomputes `softHalf` (§5.3); a 3-second hold instead opens the hidden `UI_LEDCFG` screen (§12). Both paths are gated on `runState == RUN_IDLE`, same rationale as `MEAS BLK` above.

---

## 5) The exposure math model — dose-based, two independent channels

This replaces the earlier single-measurement-plus-crossover-heuristic model. It is calibrated once per paper/developer combination using a **Stouffer stepwedge** (21 steps, 1/2 stop per step in the wedge currently used — see `kStepwedgeSteps`/`kStepwedgeStopsPerStep`), and used daily via two independent, direct measurements — no crossover term, no AUTO/MANUAL mode distinction. Every measurement is already a fresh absolute reading of the color light in question, so nothing needs "relative rescaling" against a prior baseline.

**Physical precondition (important, see §9):** the TSL2591 is a broadband sensor — it cannot distinguish blue from green light. Every measurement in this model (calibration and daily use alike) is only valid if exactly one color light is active for the whole duration of that particular reading. `CMD_METER_BLUE_ON`/`CMD_METER_GREEN_ON` exist specifically to guarantee this on the receiver/enlarger-head side; since the calibration test print (§5.2) is still exposed manually by the user at the real enlarger rather than driven by `CMD_EXPOSE_*`, its color must additionally be ensured manually.

### 5.1 Data model

Two persisted values, one per channel, replace the old three-value paper profile:

| Value | Variable | Preferences key | Meaning |
|---|---|---|---|
| Blue target dose | `calDoseBlue` | `cal_dose_b` | Target dose (lux·s) for the blue (hard/contrast) channel, found via calibration §5.2. `-1` = not calibrated yet. |
| Green target dose | `calDoseGreen` | `cal_dose_g` | Target dose (lux·s) for the green (soft/tone) channel, found the same way. `-1` = not calibrated yet. |

Transient calibration inputs on the `UI_CAL` screen (not persisted, re-entered per session — same philosophy as the old screen's intermediate values): `calRefIntensityBlue`/`calRefIntensityGreen` (captured via `READ REF`), `calStepBlue` (default 3), `calStepGreen` (default 18), `calTestHalfSeconds` (shared, default 20 = 10.0 s).

### 5.2 Calibration — finding the target dose per channel (`computeTargetDose`)

Practical workflow per channel (blue and green calibrated independently, same steps):

1. Insert the stepwedge in the enlarger's negative stage.
2. On `UI_CAL`, press **`READ REF`** for that channel — this calls the same `fetchFreshSensorMetric()` used elsewhere, capturing the enlarger's current single-color light intensity into `calRefIntensityBlue`/`Green`.
3. Manually expose a real test print through the wedge, for the fixed **`calTestHalfSeconds`** duration shown on screen (today's simulated-receiver hardware cannot drive this test exposure itself — no real lamp head is attached yet, see §10).
4. Develop the print, find the step number where the tone first reaches the desired reference (e.g. first solid black, or first visible grey) and dial that step into the **`STEP +/-`** control for that channel.
5. Press **`SAVE`** for that channel. This computes and persists the target dose:

```
lightFactor  = 2 ^ kStepwedgeStopsPerStep            // light factor per wedge step
basisSeconds = testSeconds / lightFactor^(stepFound - 1)   // time equivalent to step 1
targetDose   = calibIntensity * basisSeconds         // stored as calDoseBlue/Green
```

Worked example (matches the formula's verification case): a 10 s test exposure where the wedge's step 3 is the first solid black gives `basisSeconds = 10 / (2^0.5)^2 = 5.0 s` — the equivalent exposure time for the wedge's lightest (step 1) reference density at that same light intensity. `targetDose` is this basis time multiplied by the intensity reading, so it is comparable across different test exposure times or light levels used in later re-calibrations.

### 5.3 Daily use — direct metering (`meterAtColor`, `doseToHalfSeconds`)

No profile lookup, no relative scaling — each of the two main-screen actions is a complete, self-contained measurement:

1. **`MEAS BLK`**: sends `CMD_METER_BLUE_ON`, waits briefly (`kMeterSettleMs`) for the light to settle, takes one `fetchFreshSensorMetric()` reading, sends `CMD_STOP` (`meterAtColor()`). Then:
   ```
   hardHalf = round(calDoseBlue / measuredIntensity * 2), clamped to [0, kMaxHalfSeconds]
   ```
2. **`MEAS LIT`**: identical flow with `CMD_METER_GREEN_ON`, producing `softHalf` from `calDoseGreen`.
3. If the corresponding channel isn't calibrated yet (`calDoseBlue`/`calDoseGreen <= 0`) or the measurement fails, the existing `hardHalf`/`softHalf` value is left untouched and a distinct failure beep sounds instead — the controller never silently zeroes a working time.

`EXPOSURE` then uses `hardHalf`/`softHalf` as measured, modified only by the optional cross factor below — no other compensation step. This is the deliberate design choice discussed and confirmed during the model's design: the old crossover heuristic was a compensation for measuring only one point and *guessing* the other from it; with two independently measured, independently calibrated channels, that guess is no longer needed.

### 5.4 Optional cross-factor correction (`crossFactorPermille`, hidden `UI_XOVER` screen)

**Rationale, verified against darkroom/imaging-science literature, not just physical intuition** (also documented in §9): VC paper's contrast mechanism relies on multiple emulsion parts that share the *same* blue sensitivity but differ only in *added* green sensitivity — confirmed independently by Ilford's own technical notes (quoted in the RPS article "The Science Behind Ilford Multigrade": *"They also all have the same speed to blue light"*), by Darkroom Automation's VC-paper application note (*"The first emulsion is sensitive only to blue light. The second emulsion is sensitive to both blue and green light"*), and by Agfa's VC-paper patent literature (US 4987063). The practical, named consequence — documented independently by Lambrecht & Woodhouse ("Way Beyond Monochrome"), by real-world printers on darkroom forums (e.g. Photrio's "Split grade printing: the key to success"), and analyzed explicitly by Corsi Foto Analogica's "SplitGrade printing is... flawed" — is that **the hard/blue exposure also lands on the highlight-controlling soft/green-sensitized layer**, adding unwanted extra density to the highlights, not the other way around: the pure-blue layer has no green sensitizer at all, so a soft/green exposure genuinely cannot expose it. This is the *opposite* direction from an earlier, physically unverified assumption used during this feature's initial design — corrected here based on that literature check.
- **Value**: `crossFactorPermille` (`int16_t`, -300..+300, i.e. -0.30..+0.30), persisted as `cross_perm`. Default `0` — bit-for-bit identical to the base model's behavior until a user deliberately changes it. **Sign convention**: positive (the documented, expected-to-be-useful direction) reduces SOFT by a fraction of HARD; negative reduces HARD by a fraction of SOFT instead — kept available only in case real-world testing on a specific paper/light source combination ever shows a need for that reverse direction (§9, point 4).
- **Formula** (`applyCrossFactor()`), applied fresh at the moment `EXPOSURE` is pressed (never baked into `hardHalf`/`softHalf` or displayed anywhere before that point, to avoid reintroducing the old "displayed vs. stored" duality bug the AUTO/MANUAL model suffered from):
  ```
  if crossFactorPermille >= 0:
    reduction     = round(hardHalf * crossFactorPermille / 1000)
    correctedSoft = max(0, softHalf - reduction);  correctedHard = hardHalf
  else:
    reduction     = round(softHalf * -crossFactorPermille / 1000)
    correctedHard = max(0, hardHalf - reduction);  correctedSoft = softHalf
  ```
  The corrected pair is what actually gets sent to the receiver (`CMD_EXPOSE_*`) and is what the sender's own countdown (`beginExposure()`) displays for that run — both always match. The static `EXP HARD`/`EXP SOFT` figures on the main screen (before pressing `EXPOSURE`) intentionally keep showing the raw, *measured* values at all times; only the transmitted/counted-down durations for a specific exposure are corrected.
- **Finding the value in the darkroom**: `crossFactorPermille` is not derived from a formula — it is found empirically per paper/developer combination by making real test prints, the same way Lambrecht & Woodhouse describe manually reducing the soft exposure "by feel" before a grade-5 pass. Meter a negative needing a long hard/blue exposure, print with `crossFactorPermille = 0`, then repeat with increasing positive values (e.g. `0.10`, `0.15`) until the highlight rendering in the print matches expectations again. Once found, it is stable for that paper and reused going forward.
- **UI access**: hidden `UI_XOVER` screen, reached by a 3-second hold on `EXPOSURE` while `runState == RUN_IDLE` (see §4, §7). `+`/`-` buttons adjust in steps of `kCrossFactorStepPermille = 10` (0.01) across the full signed range, with the same press-and-hold repeat acceleration as every other stepper in this UI (§7), saving on every change (`saveCrossFactor()`). The screen also shows which direction is currently active ("HARD reduces SOFT" / "SOFT reduces HARD" / "OFF"). `BACK` returns to `UI_MAIN`.

---

## 6) Sensor measurement and display-light isolation

`fetchFreshSensorMetric()`:

1. Records current backlight duty, then forces backlight to `0` and waits `1000 ms` before triggering the sensor sample — the panel's own light must not contaminate an optical reading of the print/negative area.
2. Sends `PKT_TYPE_CMD_EXT_SAMPLE_NOW` to the RP2040 bridge and polls for up to `1400 ms` for a fresh, valid sample. **Sensor priority is fixed, not "whichever reported last":** `TSL2591` lux is always preferred when present (its on-bridge auto-gain makes it reliably usable without spectral calibration, and the ratio-based exposure math in §5 doesn't need one); the `AS7343` blue/green mix is only used as a fallback when the bridge reports no TSL2591 reading. This matters because the bridge sends both readings every cycle whenever both sensors are attached — without an explicit priority, whichever sensor's packet happens to arrive last would silently "win" (in practice always AS7343, since it's sent second on the bridge). See `fetchFreshSensorMetric()`.
3. Restores the backlight duty **on every exit path**, including the timeout path — this was explicitly verified: the timeout branch (`return false` after the polling loop) restores backlight before returning, so a sensor/cable fault cannot leave the display permanently blacked out. The only path that does not touch backlight at all is the very first guard (`!rpLinkReady`), which is safe because backlight was never turned off in that case.
4. This call is **blocking** (up to ~2.4 s worst case) and is only ever invoked from paths gated on `runState == RUN_IDLE`, so it never interrupts or delays an active print exposure.

`meterAtColor(meterOnCmd, metricOut)` (§5.3) is a thin wrapper around this same function for the daily-use color measurements: it sends the relevant `CMD_METER_BLUE_ON`/`CMD_METER_GREEN_ON`, waits `kMeterSettleMs` for the light to settle, calls `fetchFreshSensorMetric()` unchanged, then sends `CMD_STOP`. The calibration screen's `READ REF` action calls `fetchFreshSensorMetric()` directly instead, without commanding any receiver light — during calibration the actual single-color light source is the real enlarger (manually ensured by the user), not the simulated receiver.

### Backlight duty double-scaling

`setBacklight(duty)` always applies a fixed `kBacklightDimPercent = 10` multiplier on top of whatever `duty` is passed in. To achieve an effective ~10% panel brightness in normal operation, `loadPrefs()` therefore sets `backlightDuty = 255` (full scale) rather than a pre-dimmed value — the actual dimming happens once, inside `setBacklight()`. Any future change to the backlight brightness policy must account for this single point of scaling to avoid re-introducing a double-dimming bug (previously effective ~1% instead of the intended ~10%).

---

## 7) Touch input handling design

The touch layer implements three overlapping concerns with a single `handleTouch()` state machine, driven by raw touch samples (`touch_touched()`, `touch_last_x/y`):

1. **Hold-candidate detection** for two buttons, each independently tracked: `MEAS BLK` (3 s → opens hidden `UI_CAL`, via `measBlkHoldCandidate`/`measBlkHoldTriggered`) and `EXPOSURE` (3 s → opens hidden `UI_XOVER`, via `exposureHoldCandidate`/`exposureHoldTriggered`, §5.4). `MEAS LIT` needs no hold gesture at all — with the crossover/relative-rescale model gone, every measurement is already complete and absolute on a single short tap, so there is nothing left for a "hold" to do differently. `EXPOSURE`'s hold-candidate is only ever armed while `runState == RUN_IDLE` (checked at touch-down); if a press lands on `EXPOSURE` while an exposure/focus is already running, it is never a hold-candidate and falls straight through to the instant-abort dispatch in `handleMainTouch()` — see §4.
2. **Jitter tolerance**: a small movement tolerance (`kTouchHoldMoveTol`) is allowed before cancelling either hold, so typical resistive/capacitive touch noise doesn't accidentally cancel a long-press.
3. **Deferred short-tap action**: for both hold-candidate buttons, the short-tap action fires on *release* rather than on press (`doMeasureBlue()`/`doStartExposure()`), so the 3-second hold window has something to preempt. This is a deliberate, small (typically sub-300 ms) deferral versus the old instant-on-press style used everywhere else — accepted for `EXPOSURE`'s *start* path specifically because the abort path (running → idle) is never deferred (point 1 above).
4. **Press-and-hold repeat acceleration** for all `+`/`-` steppers (main screen hard/soft nudges, the `UI_CAL` step/test-time steppers, and the `UI_XOVER` cross-factor stepper), via `startHoldRepeat`/`updateHoldRepeatWhilePressed`/`performHoldRepeatStep`: after an initial `kHoldStartDelayMs` delay, repeat interval shortens in three tiers (`slow → fast → turbo`) the longer the button is held, allowing both fine single-taps and fast bulk adjustment from the same control.

A 120 ms global touch-action debounce (`lastTouchActionMs`) prevents a single physical tap from being read as multiple logical taps on noisy touch hardware.

---

## 8) Persistence model

Stored via `Preferences` under namespace `"darkroom"`:

| Key | Content | Persisted from |
|---|---|---|
| `hard_half`, `soft_half` | current working exposure times | every adjustment, and every successful `MEAS BLK`/`MEAS LIT` (`saveExposurePrefs`) |
| `cal_dose_b`, `cal_dose_g` | per-channel target dose (blue/green, lux·s) | `SAVE` on the respective `UI_CAL` channel only |
| `cross_perm` | optional cross-factor, signed permille (-300..+300 = -0.30..+0.30), §5.4 | every `+`/`-` tap on `UI_XOVER` (`saveCrossFactor`) |
| `led_count`, `led_pin` | sender's local copy of the last-set/last-sent LED panel geometry, §12 | every edit on `UI_LEDCFG` (`saveLedConfig`) — note: the receiving board (the real LED head, `darkroom-enlarger-head`) separately persists its own copy of the same two values in its own `Preferences`, and is the actual authority on which one is active (§12) |
| `t_swap`, `t_flipx`, `t_flipy` | touch orientation | never actually written (see note below) |

**Not persisted** (intentionally runtime-only, re-entered per session): `calRefIntensityBlue`/`Green`, `calStepBlue`/`Green`, `calTestHalfSeconds`. Unlike the old model, there is no runtime-only *measurement anchor* left to track at all (no `workRefMetric` equivalent) — every `MEAS BLK`/`MEAS LIT` reading is used immediately and completely on its own, nothing carries over between measurements.

**Removed as part of the dose model migration:** `auto_mode`, `cal_read`, `cal_xov`, `cal_ref_m` are no longer read or written. No migration was performed for old values already in flash from a previous firmware version — they simply become inert, unread bytes in `Preferences` storage. `saveTouchOrientation()` (previously flagged as dead code — never called from any UI path) was removed outright during this refactor; `applyTouchOrientation()`'s `prefs.getBool(key, <default>)` calls still fall through to the compiled-in default from `touch_set_orientation_defaults(kDisplayRotation)`, unchanged.

---

## 9) Known limitations / accepted trade-offs (as of current state)

1. **No ACK/retry on ESP-NOW.** A dropped `CMD_STOP` or `CMD_EXPOSE_*` is not retried. Acceptable at short range with triple-transmission (broadcast + 2 peers), but worth revisiting for the real enlarger head (`darkroom-enlarger-head`), especially for `CMD_STOP`.
2. **Broadband sensor requires strictly sequential single-color metering.** The TSL2591 cannot distinguish blue from green light by itself — every reading in the dose model (§5) is only meaningful while exactly one color is active for its whole duration. `CMD_METER_BLUE_ON`/`CMD_METER_GREEN_ON` plus the receiver's mode guards (§3) enforce this on the (simulated or real) enlarger-head side; there is no independent hardware check that some *other* stray light source isn't also present.
3. **Manual color-light selection during calibration.** The calibration test print (§5.2) is exposed manually by the user at their real enlarger — the real head (`darkroom-enlarger-head`) implements the same `CMD_EXPOSE_*`/`CMD_METER_*_ON` handling as the simulated receiver and is confirmed working on real hardware, but automating the test-print exposure itself (§10) hasn't been done yet, so ensure the correct single color manually if in doubt.
4. **Cross-channel correction is optional, manual, and empirically tuned (§5.4).** The two channels (blue/green) are still calibrated and measured fully independently; `crossFactorPermille` defaults to `0` (no effect) until a user deliberately tunes it via real test prints on `UI_XOVER`. Its sign selects the correction direction: positive (the direction confirmed against darkroom/imaging-science literature, §5.4) reduces soft by a fraction of hard; negative is kept available only for the unlikely case that a specific real paper/light source combination needs the reverse. It intentionally does not correct for e.g. paper reciprocity differences between the two test exposures, nor is it derived from any spectral/sensor model — it is a single empirical knob for the one systematic, named effect (the hard/blue pass also exposing the highlight-controlling layer) that real-world split-grade printing is documented to need.
5. **Blocking measurement call** (~1.4–2.4 s, plus `kMeterSettleMs`) freezes the sender's UI/loop during `MEAS BLK`/`MEAS LIT`/`READ REF`. Acceptable since it's always a deliberate, `RUN_IDLE`-gated action, but would need to become asynchronous if future features require touch responsiveness during measurement.

---

## 10) Roadmap context

- **Real LED head — done, on `darkroom-enlarger-head`.** The real enlarger head is a standard ESP32-S3 (e.g. Seeed XIAO ESP32-S3) running the separate `darkroom-enlarger-head` project against this project's own SK6812RGBW-NW panel design, speaking the exact same ESP-NOW protocol as `darkroom-receiver` — no changes were needed in `darkroom-sender` or `darkroom-receiver` to support it, and both can run simultaneously against the same sender broadcasts. Confirmed working on real hardware, including an intermittent LED-dimming glitch traced to the `Adafruit_NeoPixel` driver and fixed by migrating to `NeoPixelBus`/RMT (see `darkroom-enlarger-head`'s own `MEMORY.md`).
- **Automating the calibration test exposure**: `UI_CAL`'s manual "expose a test print yourself" step (§5.2) could optionally be replaced by the controller driving the actual test exposure through `CMD_EXPOSE_HARD/SOFT`, removing the last manual-light-source dependency from the workflow. Out of scope for the current milestone (see the model's explicit non-goals).
- **Per-channel sensor use**: the bridge already reads AS7343 F3 (blue) / F4 (green) separately and runs TSL2591 auto-gain (see §1, point 2) — this groundwork exists specifically so that, once the real head is in daily use, the sender could be extended to cross-check the TSL2591 dose readings against the AS7343's spectrally-resolved channels if that ever proves useful, instead of using AS7343 purely as a same-metric fallback as today.
- **Field-configurable panel geometry — done.** Pixel count and data pin are now adjustable at runtime from the sender's hidden `UI_LEDCFG` screen (§12) via `CMD_SET_LED_CONFIG`, with the receiving board itself persisting and applying the change. Data pin was originally a toggle between two fixed outputs, then changed (2026-07-27) to free +/- entry so the panel could be tested against arbitrary receiver hardware (confirmed against `darkroom-enlarger-head`'s XIAO ESP32-S3 receiver). This was added specifically so other open-source builders using a different-sized SK6812RGBW-NW panel, a different data GPIO, or a different receiver board altogether don't need to touch the firmware source at all.

---

## 11) Build & flash notes

### ESP32-S3 (`darkroom-sender`, `darkroom-receiver`)

Standard `pio run -t upload` from each project folder, over `/dev/ttyUSB0` (see `platformio.ini`). No permission issues have been seen on this system for either ESP32-S3 board — regular USB-serial upload just works. (The real LED head, `darkroom-enlarger-head`, is a separate project with its own build/flash notes.)

### RP2040 (`darkroom-rp2040`)

`pio run -t upload` targets `/dev/ttyACM0`, but on this system it has failed with `No accessible RP-series devices... Maybe try 'sudo'` — a local **udev permission issue**, not a firmware or wiring problem. Rather than chase udev rules, the RP2040's built-in **BOOTSEL/UF2 mass-storage bootloader** is a reliable, permission-issue-proof fallback:

1. `pio run` (no `-t upload`) in `darkroom-rp2040/` builds the firmware image at `.pio/build/sensecap_rp2040_bridge/firmware.uf2`.
2. Put the board into BOOTSEL mode, either:
   - physically: hold the **BOOTSEL** button while plugging in the USB cable, or
   - in software: open `/dev/ttyACM0` at **1200 baud** and close it again — the `earlephilhower` Arduino-Pico core (used by this project, see `platformio.ini`) watches for this "1200-baud touch" and reboots itself into the bootloader automatically, no physical button press needed.
3. The board (re-)enumerates as a plain USB mass-storage drive named **`RPI-RP2`** (e.g. mounted at `/media/<user>/RPI-RP2/` on Linux).
4. Copy the `.uf2` file onto that mount point (drag-and-drop or `cp`). The board flashes itself and reboots running the new firmware within about 1-2 seconds; the mass-storage drive then disappears on its own.

**Gotcha when scripting this**: immediately after the drive is detected, the mount can still be settling — a copy attempted in that instant can fail (e.g. `cp: ... Not a directory`) even though the drive is visible. When automating, poll until `os.path.isdir(mount_point)` is true **and** `os.listdir(mount_point)` succeeds before copying, instead of copying on first detection.

---

## 12) Field-configurable LED panel geometry (`CMD_SET_LED_CONFIG`, hidden `UI_LEDCFG` screen)

Since this project is open-source, someone else building their own real LED head (see `darkroom-enlarger-head`) may wire up a SK6812RGBW panel with a different pixel count than the 109-pixel reference panel (MILUKA Aristo D2, in `kicad-mcp/contrib`), or use a different data GPIO. Rather than hard-coding that, it is configurable at runtime — no re-flash needed.

- **Access**: a 3-second hold on **`MEAS LIT`** while idle on `UI_MAIN` opens the hidden `UI_LEDCFG` screen (exactly the same hold-candidate pattern already used for `MEAS BLK` → `UI_CAL` and `EXPOSURE` → `UI_XOVER`, see §4). A short tap on `MEAS LIT` still does its normal job (metering green light, §5.3) — unchanged.
- **Two settings only**, deliberately minimal:
  - **Pixel count** (1-500, `kLedConfigMinPixels`/`kLedConfigMaxPixels` in `protocol.h`) via the same +/- with hold-to-accelerate control used elsewhere in this UI.
  - **Data pin**: freely +/- adjustable (`kLedConfigMinPin`/`kLedConfigMaxPin` = 0-48 in `protocol.h`), same control style as pixel count. Originally a toggle locked to two known-good factory outputs on the original receiver hardware — changed to free entry (2026-07-27) so the panel could be tested against arbitrary receiver hardware (confirmed against `darkroom-enlarger-head`'s XIAO ESP32-S3 receiver, whose working data pin is GPIO5). `CMD_SET_LED_CONFIG` only ever configures a *remote* board over ESP-NOW, so a bad value can't brick the sender's own boot; avoiding a strapping pin or reserved flash/PSRAM pin on whichever board receives the config is the user's responsibility, same as it already was for the BLE app's config-write path, which has never restricted the pin.
- **Protocol**: `CMD_SET_LED_CONFIG` (`arg0` = pixel count, `arg1` = data GPIO), kept in sync across every `protocol.h` copy (sender, receiver, `darkroom-enlarger-head`) for consistency even though `darkroom-receiver` has no LED panel and simply falls through its `default:` case.
- **Authority and persistence split**: the real LED head (`darkroom-enlarger-head`), not the sender, is the authority on which geometry is actually active — it persists the last `CMD_SET_LED_CONFIG` it received in its own `Preferences`/NVS and rebuilds its LED driver from that on every boot (falling back to a compiled-in default if never configured or if a stored value is out of range). This matters because ESP-NOW has no ACK/retry and ships no guarantee the two boards are powered together — without its own persistence, the real head would forget its panel size on every independent reboot. The sender keeps only a local copy of the same two values (its own `Preferences`) purely so the hidden menu can display the last-set values and resend them.
- **Explicit send-on-`SAVE`, nothing on open**: `UI_LEDCFG` transmits `CMD_SET_LED_CONFIG` in exactly one place — the **`SAVE`** button (renamed from an earlier `BACK`), which always sends unconditionally, whether or not any value was actually changed since the menu opened. Opening the menu itself (the 3-second `MEAS LIT` hold) sends nothing on its own; an earlier version did send an automatic "resync ping" on open, but that meant simply opening the screen to check the current values silently transmitted and triggered the receiving board's confirmation flash even with no intent to change anything, so it was removed. This also means `SAVE` doubles as the explicit "resync a head that lost its config" action (fresh flash, swapped board): open the menu and tap `SAVE` immediately, no edit required. `+`/`- ` taps on either row only update the local value and `saveLedConfig()`'s local `Preferences` copy — they never transmit by themselves.
- **Guard**: the real LED head only accepts `CMD_SET_LED_CONFIG` while idle — reconfiguring the strip mid-exposure/focus is refused, matching the same idle-only guard philosophy already used for `CMD_FOCUS_ON`/`CMD_METER_*_ON` (§3).
