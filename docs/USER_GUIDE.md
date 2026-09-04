# Splitgrade Controller — User Guide

A simple, practical guide to everything you can do with the controller on a real darkroom printing day. No technical background needed.

---

## 1) The big picture

This controller helps you get to a good first exposure quickly, then gets out of your way so you can print by eye, the way you always have.

Two devices work together:

- **The controller in your hand** (the touchscreen unit you operate).
- **The enlarger head** (the LED lamp in your enlarger, running the
  [darkroom-enlarger-head](https://github.com/Draganito/darkroom-enlarger-head)
  firmware — or a second SenseCAP as a bench stand-in while testing).

They talk to each other wirelessly. You never need to plug anything in in the darkroom.

---

## 2) The main screen

When you switch the controller on, you see the main screen with:

- **MEAS BLK** — meter the blue (hard/contrast) light and get a fresh hard exposure time.
- **MEAS LIT** — meter the green (soft/tone) light and get a fresh soft exposure time.
- **+ / -** (two middle columns) — fine-tune `HARD` and `SOFT` exposure times by hand.
- **FOCUS** — turn on the focus light so you can compose/focus the image.
- **EXPOSURE** — start (or stop) the actual print exposure.
- A status bar shows what's currently happening (`IDLE`, `FOCUS`, or a live countdown).

There is no AUTO/MANUAL mode to think about anymore — `MEAS BLK`/`MEAS LIT` always take a fresh, direct measurement, and the `+`/`-` buttons always nudge whatever time is currently shown.

---

## 3) Setting up a paper (do this once per paper/developer combo, using a Stouffer stepwedge)

You calibrate blue and green separately, using the same steps for each color. This finds one "target dose" number per color and stores it — you won't need to touch this screen again until you change paper or developer.

1. Insert your Stouffer stepwedge in the negative carrier.
2. Hold **MEAS BLK** for about 3 seconds to open the hidden calibration screen.
3. For the **blue** side: tap **READ REF** to capture the current blue light intensity.
4. At your enlarger, manually expose a real test print through the stepwedge for the shown **TEST TIME** (adjust with `TIME +/-` if you want a different duration first).
5. Develop the print. Find the step where the tone first reaches your target (e.g. first solid black for blue) and dial that step number into **STEP +/-** for the blue side.
6. Tap **SAVE** on the blue side. The target dose is now stored.
7. Repeat steps 3–6 for the **green** side (use a lighter reference step, e.g. first visible grey, since green sets your highlight/tone response).
8. Tap **BACK** to return to the main screen.

You don't need to redo this for every print — only when you change paper, developer, or want to re-calibrate.

---

## 4) Everyday printing — the normal routine

1. Put your negative in the carrier and focus/frame the image (use **FOCUS** as needed).
2. Place your easel. **Measure on the clear film strip between two frames**, not on the image itself — this gives a stable, repeatable reading every time.
3. Tap **MEAS BLK** (a quick, single tap).
   - The screen briefly goes dark for about a second — this is intentional, so the panel's own light doesn't disturb the measurement.
   - The controller measures the blue light and computes a fresh `HARD` time from your calibrated blue dose.
4. Tap **MEAS LIT** the same way — this measures the green light and computes a fresh `SOFT` time from your calibrated green dose.
5. Tap **EXPOSURE** to make your first print.
6. Judge the result. If it's close, you're basically done. If not, fine-tune (see next section).

Both measurements are always fresh, absolute readings — there's no "baseline" to keep track of, and no need to remember whether you're in some particular mode. If you change the enlarger height or reframe, just measure again with `MEAS BLK`/`MEAS LIT` — the new readings already account for the new light level.

---

## 5) Fine-tuning by hand

- **HARD +/-**: nudge the blue/hard exposure time directly, e.g. after judging a test print.
- **SOFT +/-**: nudge the green/soft exposure time directly.
- Tapping **MEAS BLK**/**MEAS LIT** again always overwrites your hand-tuned value with a fresh measurement — so do your manual fine-tuning *after* your last measurement for a given print, not before.

---

## 6) Holding buttons down (fast adjustments)

Any `+`/`-` button (main screen or the calibration screen) can be held down for continuous, accelerating adjustment:

- Hold briefly: normal single steps.
- Hold longer: steps speed up automatically (slow → fast → very fast).

This makes large adjustments quick without needing dozens of taps.

---

## 7) Focus light safety

If you accidentally tap **FOCUS** while an exposure is actively running, the controller will **stop the exposure** instead of jumping straight into focus mode. Tap **FOCUS** again afterward if you actually want the focus light.

This protects your print from an accidental full-brightness flash mid-exposure.

---

## 8) Stopping anything, anytime

Tap **EXPOSURE** at any time during focus or an active exposure to immediately stop it. It's your universal "abort" button, and this always fires the instant you touch the screen — nothing described in §11 below ever delays or interferes with it. `MEAS BLK`/`MEAS LIT` and the calibration screen are also disabled while an exposure or focus session is running, so you can't accidentally take a contaminated reading mid-print.

---

## 9) Sound feedback

Every button press gives a short, quiet click sound, so you get immediate confirmation even in the dark, without disturbing anyone. A longer, distinct beep means an action failed (e.g. a measurement couldn't get a valid reading) — the controller never silently leaves you with a wrong number.

---

## 10) The hidden calibration menu

Hold **MEAS BLK** for about 3 seconds on the main screen to open the hidden calibration screen described in §3. Tap `BACK` to return.

---

## 11) The hidden cross-factor menu (optional, advanced)

This is a small, optional knob for one specific, real, well-documented situation in split-grade printing: your hard/blue exposure doesn't only affect the shadows — it also lands a little extra, unwanted density on the highlights, because of how variable-contrast paper's emulsion actually works (this is a known, named effect in darkroom literature, not a guess — see `TECHNICAL_CONCEPT.md` §5.4 for the sources). You may notice this as highlights looking slightly darker/muddier than your soft-exposure test strip predicted, especially with a long hard/blue exposure. Most prints never need this — leave it at its default (`+0.00`, no effect) unless you've actually seen this on a real print.

- Hold **EXPOSURE** for about 3 seconds **while idle** (not during a print) to open the hidden `XOVER TUNE` screen. This never affects starting or stopping a print — a quick tap on `EXPOSURE` still starts/stops your exposure exactly as before; only holding it for the full 3 seconds opens this menu instead.
- Use **+ / -** to raise or lower the **CROSS FACTOR** (steps of `0.01`, hold for fast adjustment, range `-0.30` to `+0.30`). A **positive** value means: for every second of hard/blue exposure, that fraction of a second is subtracted from the soft/green exposure time (e.g. `+0.15` with a 10 s hard exposure trims 1.5 s off the soft time) — this is the documented, expected-to-be-useful direction. The screen tells you in plain words which direction is currently active. A negative value is also available (subtracts from hard instead) in case your specific paper ever needs it the other way round, but start with positive values.
- Tap **BACK** to return to the main screen. The value is saved automatically as you adjust it.

How to find the right value for your paper: meter and print a negative that needs a fairly long hard/blue exposure, once with the factor at `+0.00`. If the highlights look darker/muddier than expected, try again at `+0.10`, then `+0.15`, and so on — reprinting and comparing until the highlights look right. Once you've found a good value for a given paper/developer, it stays reproducible and you won't need to touch this screen again for that combination.

---

## 12) The hidden LED-panel menu (one-time setup)

If your enlarger head uses a different LED panel than the reference one
(different pixel count, or the data wire on another GPIO), you can tell it
so from here — no re-flashing needed.

- Hold **MEAS LIT** for about 3 seconds **while idle** to open the hidden
  `LEDCFG` screen. A quick tap on `MEAS LIT` still meters green light as
  usual.
- Use **+ / -** to set the **pixel count** and the **data GPIO** to match
  your panel (see the head's
  [FLASH.md](https://github.com/Draganito/darkroom-enlarger-head/blob/main/FLASH.md)
  §4 for the reference values).
- Tap **SAVE** to send the settings to the head. The head stores them
  itself and remembers them across power cycles. Opening the menu alone
  sends nothing — only `SAVE` transmits, so you can peek at the current
  values safely. `SAVE` with unchanged values doubles as a "resync" for a
  freshly flashed head.
- Tap **BACK** to return to the main screen.

You normally touch this once, right after flashing, and never again.

---

## 13) Quick reference card

| You want to... | Do this |
|---|---|
| Set up a new paper/developer | Hold `MEAS BLK` ~3s → per color: `READ REF` → manual test print → set `STEP` → `SAVE` |
| Get a fresh starting exposure | Measure film strip between frames → `MEAS BLK` then `MEAS LIT` |
| Nudge the hard/blue time by hand | `HARD +/-` |
| Nudge the soft/green time by hand | `SOFT +/-` |
| Focus the image | `FOCUS` |
| Start/stop a print | `EXPOSURE` (quick tap, while idle/running) |
| Abort anything immediately | `EXPOSURE` |
| Tune the optional cross-factor (highlights look muddy after a long hard exposure) | Hold `EXPOSURE` ~3s while idle → `+` (positive values) → `BACK` |
| Set LED count / GPIO for your panel (once after flashing) | Hold `MEAS LIT` ~3s while idle → set values → `SAVE` |

---

## 14) What this controller is *not*

It does not make artistic decisions for you. It gives you a fast, repeatable, technically consistent starting point — the final look of the print is always yours to shape by eye, exactly as in traditional darkroom printing.
