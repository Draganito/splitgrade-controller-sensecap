#include <Arduino.h>
#include <Wire.h>
#include <WiFi.h>
#include <esp_now.h>
#include <esp_wifi.h>
#include <Preferences.h>
#include <Arduino_GFX_Library.h>
#include <PacketSerial.h>
#include "Indicator_SWSPI.h"
#include "protocol.h"
#include "touch.h"

// ---------------------------------------------------------------------------
// darkroom-sender — wireless darkroom controller (SenseCAP D1).
// Touch UI + fire-and-forget ESP-NOW + local estimated countdown.
//
// Stable baseline decisions (do not regress silently):
// - Display init must run through Indicator_SWSPI (expander-aware CS/RST),
//   otherwise cold boots can show backlight-only/blank panel behavior.
// - Touch defaults are orientation-only (swap/flip), not raw scaling maps.
// - For touch address 0x48 force FT5x06 parser (AUTO parsing caused split
//   left/right behavior when CST format occasionally mis-parsed frames).
// - ESP-NOW uses channel 1 and sends broadcast + two known peers for robust
//   one-way command delivery.
// - Dose-based exposure model (see docs/TECHNICAL_CONCEPT.md §5): blue and
//   green each have an independently calibrated target dose (lux*s), found
//   via a Stouffer stepwedge test print. MEAS BLK/MEAS LIT directly meter the
//   respective color light and convert measured intensity into exposure time
//   from that stored dose. No crossover/compensation term, no AUTO/MANUAL
//   mode distinction -- every measurement is already a fresh absolute value.
// ---------------------------------------------------------------------------

#define GFX_BL 45
#define SCREEN_W 480
#define SCREEN_H 480
static constexpr uint8_t kBacklightDimPercent = 10;

// Two known SenseCAP devices in this project.
// We send to both, skipping our own MAC at runtime.
static const uint8_t kDeviceMacA[6] = {0x90, 0x70, 0x69, 0x10, 0xA1, 0x34};
static const uint8_t kDeviceMacB[6] = {0x90, 0x70, 0x69, 0x10, 0x9F, 0xC0};
static const uint8_t kBroadcastMac[6] = {0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF};

Indicator_SWSPI *bus = new Indicator_SWSPI(5, 4, 41, 48);

Arduino_ESP32RGBPanel *rgbpanel = new Arduino_ESP32RGBPanel(
    18, 17, 16, 21,
    4, 3, 2, 1, 0,
    10, 9, 8, 7, 6, 5,
    15, 14, 13, 12, 11,
    1, 10, 8, 50,
    1, 10, 8, 20);

Arduino_RGB_Display *gfx = new Arduino_RGB_Display(
    SCREEN_W, SCREEN_H, rgbpanel, 2, true,
    bus, GFX_NOT_DEFINED,
    st7701_type1_init_operations, sizeof(st7701_type1_init_operations));

enum UiScreen : uint8_t { UI_MAIN = 0, UI_CAL, UI_XOVER, UI_LEDCFG };
enum RunState : uint8_t {
  RUN_IDLE = 0,
  RUN_FOCUS,
  RUN_HARD,
  RUN_SOFT,
  RUN_SPLIT_HARD,
  RUN_SPLIT_SOFT,
};

// Stouffer stepwedge geometry used by the calibration formulas below.
// 21 steps, 1/2 stop per step (confirmed against the physical wedge in use).
static constexpr float kStepwedgeStopsPerStep = 0.5f;
static constexpr uint8_t kStepwedgeSteps = 21;
static constexpr uint8_t kStepwedgeMinStep = 1;
static constexpr uint8_t kStepwedgeMaxStep = kStepwedgeSteps;

Preferences prefs;
UiScreen uiScreen = UI_MAIN;
UiScreen lastDrawnScreen = (UiScreen)255;
RunState runState = RUN_IDLE;
bool safelight = false;
uint8_t backlightDuty = 200;
uint16_t hardHalf = 8;   // 4.0 s default
uint16_t softHalf = 24;  // 12.0 s default
uint16_t remainingHalf = 0;
uint16_t splitSoftHalf = 0;
uint32_t nextTickAtMs = 0;
bool displayDirty = true;
uint32_t lastTouchActionMs = 0;
uint8_t selfMac[6] = {0};
bool selfMacReady = false;
int16_t lastTouchX = -1;
int16_t lastTouchY = -1;
bool touchPressedLatched = false;

// Persisted per-channel target doses (lux*s), found via the calibration
// workflow on the UI_CAL screen. -1 means "not calibrated yet".
float calDoseBlue = -1.0f;   // "cal_dose_b"
float calDoseGreen = -1.0f;  // "cal_dose_g"

// Transient calibration inputs (not persisted -- re-entered each session,
// same as the old CAL screen's intermediate values used to be).
float calRefIntensityBlue = -1.0f;
float calRefIntensityGreen = -1.0f;
uint8_t calStepBlue = 3;
uint8_t calStepGreen = 18;
uint16_t calTestHalfSeconds = 20;  // 10.0 s default test exposure

// Optional, per-paper cross-sensitivity compensation (see docs/
// TECHNICAL_CONCEPT.md §5.4). VC paper's "soft" emulsion part keeps its full
// blue sensitivity (it is a blue-sensitive emulsion with green sensitizer
// *added*, not swapped) while the "hard" part has no green sensitizer at
// all -- so a hard/blue exposure also lands on the highlight-controlling
// soft layer, but a soft/green exposure cannot reach the pure-blue layer.
// This is documented split-grade printing behavior (e.g. Lambrecht &
// Woodhouse, "Way Beyond Monochrome"; Ilford's own Multigrade technical
// notes) -- positive values reduce SOFT by a fraction of HARD, matching that
// documented direction. Negative values (the other, less-common direction)
// remain available in case real-world testing on a specific paper/light
// source ever shows a need for it. 0 = off, matching all prior behavior
// exactly. Found empirically per paper via real test prints, not derived
// from a formula.
int16_t crossFactorPermille = 0;  // "cross_perm", e.g. 150 = +0.15
static constexpr int16_t kCrossFactorMaxPermille = 300;   // cap at +/-0.30
static constexpr int16_t kCrossFactorStepPermille = 10;   // +/- step of 0.01

// LED panel geometry for darkroom-gledopto (see docs/TECHNICAL_CONCEPT.md,
// docs/GLEDOPTO_BRINGUP_NOTES.md). This is only a local copy of what was
// last sent/persisted here -- the Gledopto head is the actual authority on
// which geometry is really active (it persists CMD_SET_LED_CONFIG itself),
// this copy just lets the hidden UI_LEDCFG menu show the last-set values
// and resend/resync them (e.g. after the head reboots on its own).
static constexpr uint8_t kLedDataPinDefault = 16; // gledopto's factory-default "D" terminal
uint16_t ledPixelCount = 169;                     // "led_count", matches the reference panel
uint8_t ledDataPin = kLedDataPinDefault;          // "led_pin" -- freely +/- adjustable, see UI_LEDCFG

static constexpr int16_t kMainColW = 112;
static constexpr int16_t kMainGap = 8;
static constexpr int16_t kMainX0 = 4;      // symmetric horizontal margins
static constexpr int16_t kMainTopY = 84;
static constexpr int16_t kMainBotY = 252;
static constexpr int16_t kMainBtnH = 144;
static constexpr int16_t kMainMidBtnH = 124;
static constexpr int16_t kMainMidTopY = kMainTopY;
static constexpr int16_t kMainMidBotY = kMainBotY + (kMainBtnH - kMainMidBtnH);
static constexpr uint32_t kMeasBlkHoldMs = 3000;  // hold MEAS BLK to open CAL
static constexpr uint32_t kExposureHoldMs = 3000; // hold EXPOSURE (while idle) to open XOVER TUNE
static constexpr uint32_t kMeasLitHoldMs = 3000;  // hold MEAS LIT to open LED PANEL CONFIG
static constexpr int16_t kTouchHoldMoveTol = 18;
static constexpr uint32_t kHoldStartDelayMs = 320;
static constexpr uint32_t kHoldRepeatSlowMs = 140;
static constexpr uint32_t kHoldRepeatFastMs = 70;
static constexpr uint32_t kHoldRepeatTurboMs = 35;
static constexpr uint16_t kTapBeepMs = 12; // fixed low click feedback
// LED warm-up after switching a meter color on, before any sample is taken.
// SK6812 output drifts noticeably in the first seconds after turn-on; 3 s
// gets past the steep part of that curve for both calibration (READ REF)
// and daily metering (MEAS BLK/MEAS LIT), so both see the same light.
static constexpr uint32_t kMeterWarmupMs = 3000;
// Multi-sample averaging: 20 samples, trimmed mean (min and max dropped).
// The TSL2591 integrates 100 ms per sample on the bridge, so a full run is
// ~2.5 s -- cheap insurance for a value that ends up persisted in flash.
static constexpr uint8_t kMeterSamples = 20;
static constexpr uint8_t kMeterSamplesMin = 12;      // accept run despite a few dropped samples
static constexpr uint32_t kMeterSampleTimeoutMs = 1400; // per-sample poll timeout
static constexpr uint32_t kMeterRunBudgetMs = 8000;  // hard cap on one averaging run

enum MeterSensorType : uint8_t { SENSOR_NONE = 0, SENSOR_TSL2591, SENSOR_AS7343 };
MeterSensorType meterSensor = SENSOR_NONE;
float meterLastLux = -1.0f;
float meterLastBlueNm = -1.0f;
float meterLastGreenNm = -1.0f;
uint32_t meterLastReadMs = 0;
bool meterLastReadOk = false;
uint32_t meterLastRxMs = 0;
uint32_t meterLastLuxMs = 0;
uint32_t meterLastBlueMs = 0;
uint32_t meterLastGreenMs = 0;

// Hold-to-open-CAL state machine for the MEAS BLK button (top-left, col 0).
bool measBlkHoldCandidate = false;
bool measBlkHoldTriggered = false;
uint32_t measBlkHoldStartedMs = 0;
int16_t measBlkHoldStartX = -1;
int16_t measBlkHoldStartY = -1;

// Hold-to-open-XOVER-TUNE state machine for the EXPOSURE button (bottom-right,
// col 3). Only ever becomes a candidate while runState == RUN_IDLE (see
// pointInMainExposureButton()/handleTouch()), so the safety-critical instant
// STOP-on-press behavior while an exposure/focus is running is completely
// untouched -- this hold gesture cannot delay or interfere with an abort.
bool exposureHoldCandidate = false;
bool exposureHoldTriggered = false;
uint32_t exposureHoldStartedMs = 0;
int16_t exposureHoldStartX = -1;
int16_t exposureHoldStartY = -1;

// Hold-to-open-LED-PANEL-CONFIG state machine for the MEAS LIT button
// (bottom-left, col 0). Same pattern as measBlkHoldCandidate above --
// unrelated to exposure safety, so no interaction with the STOP paths.
bool measLitHoldCandidate = false;
bool measLitHoldTriggered = false;
uint32_t measLitHoldStartedMs = 0;
int16_t measLitHoldStartX = -1;
int16_t measLitHoldStartY = -1;

enum HoldRepeatAction : uint8_t {
  HOLD_NONE = 0,
  HOLD_MAIN_HARD_PLUS,
  HOLD_MAIN_HARD_MINUS,
  HOLD_MAIN_SOFT_PLUS,
  HOLD_MAIN_SOFT_MINUS,
  HOLD_CAL_TIME_PLUS,
  HOLD_CAL_TIME_MINUS,
  HOLD_CAL_STEP_BLUE_PLUS,
  HOLD_CAL_STEP_BLUE_MINUS,
  HOLD_CAL_STEP_GREEN_PLUS,
  HOLD_CAL_STEP_GREEN_MINUS,
  HOLD_XOVER_PLUS,
  HOLD_XOVER_MINUS,
  HOLD_LEDCFG_COUNT_PLUS,
  HOLD_LEDCFG_COUNT_MINUS,
  HOLD_LEDCFG_PIN_PLUS,
  HOLD_LEDCFG_PIN_MINUS,
};
HoldRepeatAction holdRepeatAction = HOLD_NONE;
uint32_t holdRepeatStartedMs = 0;
uint32_t holdRepeatLastStepMs = 0;
int16_t holdRepeatX = 0;
int16_t holdRepeatY = 0;
int16_t holdRepeatW = 0;
int16_t holdRepeatH = 0;

PacketSerial rpPacketSerial;
bool rpLinkReady = false;

// ESP32 <-> RP2040 packet types (COBS framed by PacketSerial)
static constexpr uint8_t PKT_TYPE_CMD_BEEP_ON = 0xA1;
static constexpr uint8_t PKT_TYPE_CMD_BEEP_OFF = 0xA2;
static constexpr uint8_t PKT_TYPE_CMD_EXT_SAMPLE_NOW = 0xA6;
static constexpr uint8_t PKT_TYPE_SENSOR_EXT_TSL2591_LUX = 0xB6;
// Byte values (0xB7/0xB8) match darkroom-rp2040's bridge firmware, which
// reads AS7343 channels F3 (475nm, blue) / F4 (515nm, green) -- chosen to
// match the SK6812RGBW-NW LED peaks planned for the future enlarger head.
// Named BLUE/GREEN (not F2/F5) to stay in sync with the bridge's naming.
static constexpr uint8_t PKT_TYPE_SENSOR_EXT_AS7343_BLUE = 0xB7;
static constexpr uint8_t PKT_TYPE_SENSOR_EXT_AS7343_GREEN = 0xB8;

struct Btn {
  int16_t x, y, w, h;
  const char *label;
  uint16_t fill;
  uint16_t text;
  bool enabled;
};

static bool initDisplay();
void setBacklight(uint8_t duty);
static bool sendCommandRetry(Command cmd, uint16_t arg0 = 0, uint16_t arg1 = 0,
                             uint8_t attempts = 3, uint16_t gapMs = 12);
static bool pointInMainMeasBlkButton(int16_t x, int16_t y);
static bool pointInMainMeasLitButton(int16_t x, int16_t y);
static bool pointInMainExposureButton(int16_t x, int16_t y);
static bool fetchFreshSensorMetric(float &metricOut);
static bool meterAtColor(Command meterOnCmd, float &metricOut);
static void doMeasureBlue();
static void doMeasureGreen();
static void doStartExposure();
static void clearHoldRepeat();
static void startHoldRepeat(HoldRepeatAction action, int16_t x, int16_t y, int16_t w, int16_t h);
static bool isWithinHoldRepeat(int16_t x, int16_t y);
static void performHoldRepeatStep();
static void updateHoldRepeatWhilePressed(int16_t x, int16_t y);
static bool initRp2040Link();
static void updateRp2040Link();
static void rp2040OnPacket(const uint8_t *buffer, size_t size);
static void rp2040SendCommand(uint8_t type, uint32_t value = 0);
static void rp2040BeepPulse(uint16_t ms = 70);
static const char *meterSensorName();
static void playTapBeep();

static bool initDisplay() {
  pinMode(GFX_BL, OUTPUT);
  digitalWrite(GFX_BL, LOW);
  for (uint8_t i = 0; i < 4; ++i) {
    if (gfx->begin()) {
      setBacklight(backlightDuty);
      return true;
    }
    delay(120);
  }
  return false;
}

void setBacklight(uint8_t duty) {
  backlightDuty = duty;
  const uint8_t pwm = (uint8_t)(((uint16_t)duty * kBacklightDimPercent) / 100);
  analogWrite(GFX_BL, pwm);
}

static bool sendCommand(Command cmd, uint16_t arg0 = 0, uint16_t arg1 = 0) {
  ControlPacket pkt{};
  fillPacket(pkt, cmd, arg0, arg1);
  bool ok = false;
  // Broadcast fallback to avoid stale peer/channel edge cases.
  ok = (esp_now_send(kBroadcastMac, (uint8_t *)&pkt, sizeof(pkt)) == ESP_OK) || ok;
  if (!selfMacReady || memcmp(selfMac, kDeviceMacA, 6) != 0) {
    ok = (esp_now_send(kDeviceMacA, (uint8_t *)&pkt, sizeof(pkt)) == ESP_OK) || ok;
  }
  if (!selfMacReady || memcmp(selfMac, kDeviceMacB, 6) != 0) {
    ok = (esp_now_send(kDeviceMacB, (uint8_t *)&pkt, sizeof(pkt)) == ESP_OK) || ok;
  }
  return ok;
}

static bool sendCommandRetry(Command cmd, uint16_t arg0, uint16_t arg1, uint8_t attempts, uint16_t gapMs) {
  if (attempts == 0) {
    attempts = 1;
  }
  bool ok = false;
  for (uint8_t i = 0; i < attempts; ++i) {
    ok = sendCommand(cmd, arg0, arg1) || ok;
    if (ok) {
      break;
    }
    if (i + 1 < attempts) {
      delay(gapMs);
    }
  }
  return ok;
}

static void noteTxResult(bool ok) {
  (void)ok;
  displayDirty = true;
}

static void applyTouchOrientation() {
  const bool swap = prefs.getBool("t_swap", touch_swap_xy);
  const bool flipX = prefs.getBool("t_flipx", touch_flip_x);
  const bool flipY = prefs.getBool("t_flipy", touch_flip_y);
  touch_set_orientation(swap, flipX, flipY);
}

static void loadPrefs() {
  // Fixed darkroom UI mode: red palette + fixed 10% backlight.
  safelight = true;
  // setBacklight() applies kBacklightDimPercent again, so duty stays at full
  // scale here to achieve an effective 10% output.
  backlightDuty = 255;
  hardHalf = prefs.getUShort("hard_half", 8);
  softHalf = prefs.getUShort("soft_half", 24);
  calDoseBlue = prefs.getFloat("cal_dose_b", -1.0f);
  calDoseGreen = prefs.getFloat("cal_dose_g", -1.0f);
  crossFactorPermille = prefs.getShort("cross_perm", 0);
  ledPixelCount = prefs.getUShort("led_count", ledPixelCount);
  ledDataPin = prefs.getUChar("led_pin", ledDataPin);
  const bool pinValid = (ledDataPin >= kLedConfigMinPin && ledDataPin <= kLedConfigMaxPin);
  const bool countValid = (ledPixelCount >= kLedConfigMinPixels && ledPixelCount <= kLedConfigMaxPixels);
  if (!pinValid || !countValid) {
    ledPixelCount = 169;
    ledDataPin = kLedDataPinDefault;
  }
}

static void saveExposurePrefs() {
  prefs.putUShort("hard_half", hardHalf);
  prefs.putUShort("soft_half", softHalf);
}

static void saveCrossFactor() {
  prefs.putShort("cross_perm", crossFactorPermille);
}

static void saveDoseBlue() {
  prefs.putFloat("cal_dose_b", calDoseBlue);
}

static void saveDoseGreen() {
  prefs.putFloat("cal_dose_g", calDoseGreen);
}

static void saveLedConfig() {
  prefs.putUShort("led_count", ledPixelCount);
  prefs.putUChar("led_pin", ledDataPin);
}

// Sends the currently held panel geometry to darkroom-gledopto. Called
// exactly once, unconditionally, when SAVE is tapped in UI_LEDCFG (see
// handleLedCfgTouch()) -- covering every +/- tap and pin toggle made since
// the menu was opened in one transmission, not one per edit. Opening the
// menu itself never sends anything; tapping SAVE right away with no edits
// is the deliberate way to resync a Gledopto head that lost its config
// (e.g. fresh flash, swapped board) without going through +/- first.
static void sendLedConfig() {
  const bool ok = sendCommandRetry(CMD_SET_LED_CONFIG, ledPixelCount, ledDataPin);
  noteTxResult(ok);
  if (!ok) {
    rp2040BeepPulse(180);
  }
}

static const char *meterSensorName() {
  switch (meterSensor) {
    case SENSOR_TSL2591:
      return "TSL2591";
    case SENSOR_AS7343:
      return "AS7343";
    default:
      return "NONE";
  }
}

static bool initRp2040Link() {
  Serial1.begin(115200, SERIAL_8N1, 20, 19);
  rpPacketSerial.setStream(&Serial1);
  rpPacketSerial.setPacketHandler(&rp2040OnPacket);
  rpLinkReady = true;
  Serial.println("RP2040 link ready on UART1");
  return true;
}

static void updateRp2040Link() {
  if (!rpLinkReady) {
    return;
  }
  rpPacketSerial.update();
  if (rpPacketSerial.overflow()) {
    Serial.println("RP2040 packet overflow");
  }
}

static void rp2040OnPacket(const uint8_t *buffer, size_t size) {
  if (size < 5) {
    return;
  }
  const uint8_t type = buffer[0];
  float value = 0.0f;
  memcpy(&value, &buffer[1], sizeof(float));

  meterLastRxMs = millis();
  meterLastReadMs = meterLastRxMs;
  meterLastReadOk = true;

  switch (type) {
    case PKT_TYPE_SENSOR_EXT_TSL2591_LUX:
      meterSensor = SENSOR_TSL2591;
      meterLastLux = value;
      meterLastLuxMs = meterLastRxMs;
      break;
    case PKT_TYPE_SENSOR_EXT_AS7343_BLUE:
      meterSensor = SENSOR_AS7343;
      meterLastBlueNm = value;
      meterLastBlueMs = meterLastRxMs;
      break;
    case PKT_TYPE_SENSOR_EXT_AS7343_GREEN:
      meterSensor = SENSOR_AS7343;
      meterLastGreenNm = value;
      meterLastGreenMs = meterLastRxMs;
      break;
    default:
      break;
  }
  displayDirty = true;
}

static void rp2040SendCommand(uint8_t type, uint32_t value) {
  if (!rpLinkReady) {
    return;
  }
  uint8_t buf[5];
  buf[0] = type;
  memcpy(&buf[1], &value, sizeof(value));
  rpPacketSerial.send(buf, sizeof(buf));
}

static void rp2040BeepPulse(uint16_t ms) {
  rp2040SendCommand(PKT_TYPE_CMD_BEEP_ON, ms);
}

static void playTapBeep() {
  rp2040BeepPulse(kTapBeepMs);
}

static bool pointInMainMeasBlkButton(int16_t x, int16_t y) {
  const int16_t bx = kMainX0 + 0 * (kMainColW + kMainGap);
  const int16_t by = kMainTopY;
  return x >= bx && x <= (bx + kMainColW) && y >= by && y <= (by + kMainBtnH);
}

static bool pointInMainMeasLitButton(int16_t x, int16_t y) {
  const int16_t bx = kMainX0 + 0 * (kMainColW + kMainGap);
  const int16_t by = kMainBotY;
  return x >= bx && x <= (bx + kMainColW) && y >= by && y <= (by + kMainBtnH);
}

static bool pointInMainExposureButton(int16_t x, int16_t y) {
  const int16_t bx = kMainX0 + 3 * (kMainColW + kMainGap);
  const int16_t by = kMainBotY;
  return x >= bx && x <= (bx + kMainColW) && y >= by && y <= (by + kMainBtnH);
}

// One sensor sample. Assumes the backlight is already off (see
// fetchFreshSensorMetric()); sends SAMPLE_NOW and polls for a fresh value.
static bool fetchSensorSampleRaw(float &metricOut) {
  const uint32_t startedAt = millis();
  meterLastReadOk = false;
  rp2040SendCommand(PKT_TYPE_CMD_EXT_SAMPLE_NOW, 0);

  while ((millis() - startedAt) < kMeterSampleTimeoutMs) {
    updateRp2040Link();
    // TSL2591 (with the bridge's auto-gain) is the sensor we actually rely on
    // for metering -- it needs no spectral calibration for the ratio-based
    // exposure math. AS7343 stays wired and is only used as a fallback when
    // no TSL2591 is present on the bridge, so it remains testable on its own.
    // Checked in this fixed priority order rather than "whichever sensor's
    // packet happened to arrive last", since the bridge sends both readings
    // every cycle when both sensors are attached.
    if (meterLastLuxMs >= startedAt && meterLastLux > 0.01f) {
      meterSensor = SENSOR_TSL2591;
      metricOut = meterLastLux;
      return true;
    }
    if (meterLastBlueMs >= startedAt && meterLastGreenMs >= startedAt) {
      const float mix = (meterLastBlueNm + meterLastGreenNm) * 0.5f;
      if (mix > 0.01f) {
        meterSensor = SENSOR_AS7343;
        metricOut = mix;
        return true;
      }
    }
    delay(8);
  }
  return false;
}

// Averaged measurement: backlight off, up to kMeterSamples individual
// samples (bounded by kMeterRunBudgetMs), trimmed mean (min and max
// dropped). Fails fast if the very first sample times out (dead link /
// missing sensor) so the screen doesn't stay dark for the whole budget.
// Backlight is restored on every exit path.
static bool fetchFreshSensorMetric(float &metricOut) {
  if (!rpLinkReady) {
    return false;
  }
  const uint8_t restoreDuty = backlightDuty;
  // Prevent panel light from disturbing optical sensor reads.
  setBacklight(0);
  delay(1000);

  float samples[kMeterSamples];
  uint8_t got = 0;
  const uint32_t runStartedAt = millis();
  for (uint8_t i = 0; i < kMeterSamples; ++i) {
    if ((millis() - runStartedAt) >= kMeterRunBudgetMs) {
      break;
    }
    float v = 0.0f;
    if (fetchSensorSampleRaw(v)) {
      samples[got++] = v;
    } else if (got == 0) {
      break;
    }
  }
  setBacklight(restoreDuty);

  if (got < kMeterSamplesMin) {
    return false;
  }
  float sum = 0.0f;
  float mn = samples[0];
  float mx = samples[0];
  for (uint8_t i = 0; i < got; ++i) {
    sum += samples[i];
    if (samples[i] < mn) {
      mn = samples[i];
    }
    if (samples[i] > mx) {
      mx = samples[i];
    }
  }
  metricOut = (sum - mn - mx) / (float)(got - 2);
  return metricOut > 0.01f;
}

// Turns on a single color light on the (simulated or real) enlarger head,
// waits kMeterWarmupMs for the LEDs to warm up, takes an averaged sensor
// reading (display dark during the samples), then turns the light back off.
// Used by the daily MEAS BLK/MEAS LIT measurements *and* by the CAL screen's
// READ REF -- both must see the same warmed-up light through the same
// averaging, otherwise the noise doesn't cancel in dose / intensity.
// The TSL2591 is a broadband sensor, so this only produces a valid color-
// specific reading if no other light is active at the same time -- on real
// hardware the receiver enforces exactly one color at a time.
static bool meterAtColor(Command meterOnCmd, float &metricOut) {
  sendCommandRetry(meterOnCmd);
  delay(kMeterWarmupMs);
  const bool ok = fetchFreshSensorMetric(metricOut);
  sendCommandRetry(CMD_STOP);
  return ok;
}

// seconds = targetDose / measuredIntensity, converted to clamped half-seconds.
static uint16_t doseToHalfSeconds(float targetDose, float measuredIntensity) {
  if (targetDose <= 0.0f || measuredIntensity <= 0.01f) {
    return 0;
  }
  const float seconds = targetDose / measuredIntensity;
  long half = lroundf(seconds * 2.0f);
  if (half < 0) {
    half = 0;
  }
  if (half > (long)kMaxHalfSeconds) {
    half = (long)kMaxHalfSeconds;
  }
  return (uint16_t)half;
}

// Calibration formula (see docs/TECHNICAL_CONCEPT.md §5):
//   lightFactor  = 2^kStepwedgeStopsPerStep
//   basisSeconds = testSeconds / lightFactor^(stepFound - 1)
//   targetDose   = calibIntensity * basisSeconds
// basisSeconds is the exposure time equivalent to step 1 of the wedge (the
// darkest/reference step), so targetDose is comparable across different test
// exposure times.
static float computeTargetDose(float calibIntensity, uint16_t testHalfSeconds, uint8_t stepFound) {
  if (calibIntensity <= 0.01f || testHalfSeconds < 1 || stepFound < kStepwedgeMinStep) {
    return -1.0f;
  }
  const float testSeconds = testHalfSeconds / 2.0f;
  const float lightFactor = powf(2.0f, kStepwedgeStopsPerStep);
  const float basisSeconds = testSeconds / powf(lightFactor, (float)(stepFound - 1));
  return calibIntensity * basisSeconds;
}

static void doMeasureBlue() {
  float metric = 0.0f;
  const bool ok = meterAtColor(CMD_METER_BLUE_ON, metric);
  if (ok && calDoseBlue > 0.0f) {
    hardHalf = doseToHalfSeconds(calDoseBlue, metric);
    saveExposurePrefs();
  } else {
    rp2040BeepPulse(180);
  }
  displayDirty = true;
}

static void doMeasureGreen() {
  float metric = 0.0f;
  const bool ok = meterAtColor(CMD_METER_GREEN_ON, metric);
  if (ok && calDoseGreen > 0.0f) {
    softHalf = doseToHalfSeconds(calDoseGreen, metric);
    saveExposurePrefs();
  } else {
    rp2040BeepPulse(180);
  }
  displayDirty = true;
}

// Optional per-paper cross-sensitivity correction (see crossFactorPermille
// above): reduces one channel by a fraction of the other, computed fresh
// from whatever hardHalf/softHalf currently are -- never baked into a
// stored value, so there is no "raw vs. displayed" duality to keep in sync
// (that duality was the root cause of the old AUTO/MANUAL continuity bug
// this project deliberately removed). With crossFactorPermille == 0
// (default), hardOut/softOut come back unchanged.
//
// Sign convention: positive reduces SOFT by a fraction of HARD (documented
// direction -- the hard/blue exposure also lands on the soft/green-
// sensitized layer that controls highlights). Negative reduces HARD by a
// fraction of SOFT instead, kept available only in case real-world testing
// on a specific paper/light source ever shows a need for that direction.
static void applyCrossFactor(uint16_t hard, uint16_t soft, uint16_t &hardOut, uint16_t &softOut) {
  if (crossFactorPermille >= 0) {
    const uint32_t reduction = ((uint32_t)hard * (uint32_t)crossFactorPermille + 500U) / 1000U;
    hardOut = hard;
    softOut = (reduction >= soft) ? 0 : (uint16_t)(soft - reduction);
  } else {
    const uint32_t reduction = ((uint32_t)soft * (uint32_t)(-crossFactorPermille) + 500U) / 1000U;
    hardOut = (reduction >= hard) ? 0 : (uint16_t)(hard - reduction);
    softOut = soft;
  }
}

static void beginExposure(RunState state, uint16_t half0, uint16_t half1 = 0) {
  runState = state;
  remainingHalf = half0;
  splitSoftHalf = half1;
  nextTickAtMs = millis() + kTickMs;
  rp2040BeepPulse(90);
  displayDirty = true;
}

static void stopAll() {
  runState = RUN_IDLE;
  remainingHalf = 0;
  splitSoftHalf = 0;
  rp2040BeepPulse(70);
  sendCommandRetry(CMD_STOP);
  displayDirty = true;
}

// Fires the actual print exposure using the cross-corrected hard time. Called
// only from the EXPOSURE hold-candidate's short-tap release path (see
// handleTouch()) -- by the time this runs we already know runState was
// RUN_IDLE at press time and the touch didn't turn into a 3s XOVER TUNE hold.
static void doStartExposure() {
  uint16_t hardComp = 0;
  uint16_t softComp = 0;
  applyCrossFactor(hardHalf, softHalf, hardComp, softComp);
  if (hardComp == 0 && softComp == 0) {
    displayDirty = true;
    return;
  }
  if (hardComp == 0) {
    const bool ok = sendCommandRetry(CMD_EXPOSE_SOFT, softComp);
    noteTxResult(ok);
    if (ok) {
      beginExposure(RUN_SOFT, softComp);
    } else {
      rp2040BeepPulse(180);
    }
    return;
  }
  if (softComp == 0) {
    const bool ok = sendCommandRetry(CMD_EXPOSE_HARD, hardComp);
    noteTxResult(ok);
    if (ok) {
      beginExposure(RUN_HARD, hardComp);
    } else {
      rp2040BeepPulse(180);
    }
    return;
  }
  const bool ok = sendCommandRetry(CMD_EXPOSE_SPLIT, hardComp, softComp);
  noteTxResult(ok);
  if (ok) {
    beginExposure(RUN_SPLIT_HARD, hardComp, softComp);
  } else {
    rp2040BeepPulse(180);
  }
}

// ---------------------------------------------------------------------------
// Unified red-safelight design system. Everything stays in warm/red tones so
// the panel is safe to look at in the darkroom; hierarchy comes from
// brightness tiers, not new hues.
// ---------------------------------------------------------------------------
static uint16_t themeBg() { return 0x0800; }        // near-black red background
static uint16_t themeText() { return 0xFBEF; }      // warm light text (primary)
static uint16_t themeTextDim() { return 0xAD55; }   // warm dim text (secondary)
static uint16_t themeAccent() { return 0xF800; }    // pure red accent (headings)

static uint16_t colPrimary() { return 0xF800; }     // main action: EXPOSURE / SAVE
static uint16_t colSecondary() { return 0xC000; }   // FOCUS
static uint16_t colUtility() { return 0x6000; }     // MEAS / READ / BACK
static uint16_t colIncPlus() { return 0xA000; }     // "+" steppers
static uint16_t colIncMinus() { return 0x9000; }    // "-" steppers
static uint16_t colDisabledFill() { return 0x4208; }
static uint16_t colDisabledText() { return 0x8410; }
static uint16_t colBorderSubtle() { return 0x8410; }
static uint16_t colCardFill() { return 0x5000; }
static uint16_t colCardBorder() { return 0x9000; }

static uint16_t outlineFor(uint16_t fill) {
  if (fill == colDisabledFill()) return colDisabledText();
  if (fill == colPrimary()) return themeText();
  return colBorderSubtle();
}

// Draws text using the display driver's built-in opaque glyph fill
// (foreground + background painted together, per character, in one write)
// instead of the old "fillRect to erase, then print" two-step used all over
// this screen for values that update without a fullRedraw (CAL step count,
// CAL/LEDCFG result rows, XOVER dose, main-screen HARD/SOFT readout, ...).
//
// Why: this panel's framebuffer is continuously scanned out (no vsync-timed
// double buffer), so any moment where the screen holds "erased to background,
// new text not drawn yet" is a real, visible frame -- that gap was the
// flicker. Arduino_GFX's two-argument setTextColor(fg, bg) makes drawChar()
// fill each glyph's background cell and then draw its foreground pixels in
// the same write, so there is no erased-but-empty state to catch mid-scan.
//
// Callers MUST pass text already padded to the field's max width (e.g. via
// "%-10s"/"%3u"), otherwise trailing characters from a longer previous value
// won't be covered by the new (shorter) string's glyph cells and will ghost.
static void printOpaque(int16_t x, int16_t y, uint8_t textSize, uint16_t fg,
                         uint16_t bg, const char *text) {
  gfx->setTextSize(textSize);
  gfx->setTextColor(fg, bg);
  gfx->setCursor(x, y);
  gfx->print(text);
}

static void drawButton(const Btn &b) {
  uint16_t fill = b.enabled ? b.fill : colDisabledFill();
  uint16_t txt = b.enabled ? b.text : colDisabledText();
  gfx->fillRoundRect(b.x, b.y, b.w, b.h, 14, fill);
  gfx->drawRoundRect(b.x, b.y, b.w, b.h, 14, outlineFor(fill));
  const size_t len = strlen(b.label);
  uint8_t textSize = 2;
  if (len <= 4) {
    textSize = 3;
  } else if (len >= 13) {
    textSize = 1;
  }
  gfx->setTextSize(textSize);
  gfx->setTextColor(txt);
  const int16_t textW = (int16_t)len * 6 * textSize;
  const int16_t textH = 8 * textSize;
  int16_t tx = b.x + (b.w - textW) / 2;
  if (tx < (b.x + 8)) tx = b.x + 8;
  int16_t ty = b.y + (b.h - textH) / 2;
  if (ty < (b.y + 8)) ty = b.y + 8;
  gfx->setCursor(tx, ty);
  gfx->println(b.label);
}

static void drawStatePill(bool force) {
  static constexpr int16_t px = 16, py = 34, pw = 280, ph = 30;
  static bool statePillValid = false;
  static RunState lastRunState = (RunState)255;
  static uint16_t lastRemainingHalf = 0xFFFF;
  static uint16_t lastFill = 0xFFFF;
  const bool isDynamicState = !(runState == RUN_IDLE || runState == RUN_FOCUS);
  if (!force && statePillValid && runState == lastRunState &&
      (!isDynamicState || remainingHalf == lastRemainingHalf)) {
    return;
  }
  uint16_t fill = colUtility();
  char label[28];
  if (runState == RUN_IDLE) {
    fill = colUtility();
    snprintf(label, sizeof(label), "%-16s", "IDLE");
  } else if (runState == RUN_FOCUS) {
    fill = colSecondary();
    snprintf(label, sizeof(label), "%-16s", "FOCUS");
  } else {
    fill = colPrimary();
    const char *phase = (runState == RUN_HARD || runState == RUN_SPLIT_HARD) ? "HARD" : "SOFT";
    char raw[24];
    snprintf(raw, sizeof(raw), "%s  %.1fs", phase, remainingHalf / 2.0f);
    snprintf(label, sizeof(label), "%-16s", raw);
  }
  // The pill's rounded shape (and outline) only needs a fresh fill when its
  // color actually changes (idle/focus/exposing); the per-tick countdown
  // text update below is opaque against that fill, so it never needs the
  // shape re-filled just to redraw a changed digit.
  if (force || fill != lastFill) {
    gfx->fillRoundRect(px, py, pw, ph, 8, fill);
    gfx->drawRoundRect(px, py, pw, ph, 8, outlineFor(fill));
    lastFill = fill;
  }
  printOpaque(px + 12, py + 7, 2, themeText(), fill, label);
  statePillValid = true;
  lastRunState = runState;
  lastRemainingHalf = remainingHalf;
}

static void drawSensorPill(bool force) {
  static constexpr int16_t px = 16, py = 34, pw = 280, ph = 30;
  static bool sensorPillValid = false;
  static MeterSensorType lastSensor = (MeterSensorType)255;
  if (!force && sensorPillValid && meterSensor == lastSensor) {
    return;
  }
  gfx->fillRoundRect(px, py, pw, ph, 8, colUtility());
  gfx->drawRoundRect(px, py, pw, ph, 8, outlineFor(colUtility()));
  gfx->setTextSize(2);
  gfx->setTextColor(themeText());
  gfx->setCursor(px + 12, py + 7);
  gfx->printf("SENSOR: %s", meterSensorName());
  sensorPillValid = true;
  lastSensor = meterSensor;
}

static void drawHeader(const char *title, bool fullRedraw, uint8_t forcedTitleSize = 0,
                       bool showStatePill = true) {
  if (fullRedraw) {
    gfx->fillRect(0, 0, SCREEN_W, 72, themeBg());
    const size_t titleLen = strlen(title);
    const uint8_t titleSize = forcedTitleSize != 0 ? forcedTitleSize : (titleLen > 24 ? 1 : 2);
    gfx->setTextSize(titleSize);
    gfx->setTextColor(themeAccent());
    gfx->setCursor(16, 10);
    gfx->println(title);
  }
  if (showStatePill) {
    drawStatePill(fullRedraw);
  }
}

static void drawMainScreen(bool fullRedraw) {
  if (fullRedraw) {
    gfx->fillScreen(themeBg());
  }
  drawHeader("MILUKA SPLITGRADE CONTROLLER", fullRedraw, 2);

  if (fullRedraw) {
    Btn buttons[] = {
        {kMainX0 + 0 * (kMainColW + kMainGap), kMainTopY, kMainColW, kMainBtnH, "MEAS BLK", colUtility(), themeText(), true},
        {kMainX0 + 0 * (kMainColW + kMainGap), kMainBotY, kMainColW, kMainBtnH, "MEAS LIT", colUtility(), themeText(), true},
        {kMainX0 + 1 * (kMainColW + kMainGap), kMainMidTopY, kMainColW, kMainMidBtnH, "+", colIncPlus(), themeText(), true},
        {kMainX0 + 1 * (kMainColW + kMainGap), kMainMidBotY, kMainColW, kMainMidBtnH, "-", colIncMinus(), themeText(), true},
        {kMainX0 + 2 * (kMainColW + kMainGap), kMainMidTopY, kMainColW, kMainMidBtnH, "+", colIncPlus(), themeText(), true},
        {kMainX0 + 2 * (kMainColW + kMainGap), kMainMidBotY, kMainColW, kMainMidBtnH, "-", colIncMinus(), themeText(), true},
        {kMainX0 + 3 * (kMainColW + kMainGap), kMainTopY, kMainColW, kMainBtnH, "FOCUS", colSecondary(), themeText(), true},
        {kMainX0 + 3 * (kMainColW + kMainGap), kMainBotY, kMainColW, kMainBtnH, "EXPOSURE", colPrimary(), themeText(), true},
    };
    for (const Btn &b : buttons) {
      drawButton(b);
    }
  }

  // Time readout between + and - buttons (centered and product-style).
  // Card frame + labels are static chrome -- draw once on fullRedraw. Only
  // the two value strings actually change (on every +/- tap and every
  // countdown tick), so only they get redrawn each call, opaquely, with no
  // separate erase step (see printOpaque() for why).
  const int16_t infoY = 214;
  const int16_t infoH = 50;
  const int16_t infoXH = kMainX0 + 1 * (kMainColW + kMainGap) + 8;
  const int16_t infoXS = kMainX0 + 2 * (kMainColW + kMainGap) + 8;
  const int16_t infoW = kMainColW - 16;
  if (fullRedraw) {
    gfx->fillRoundRect(infoXH, infoY, infoW, infoH, 8, colCardFill());
    gfx->drawRoundRect(infoXH, infoY, infoW, infoH, 8, colCardBorder());
    gfx->fillRoundRect(infoXS, infoY, infoW, infoH, 8, colCardFill());
    gfx->drawRoundRect(infoXS, infoY, infoW, infoH, 8, colCardBorder());

    gfx->setTextColor(themeTextDim());
    gfx->setTextSize(2);
    const char *lblHard = "EXP HARD";
    const char *lblSoft = "EXP SOFT";
    const int16_t lblHardX = infoXH + (infoW - ((int)strlen(lblHard) * 12)) / 2;
    const int16_t lblSoftX = infoXS + (infoW - ((int)strlen(lblSoft) * 12)) / 2;
    gfx->setCursor(lblHardX, infoY + 4);
    gfx->print(lblHard);
    gfx->setCursor(lblSoftX, infoY + 4);
    gfx->print(lblSoft);
  }

  char hardRaw[10];
  char softRaw[10];
  char hardBuf[12];
  char softBuf[12];
  snprintf(hardRaw, sizeof(hardRaw), "%.1fs", hardHalf / 2.0f);
  snprintf(softRaw, sizeof(softRaw), "%.1fs", softHalf / 2.0f);
  snprintf(hardBuf, sizeof(hardBuf), "%-6s", hardRaw);
  snprintf(softBuf, sizeof(softBuf), "%-6s", softRaw);
  // Fixed-width field centered on the card -- width doesn't depend on the
  // actual digit count so the centering point (and opaque-erase coverage)
  // stays stable as the value grows/shrinks.
  const int16_t hardValX = infoXH + (infoW - (6 * 12)) / 2;
  const int16_t softValX = infoXS + (infoW - (6 * 12)) / 2;
  printOpaque(hardValX, infoY + 27, 2, themeText(), colCardFill(), hardBuf);
  printOpaque(softValX, infoY + 27, 2, themeText(), colCardFill(), softBuf);
}

// UI_CAL layout constants (single source of truth for draw + touch hitboxes).
// Two side-by-side channel columns (blue left, green right), a shared test
// time stepper + BACK row, and a results card at the bottom.
static constexpr int16_t kCalTitleY = 76;
static constexpr int16_t kCalRow1Y = 92;   // STEP -/+
static constexpr int16_t kCalRow2Y = 146;  // READ REF
static constexpr int16_t kCalRow3Y = 200;  // SAVE
static constexpr int16_t kCalRow4Y = 254;  // TEST TIME -/+, BACK
static constexpr int16_t kCalRowH = 48;
static constexpr int16_t kCalLeftX = 16;
static constexpr int16_t kCalRightX = 256;
static constexpr int16_t kCalHalfW = 208;
static constexpr int16_t kCalStepBtnW = 100;
static constexpr int16_t kCalCardX = 16;
static constexpr int16_t kCalCardY = 310;
static constexpr int16_t kCalCardW = SCREEN_W - 32;
static constexpr int16_t kCalCardH = 154;

static void drawCalScreen(bool fullRedraw) {
  if (fullRedraw) {
    gfx->fillScreen(themeBg());
  }
  drawHeader("CAL DOSE", fullRedraw, 0, false);
  drawSensorPill(fullRedraw);

  // Step readouts change on every +/- tap without a fullRedraw. Opaque,
  // fixed-width print instead of erase-then-print -- see printOpaque().
  char blueStepRaw[20];
  char greenStepRaw[20];
  char blueStepBuf[24];
  char greenStepBuf[24];
  snprintf(blueStepRaw, sizeof(blueStepRaw), "BLUE STEP %u", (unsigned)calStepBlue);
  snprintf(greenStepRaw, sizeof(greenStepRaw), "GREEN STEP %u", (unsigned)calStepGreen);
  snprintf(blueStepBuf, sizeof(blueStepBuf), "%-20s", blueStepRaw);
  snprintf(greenStepBuf, sizeof(greenStepBuf), "%-20s", greenStepRaw);
  printOpaque(kCalLeftX + 2, kCalTitleY, 1, themeTextDim(), themeBg(), blueStepBuf);
  printOpaque(kCalRightX + 2, kCalTitleY, 1, themeTextDim(), themeBg(), greenStepBuf);

  if (fullRedraw) {
    Btn buttons[] = {
        {kCalLeftX, kCalRow1Y, kCalStepBtnW, kCalRowH, "STEP -", colIncMinus(), themeText(), true},
        {kCalLeftX + kCalStepBtnW + 8, kCalRow1Y, kCalStepBtnW, kCalRowH, "STEP +", colIncPlus(), themeText(), true},
        {kCalRightX, kCalRow1Y, kCalStepBtnW, kCalRowH, "STEP -", colIncMinus(), themeText(), true},
        {kCalRightX + kCalStepBtnW + 8, kCalRow1Y, kCalStepBtnW, kCalRowH, "STEP +", colIncPlus(), themeText(), true},

        {kCalLeftX, kCalRow2Y, kCalHalfW, kCalRowH, "READ REF", colUtility(), themeText(), true},
        {kCalRightX, kCalRow2Y, kCalHalfW, kCalRowH, "READ REF", colUtility(), themeText(), true},

        {kCalLeftX, kCalRow3Y, kCalHalfW, kCalRowH, "SAVE", colPrimary(), themeText(), true},
        {kCalRightX, kCalRow3Y, kCalHalfW, kCalRowH, "SAVE", colPrimary(), themeText(), true},

        {kCalLeftX, kCalRow4Y, 140, kCalRowH, "TIME -", colIncMinus(), themeText(), true},
        {kCalLeftX + 148, kCalRow4Y, 140, kCalRowH, "TIME +", colIncPlus(), themeText(), true},
        {kCalLeftX + 148 + 140 + 8, kCalRow4Y, 152, kCalRowH, "BACK", colUtility(), themeText(), true},
    };
    for (const Btn &b : buttons) {
      drawButton(b);
    }
  }

  // Card frame + row labels are static -- draw once. Values are redrawn on
  // every call (opaque, fixed-width, no separate erase -- see printOpaque()).
  if (fullRedraw) {
    gfx->fillRoundRect(kCalCardX, kCalCardY, kCalCardW, kCalCardH, 10, colCardFill());
    gfx->drawRoundRect(kCalCardX, kCalCardY, kCalCardW, kCalCardH, 10, colCardBorder());
    gfx->setTextSize(2);
    gfx->setTextColor(themeTextDim());
    auto printLabel = [&](int16_t rowY, const char *label) {
      gfx->setCursor(kCalCardX + 12, rowY);
      gfx->print(label);
    };
    printLabel(kCalCardY + 8, "TEST TIME");
    printLabel(kCalCardY + 32, "BLUE REF");
    printLabel(kCalCardY + 56, "BLUE DOSE");
    printLabel(kCalCardY + 84, "GRN REF");
    printLabel(kCalCardY + 108, "GRN DOSE");
  }

  auto printValue = [&](int16_t rowY, const char *valueBuf) {
    printOpaque(kCalCardX + 170, rowY, 2, themeText(), colCardFill(), valueBuf);
  };

  char raw[16];
  char buf[16];
  snprintf(raw, sizeof(raw), "%.1fs", calTestHalfSeconds / 2.0f);
  snprintf(buf, sizeof(buf), "%-9s", raw);
  printValue(kCalCardY + 8, buf);

  // Three decimals: green target doses are legitimately tiny (a threshold
  // dose of ~0.03 lux*s is physically plausible), so one decimal would
  // display a real, saved value as "0.0" and look like a failed SAVE.
  if (calRefIntensityBlue > 0.01f) {
    snprintf(raw, sizeof(raw), "%.3f", calRefIntensityBlue);
  } else {
    snprintf(raw, sizeof(raw), "NOT READ");
  }
  snprintf(buf, sizeof(buf), "%-9s", raw);
  printValue(kCalCardY + 32, buf);

  if (calDoseBlue > 0.0f) {
    snprintf(raw, sizeof(raw), "%.3f", calDoseBlue);
  } else {
    snprintf(raw, sizeof(raw), "NOT SET");
  }
  snprintf(buf, sizeof(buf), "%-9s", raw);
  printValue(kCalCardY + 56, buf);

  if (calRefIntensityGreen > 0.01f) {
    snprintf(raw, sizeof(raw), "%.3f", calRefIntensityGreen);
  } else {
    snprintf(raw, sizeof(raw), "NOT READ");
  }
  snprintf(buf, sizeof(buf), "%-9s", raw);
  printValue(kCalCardY + 84, buf);

  if (calDoseGreen > 0.0f) {
    snprintf(raw, sizeof(raw), "%.3f", calDoseGreen);
  } else {
    snprintf(raw, sizeof(raw), "NOT SET");
  }
  snprintf(buf, sizeof(buf), "%-9s", raw);
  printValue(kCalCardY + 108, buf);
}

// UI_XOVER ("XOVER TUNE") layout constants -- a single hidden, optional
// value, reached by a 3s hold on EXPOSURE while idle (see docs/
// TECHNICAL_CONCEPT.md §5.4). Deliberately minimal: one value, two buttons,
// BACK.
static constexpr int16_t kXoverCardX = 16;
static constexpr int16_t kXoverCardY = 100;
static constexpr int16_t kXoverCardW = SCREEN_W - 32;
static constexpr int16_t kXoverCardH = 130;
static constexpr int16_t kXoverRow1Y = 250;
static constexpr int16_t kXoverRowH = 110;
static constexpr int16_t kXoverHalfW = 214;
static constexpr int16_t kXoverBackY = 380;
static constexpr int16_t kXoverBackH = 76;

static void drawXoverScreen(bool fullRedraw) {
  if (fullRedraw) {
    gfx->fillScreen(themeBg());
  }
  drawHeader("XOVER TUNE", fullRedraw, 0, false);

  // Card frame + "CROSS FACTOR" caption are static -- draw once. The value
  // and direction line change on every +/- tap, so they're redrawn every
  // call, opaquely, with no separate erase step (see printOpaque()).
  if (fullRedraw) {
    gfx->fillRoundRect(kXoverCardX, kXoverCardY, kXoverCardW, kXoverCardH, 10, colCardFill());
    gfx->drawRoundRect(kXoverCardX, kXoverCardY, kXoverCardW, kXoverCardH, 10, colCardBorder());
    gfx->setTextSize(2);
    gfx->setTextColor(themeTextDim());
    gfx->setCursor(kXoverCardX + 12, kXoverCardY + 10);
    gfx->print("CROSS FACTOR");
  }
  char buf[16];
  snprintf(buf, sizeof(buf), "%+.2f", crossFactorPermille / 1000.0f);
  printOpaque(kXoverCardX + 12, kXoverCardY + 30, 5, themeText(), colCardFill(), buf);
  const char *dir;
  if (crossFactorPermille > 0) {
    dir = "HARD -> SOFT (default)  ";
  } else if (crossFactorPermille < 0) {
    dir = "SOFT -> HARD (reverse)  ";
  } else {
    dir = "OFF (no correction)    ";
  }
  printOpaque(kXoverCardX + 12, kXoverCardY + 96, 2, themeTextDim(), colCardFill(), dir);

  if (fullRedraw) {
    Btn buttons[] = {
        {kXoverCardX, kXoverRow1Y, kXoverHalfW, kXoverRowH, "-", colIncMinus(), themeText(), true},
        {kXoverCardX + kXoverHalfW + 20, kXoverRow1Y, kXoverHalfW, kXoverRowH, "+", colIncPlus(), themeText(), true},
        {kXoverCardX, kXoverBackY, kXoverCardW, kXoverBackH, "BACK", colUtility(), themeText(), true},
    };
    for (const Btn &b : buttons) {
      drawButton(b);
    }
  }
}

// UI_LEDCFG ("LED PANEL CONFIG") layout constants -- open-source/field
// hardware config for the paired receiver board (this repo's
// darkroom-enlarger-head/ XIAO receiver): how many pixels the attached
// SK6812RGBW-NW panel actually has, and which GPIO it's wired to on that
// board. Reached by a 3s hold on MEAS LIT while idle (see
// docs/TECHNICAL_CONCEPT.md, docs/GLEDOPTO_BRINGUP_NOTES.md).
// Data pin is a free +/- adjustable number (like pixel count), not a
// toggle between two fixed pins: CMD_SET_LED_CONFIG only ever configures a
// *remote* board over ESP-NOW, so a bad value here cannot brick this
// sender's own boot -- only [kLedConfigMinPin, kLedConfigMaxPin] is
// enforced as a basic sanity bound. Picking a pin that isn't actually
// broken out/wired on the receiver is the user's own responsibility.
static constexpr int16_t kLedCfgCardX = 16;
static constexpr int16_t kLedCfgCardY = 84;
static constexpr int16_t kLedCfgCardW = SCREEN_W - 32;
static constexpr int16_t kLedCfgCardH = 70;
static constexpr int16_t kLedCfgCountRowY = 172;
static constexpr int16_t kLedCfgRowH = 90;
static constexpr int16_t kLedCfgHalfW = 214;
static constexpr int16_t kLedCfgPinRowY = 274;
static constexpr int16_t kLedCfgPinRowH = 70;
static constexpr int16_t kLedCfgBackY = 356;
static constexpr int16_t kLedCfgBackH = 60;

static void drawLedCfgScreen(bool fullRedraw) {
  if (fullRedraw) {
    gfx->fillScreen(themeBg());
  }
  drawHeader("LED PANEL CONFIG", fullRedraw, 0, false);

  // Card frame + row labels are static -- draw once. Values change on every
  // +/- tap and are redrawn opaquely every call, no separate erase step
  // needed (see printOpaque()).
  if (fullRedraw) {
    gfx->fillRoundRect(kLedCfgCardX, kLedCfgCardY, kLedCfgCardW, kLedCfgCardH, 10, colCardFill());
    gfx->drawRoundRect(kLedCfgCardX, kLedCfgCardY, kLedCfgCardW, kLedCfgCardH, 10, colCardBorder());
    gfx->setTextSize(2);
    gfx->setTextColor(themeTextDim());
    gfx->setCursor(kLedCfgCardX + 12, kLedCfgCardY + 10);
    gfx->print("PIXEL COUNT");
    gfx->setCursor(kLedCfgCardX + 12, kLedCfgCardY + 38);
    gfx->print("DATA PIN");
  }

  char raw[16];
  char buf[16];
  snprintf(raw, sizeof(raw), "%u", (unsigned)ledPixelCount);
  snprintf(buf, sizeof(buf), "%-6s", raw);
  printOpaque(kLedCfgCardX + 220, kLedCfgCardY + 10, 2, themeText(), colCardFill(), buf);
  snprintf(raw, sizeof(raw), "GPIO%u", (unsigned)ledDataPin);
  snprintf(buf, sizeof(buf), "%-8s", raw);
  printOpaque(kLedCfgCardX + 220, kLedCfgCardY + 38, 2, themeText(), colCardFill(), buf);

  if (fullRedraw) {
    const int16_t plusX = kLedCfgCardX + kLedCfgHalfW + 20;
    Btn buttons[] = {
        {kLedCfgCardX, kLedCfgCountRowY, kLedCfgHalfW, kLedCfgRowH, "-", colIncMinus(), themeText(), true},
        {plusX, kLedCfgCountRowY, kLedCfgHalfW, kLedCfgRowH, "+", colIncPlus(), themeText(), true},
        {kLedCfgCardX, kLedCfgPinRowY, kLedCfgHalfW, kLedCfgPinRowH, "-", colIncMinus(), themeText(), true},
        {plusX, kLedCfgPinRowY, kLedCfgHalfW, kLedCfgPinRowH, "+", colIncPlus(), themeText(), true},
        {kLedCfgCardX, kLedCfgBackY, kLedCfgCardW, kLedCfgBackH, "SAVE", colUtility(), themeText(), true},
    };
    for (const Btn &b : buttons) {
      drawButton(b);
    }
  }
}

static void redraw() {
  const bool fullRedraw = (uiScreen != lastDrawnScreen);
  lastDrawnScreen = uiScreen;
  switch (uiScreen) {
    case UI_CAL:
      drawCalScreen(fullRedraw);
      break;
    case UI_XOVER:
      drawXoverScreen(fullRedraw);
      break;
    case UI_LEDCFG:
      drawLedCfgScreen(fullRedraw);
      break;
    default:
      drawMainScreen(fullRedraw);
      break;
  }
}

static bool hit(int16_t x, int16_t y, const Btn &b) {
  return x >= b.x && x <= (b.x + b.w) && y >= b.y && y <= (b.y + b.h);
}

static void adjustHalfAllowZero(uint16_t &v, int delta) {
  int nv = (int)v + delta;
  if (nv < 0) {
    nv = 0;
  }
  if (nv > (int)kMaxHalfSeconds) {
    nv = kMaxHalfSeconds;
  }
  v = (uint16_t)nv;
}

static void adjustHalfMinOne(uint16_t &v, int delta) {
  int nv = (int)v + delta;
  if (nv < 1) {
    nv = 1;
  }
  if (nv > (int)kMaxHalfSeconds) {
    nv = kMaxHalfSeconds;
  }
  v = (uint16_t)nv;
}

static void adjustStep(uint8_t &step, int delta) {
  int nv = (int)step + delta;
  if (nv < (int)kStepwedgeMinStep) {
    nv = kStepwedgeMinStep;
  }
  if (nv > (int)kStepwedgeMaxStep) {
    nv = kStepwedgeMaxStep;
  }
  step = (uint8_t)nv;
}

static void adjustCrossFactor(int deltaSteps) {
  int nv = (int)crossFactorPermille + deltaSteps * (int)kCrossFactorStepPermille;
  if (nv < -(int)kCrossFactorMaxPermille) {
    nv = -(int)kCrossFactorMaxPermille;
  }
  if (nv > (int)kCrossFactorMaxPermille) {
    nv = kCrossFactorMaxPermille;
  }
  crossFactorPermille = (int16_t)nv;
}

static void adjustLedPixelCount(int delta) {
  int nv = (int)ledPixelCount + delta;
  if (nv < (int)kLedConfigMinPixels) {
    nv = kLedConfigMinPixels;
  }
  if (nv > (int)kLedConfigMaxPixels) {
    nv = kLedConfigMaxPixels;
  }
  ledPixelCount = (uint16_t)nv;
}

static void adjustLedDataPin(int delta) {
  int nv = (int)ledDataPin + delta;
  if (nv < (int)kLedConfigMinPin) {
    nv = kLedConfigMinPin;
  }
  if (nv > (int)kLedConfigMaxPin) {
    nv = kLedConfigMaxPin;
  }
  ledDataPin = (uint8_t)nv;
}

static void clearHoldRepeat() {
  holdRepeatAction = HOLD_NONE;
  holdRepeatStartedMs = 0;
  holdRepeatLastStepMs = 0;
  holdRepeatX = 0;
  holdRepeatY = 0;
  holdRepeatW = 0;
  holdRepeatH = 0;
}

static void startHoldRepeat(HoldRepeatAction action, int16_t x, int16_t y, int16_t w, int16_t h) {
  holdRepeatAction = action;
  holdRepeatStartedMs = millis();
  holdRepeatLastStepMs = holdRepeatStartedMs;
  holdRepeatX = x;
  holdRepeatY = y;
  holdRepeatW = w;
  holdRepeatH = h;
}

static bool isWithinHoldRepeat(int16_t x, int16_t y) {
  return x >= (holdRepeatX - kTouchHoldMoveTol) &&
         x <= (holdRepeatX + holdRepeatW + kTouchHoldMoveTol) &&
         y >= (holdRepeatY - kTouchHoldMoveTol) &&
         y <= (holdRepeatY + holdRepeatH + kTouchHoldMoveTol);
}

static void performHoldRepeatStep() {
  switch (holdRepeatAction) {
    case HOLD_MAIN_HARD_PLUS:
      adjustHalfAllowZero(hardHalf, +1);
      saveExposurePrefs();
      displayDirty = true;
      break;
    case HOLD_MAIN_HARD_MINUS:
      adjustHalfAllowZero(hardHalf, -1);
      saveExposurePrefs();
      displayDirty = true;
      break;
    case HOLD_MAIN_SOFT_PLUS:
      adjustHalfAllowZero(softHalf, +1);
      saveExposurePrefs();
      displayDirty = true;
      break;
    case HOLD_MAIN_SOFT_MINUS:
      adjustHalfAllowZero(softHalf, -1);
      saveExposurePrefs();
      displayDirty = true;
      break;
    case HOLD_CAL_TIME_PLUS:
      adjustHalfMinOne(calTestHalfSeconds, +1);
      displayDirty = true;
      break;
    case HOLD_CAL_TIME_MINUS:
      adjustHalfMinOne(calTestHalfSeconds, -1);
      displayDirty = true;
      break;
    case HOLD_CAL_STEP_BLUE_PLUS:
      adjustStep(calStepBlue, +1);
      displayDirty = true;
      break;
    case HOLD_CAL_STEP_BLUE_MINUS:
      adjustStep(calStepBlue, -1);
      displayDirty = true;
      break;
    case HOLD_CAL_STEP_GREEN_PLUS:
      adjustStep(calStepGreen, +1);
      displayDirty = true;
      break;
    case HOLD_CAL_STEP_GREEN_MINUS:
      adjustStep(calStepGreen, -1);
      displayDirty = true;
      break;
    case HOLD_XOVER_PLUS:
      adjustCrossFactor(+1);
      saveCrossFactor();
      displayDirty = true;
      break;
    case HOLD_XOVER_MINUS:
      adjustCrossFactor(-1);
      saveCrossFactor();
      displayDirty = true;
      break;
    case HOLD_LEDCFG_COUNT_PLUS:
      adjustLedPixelCount(+1);
      saveLedConfig();
      displayDirty = true;
      break;
    case HOLD_LEDCFG_COUNT_MINUS:
      adjustLedPixelCount(-1);
      saveLedConfig();
      displayDirty = true;
      break;
    case HOLD_LEDCFG_PIN_PLUS:
      adjustLedDataPin(+1);
      saveLedConfig();
      displayDirty = true;
      break;
    case HOLD_LEDCFG_PIN_MINUS:
      adjustLedDataPin(-1);
      saveLedConfig();
      displayDirty = true;
      break;
    default:
      break;
  }
}

static void updateHoldRepeatWhilePressed(int16_t x, int16_t y) {
  if (holdRepeatAction == HOLD_NONE) {
    return;
  }
  if (!isWithinHoldRepeat(x, y)) {
    clearHoldRepeat();
    return;
  }
  const uint32_t now = millis();
  const uint32_t heldMs = now - holdRepeatStartedMs;
  if (heldMs < kHoldStartDelayMs) {
    return;
  }
  uint32_t interval = kHoldRepeatSlowMs;
  if (heldMs >= 1200) {
    interval = kHoldRepeatFastMs;
  }
  if (heldMs >= 2500) {
    interval = kHoldRepeatTurboMs;
  }
  if ((now - holdRepeatLastStepMs) < interval) {
    return;
  }
  holdRepeatLastStepMs = now;
  performHoldRepeatStep();
}

static void handleMainTouch(int16_t x, int16_t y) {
  clearHoldRepeat();

  if (hit(x, y, {kMainX0 + 0 * (kMainColW + kMainGap), kMainTopY, kMainColW, kMainBtnH, "", 0, 0, true})) {
    // Reached only when this touch was NOT treated as a MEAS BLK hold
    // candidate (e.g. an exposure was running when pressed) -- safety no-op.
    return;
  }
  if (hit(x, y, {kMainX0 + 0 * (kMainColW + kMainGap), kMainBotY, kMainColW, kMainBtnH, "", 0, 0, true})) {
    // Reached only when this touch was NOT treated as a MEAS LIT hold
    // candidate (e.g. an exposure was running when pressed) -- safety no-op.
    return;
  }
  if (hit(x, y, {kMainX0 + 1 * (kMainColW + kMainGap), kMainMidTopY, kMainColW, kMainMidBtnH, "", 0, 0, true})) {
    playTapBeep();
    adjustHalfAllowZero(hardHalf, +1);
    startHoldRepeat(HOLD_MAIN_HARD_PLUS, kMainX0 + 1 * (kMainColW + kMainGap), kMainMidTopY, kMainColW, kMainMidBtnH);
    saveExposurePrefs();
    displayDirty = true;
    return;
  }
  if (hit(x, y, {kMainX0 + 1 * (kMainColW + kMainGap), kMainMidBotY, kMainColW, kMainMidBtnH, "", 0, 0, true})) {
    playTapBeep();
    adjustHalfAllowZero(hardHalf, -1);
    startHoldRepeat(HOLD_MAIN_HARD_MINUS, kMainX0 + 1 * (kMainColW + kMainGap), kMainMidBotY, kMainColW, kMainMidBtnH);
    saveExposurePrefs();
    displayDirty = true;
    return;
  }
  if (hit(x, y, {kMainX0 + 2 * (kMainColW + kMainGap), kMainMidTopY, kMainColW, kMainMidBtnH, "", 0, 0, true})) {
    playTapBeep();
    adjustHalfAllowZero(softHalf, +1);
    startHoldRepeat(HOLD_MAIN_SOFT_PLUS, kMainX0 + 2 * (kMainColW + kMainGap), kMainMidTopY, kMainColW, kMainMidBtnH);
    saveExposurePrefs();
    displayDirty = true;
    return;
  }
  if (hit(x, y, {kMainX0 + 2 * (kMainColW + kMainGap), kMainMidBotY, kMainColW, kMainMidBtnH, "", 0, 0, true})) {
    playTapBeep();
    adjustHalfAllowZero(softHalf, -1);
    startHoldRepeat(HOLD_MAIN_SOFT_MINUS, kMainX0 + 2 * (kMainColW + kMainGap), kMainMidBotY, kMainColW, kMainMidBtnH);
    saveExposurePrefs();
    displayDirty = true;
    return;
  }
  if (hit(x, y, {kMainX0 + 3 * (kMainColW + kMainGap), kMainTopY, kMainColW, kMainBtnH, "", 0, 0, true})) {
    playTapBeep();
    if (runState == RUN_FOCUS) {
      stopAll();
    } else if (runState != RUN_IDLE) {
      // Safety: during active exposure, first press on FOCUS acts as STOP.
      stopAll();
    } else {
      const bool ok = sendCommandRetry(CMD_FOCUS_ON);
      noteTxResult(ok);
      if (ok) {
        runState = RUN_FOCUS;
        displayDirty = true;
      } else {
        // Distinct longer beep for command-send failure.
        rp2040BeepPulse(180);
      }
    }
    return;
  }
  if (hit(x, y, {kMainX0 + 3 * (kMainColW + kMainGap), kMainBotY, kMainColW, kMainBtnH, "", 0, 0, true})) {
    // Starting a new exposure while idle is handled on release via the
    // EXPOSURE hold-candidate path in handleTouch() (doStartExposure()), so a
    // 3s hold can instead open the hidden XOVER TUNE menu. This press-dispatch
    // path is therefore only ever reached while an exposure/focus is already
    // running (runState != RUN_IDLE), in which case STOP still fires
    // immediately on press exactly as before -- unchanged, zero added
    // latency for the abort case.
    playTapBeep();
    if (runState != RUN_IDLE) {
      stopAll();
    }
    return;
  }
}

static void handleCalTouch(int16_t x, int16_t y) {
  clearHoldRepeat();
  const int16_t stepBluePlusX = kCalLeftX + kCalStepBtnW + 8;
  const int16_t stepGreenPlusX = kCalRightX + kCalStepBtnW + 8;
  const int16_t timePlusX = kCalLeftX + 148;
  const int16_t backX = kCalLeftX + 148 + 140 + 8;

  if (hit(x, y, {kCalLeftX, kCalRow1Y, kCalStepBtnW, kCalRowH, "", 0, 0, true})) {
    playTapBeep();
    adjustStep(calStepBlue, -1);
    startHoldRepeat(HOLD_CAL_STEP_BLUE_MINUS, kCalLeftX, kCalRow1Y, kCalStepBtnW, kCalRowH);
  } else if (hit(x, y, {stepBluePlusX, kCalRow1Y, kCalStepBtnW, kCalRowH, "", 0, 0, true})) {
    playTapBeep();
    adjustStep(calStepBlue, +1);
    startHoldRepeat(HOLD_CAL_STEP_BLUE_PLUS, stepBluePlusX, kCalRow1Y, kCalStepBtnW, kCalRowH);
  } else if (hit(x, y, {kCalRightX, kCalRow1Y, kCalStepBtnW, kCalRowH, "", 0, 0, true})) {
    playTapBeep();
    adjustStep(calStepGreen, -1);
    startHoldRepeat(HOLD_CAL_STEP_GREEN_MINUS, kCalRightX, kCalRow1Y, kCalStepBtnW, kCalRowH);
  } else if (hit(x, y, {stepGreenPlusX, kCalRow1Y, kCalStepBtnW, kCalRowH, "", 0, 0, true})) {
    playTapBeep();
    adjustStep(calStepGreen, +1);
    startHoldRepeat(HOLD_CAL_STEP_GREEN_PLUS, stepGreenPlusX, kCalRow1Y, kCalStepBtnW, kCalRowH);
  } else if (hit(x, y, {kCalLeftX, kCalRow2Y, kCalHalfW, kCalRowH, "", 0, 0, true})) {
    playTapBeep();
    // READ REF drives the head itself: blue light on, warm-up, averaged
    // read, light off -- same path as the daily MEAS BLK, so calibration
    // and daily metering see identical conditions.
    float metric = 0.0f;
    const bool ok = meterAtColor(CMD_METER_BLUE_ON, metric);
    if (ok) {
      calRefIntensityBlue = metric;
    } else {
      rp2040BeepPulse(180);
    }
  } else if (hit(x, y, {kCalRightX, kCalRow2Y, kCalHalfW, kCalRowH, "", 0, 0, true})) {
    playTapBeep();
    float metric = 0.0f;
    const bool ok = meterAtColor(CMD_METER_GREEN_ON, metric);
    if (ok) {
      calRefIntensityGreen = metric;
    } else {
      rp2040BeepPulse(180);
    }
  } else if (hit(x, y, {kCalLeftX, kCalRow3Y, kCalHalfW, kCalRowH, "", 0, 0, true})) {
    playTapBeep();
    const float dose = computeTargetDose(calRefIntensityBlue, calTestHalfSeconds, calStepBlue);
    if (dose > 0.0f) {
      calDoseBlue = dose;
      saveDoseBlue();
    } else {
      rp2040BeepPulse(180);
    }
  } else if (hit(x, y, {kCalRightX, kCalRow3Y, kCalHalfW, kCalRowH, "", 0, 0, true})) {
    playTapBeep();
    const float dose = computeTargetDose(calRefIntensityGreen, calTestHalfSeconds, calStepGreen);
    if (dose > 0.0f) {
      calDoseGreen = dose;
      saveDoseGreen();
    } else {
      rp2040BeepPulse(180);
    }
  } else if (hit(x, y, {kCalLeftX, kCalRow4Y, 140, kCalRowH, "", 0, 0, true})) {
    playTapBeep();
    adjustHalfMinOne(calTestHalfSeconds, -1);
    startHoldRepeat(HOLD_CAL_TIME_MINUS, kCalLeftX, kCalRow4Y, 140, kCalRowH);
  } else if (hit(x, y, {timePlusX, kCalRow4Y, 140, kCalRowH, "", 0, 0, true})) {
    playTapBeep();
    adjustHalfMinOne(calTestHalfSeconds, +1);
    startHoldRepeat(HOLD_CAL_TIME_PLUS, timePlusX, kCalRow4Y, 140, kCalRowH);
  } else if (hit(x, y, {backX, kCalRow4Y, 152, kCalRowH, "", 0, 0, true})) {
    playTapBeep();
    uiScreen = UI_MAIN;
  }
  displayDirty = true;
}

static void handleXoverTouch(int16_t x, int16_t y) {
  clearHoldRepeat();
  const int16_t plusX = kXoverCardX + kXoverHalfW + 20;

  if (hit(x, y, {kXoverCardX, kXoverRow1Y, kXoverHalfW, kXoverRowH, "", 0, 0, true})) {
    playTapBeep();
    adjustCrossFactor(-1);
    saveCrossFactor();
    startHoldRepeat(HOLD_XOVER_MINUS, kXoverCardX, kXoverRow1Y, kXoverHalfW, kXoverRowH);
  } else if (hit(x, y, {plusX, kXoverRow1Y, kXoverHalfW, kXoverRowH, "", 0, 0, true})) {
    playTapBeep();
    adjustCrossFactor(+1);
    saveCrossFactor();
    startHoldRepeat(HOLD_XOVER_PLUS, plusX, kXoverRow1Y, kXoverHalfW, kXoverRowH);
  } else if (hit(x, y, {kXoverCardX, kXoverBackY, kXoverCardW, kXoverBackH, "", 0, 0, true})) {
    playTapBeep();
    uiScreen = UI_MAIN;
  }
  displayDirty = true;
}

static void handleLedCfgTouch(int16_t x, int16_t y) {
  clearHoldRepeat();
  const int16_t plusX = kLedCfgCardX + kLedCfgHalfW + 20;

  if (hit(x, y, {kLedCfgCardX, kLedCfgCountRowY, kLedCfgHalfW, kLedCfgRowH, "", 0, 0, true})) {
    playTapBeep();
    adjustLedPixelCount(-1);
    saveLedConfig();
    startHoldRepeat(HOLD_LEDCFG_COUNT_MINUS, kLedCfgCardX, kLedCfgCountRowY, kLedCfgHalfW, kLedCfgRowH);
  } else if (hit(x, y, {plusX, kLedCfgCountRowY, kLedCfgHalfW, kLedCfgRowH, "", 0, 0, true})) {
    playTapBeep();
    adjustLedPixelCount(+1);
    saveLedConfig();
    startHoldRepeat(HOLD_LEDCFG_COUNT_PLUS, plusX, kLedCfgCountRowY, kLedCfgHalfW, kLedCfgRowH);
  } else if (hit(x, y, {kLedCfgCardX, kLedCfgPinRowY, kLedCfgHalfW, kLedCfgPinRowH, "", 0, 0, true})) {
    playTapBeep();
    adjustLedDataPin(-1);
    saveLedConfig();
    startHoldRepeat(HOLD_LEDCFG_PIN_MINUS, kLedCfgCardX, kLedCfgPinRowY, kLedCfgHalfW, kLedCfgPinRowH);
  } else if (hit(x, y, {plusX, kLedCfgPinRowY, kLedCfgHalfW, kLedCfgPinRowH, "", 0, 0, true})) {
    playTapBeep();
    adjustLedDataPin(+1);
    saveLedConfig();
    startHoldRepeat(HOLD_LEDCFG_PIN_PLUS, plusX, kLedCfgPinRowY, kLedCfgHalfW, kLedCfgPinRowH);
  } else if (hit(x, y, {kLedCfgCardX, kLedCfgBackY, kLedCfgCardW, kLedCfgBackH, "", 0, 0, true})) {
    playTapBeep();
    // SAVE is the only place this menu transmits, and it always sends
    // unconditionally (not just when something was actually edited) --
    // covering every +/- tap (count or pin) made since the menu was opened
    // in one transmission instead of one per edit, *and* doubling as the
    // explicit "resync this head" action (open the menu, tap SAVE straight
    // away, no edit needed) now that opening the menu itself no longer
    // sends anything on its own.
    sendLedConfig();
    uiScreen = UI_MAIN;
  }
  displayDirty = true;
}

static void handleTouch() {
  if (!touch_touched()) {
    if (measBlkHoldCandidate && !measBlkHoldTriggered && uiScreen == UI_MAIN && runState == RUN_IDLE) {
      // Short tap on MEAS BLK: meter blue light and update hard time.
      playTapBeep();
      doMeasureBlue();
    }
    if (measLitHoldCandidate && !measLitHoldTriggered && uiScreen == UI_MAIN && runState == RUN_IDLE) {
      // Short tap on MEAS LIT: meter green light and update soft time.
      playTapBeep();
      doMeasureGreen();
    }
    if (exposureHoldCandidate && !exposureHoldTriggered && uiScreen == UI_MAIN && runState == RUN_IDLE) {
      // Short tap on EXPOSURE while idle: actually start the exposure now
      // (deferred to release so a 3s hold can instead open XOVER TUNE).
      playTapBeep();
      doStartExposure();
    }
    measBlkHoldCandidate = false;
    measBlkHoldTriggered = false;
    measBlkHoldStartedMs = 0;
    measBlkHoldStartX = -1;
    measBlkHoldStartY = -1;
    measLitHoldCandidate = false;
    measLitHoldTriggered = false;
    measLitHoldStartedMs = 0;
    measLitHoldStartX = -1;
    measLitHoldStartY = -1;
    exposureHoldCandidate = false;
    exposureHoldTriggered = false;
    exposureHoldStartedMs = 0;
    exposureHoldStartX = -1;
    exposureHoldStartY = -1;
    clearHoldRepeat();
    lastTouchX = -1;
    lastTouchY = -1;
    touchPressedLatched = false;
    return;
  }

  const int16_t x = touch_last_x;
  const int16_t y = touch_last_y;

  if (touchPressedLatched) {
    if (measBlkHoldCandidate && !measBlkHoldTriggered && uiScreen == UI_MAIN) {
      const int16_t dx = abs(x - measBlkHoldStartX);
      const int16_t dy = abs(y - measBlkHoldStartY);
      if (dx > kTouchHoldMoveTol || dy > kTouchHoldMoveTol || !pointInMainMeasBlkButton(x, y)) {
        measBlkHoldCandidate = false;
      } else if (millis() - measBlkHoldStartedMs >= kMeasBlkHoldMs) {
        measBlkHoldTriggered = true;
        rp2040BeepPulse(120);
        uiScreen = UI_CAL;
        displayDirty = true;
      }
    }
    if (measLitHoldCandidate && !measLitHoldTriggered && uiScreen == UI_MAIN) {
      const int16_t dx = abs(x - measLitHoldStartX);
      const int16_t dy = abs(y - measLitHoldStartY);
      if (dx > kTouchHoldMoveTol || dy > kTouchHoldMoveTol || !pointInMainMeasLitButton(x, y)) {
        measLitHoldCandidate = false;
      } else if (millis() - measLitHoldStartedMs >= kMeasLitHoldMs) {
        measLitHoldTriggered = true;
        rp2040BeepPulse(120);
        uiScreen = UI_LEDCFG;
        // Deliberately no send here just from opening the menu -- that
        // would silently transmit (and flash the real head) every time
        // someone opens this screen just to check the current values. The
        // explicit SAVE tap (handleLedCfgTouch()) is the only place this
        // menu ever transmits, and it always sends unconditionally, which
        // also covers the "resync a head that lost its config" case: open
        // the menu and tap SAVE right away, no edit required.
        displayDirty = true;
      }
    }
    if (exposureHoldCandidate && !exposureHoldTriggered && uiScreen == UI_MAIN) {
      const int16_t dx = abs(x - exposureHoldStartX);
      const int16_t dy = abs(y - exposureHoldStartY);
      if (dx > kTouchHoldMoveTol || dy > kTouchHoldMoveTol || !pointInMainExposureButton(x, y)) {
        exposureHoldCandidate = false;
      } else if (millis() - exposureHoldStartedMs >= kExposureHoldMs) {
        exposureHoldTriggered = true;
        rp2040BeepPulse(120);
        uiScreen = UI_XOVER;
        displayDirty = true;
      }
    }
    updateHoldRepeatWhilePressed(x, y);
    return;
  }
  touchPressedLatched = true;
  measBlkHoldStartedMs = millis();
  measBlkHoldStartX = x;
  measBlkHoldStartY = y;
  measBlkHoldTriggered = false;
  measBlkHoldCandidate = (uiScreen == UI_MAIN && runState == RUN_IDLE && pointInMainMeasBlkButton(x, y));
  measLitHoldStartedMs = measBlkHoldStartedMs;
  measLitHoldStartX = x;
  measLitHoldStartY = y;
  measLitHoldTriggered = false;
  measLitHoldCandidate = (uiScreen == UI_MAIN && runState == RUN_IDLE && pointInMainMeasLitButton(x, y));
  exposureHoldStartedMs = measBlkHoldStartedMs;
  exposureHoldStartX = x;
  exposureHoldStartY = y;
  exposureHoldTriggered = false;
  // Only a hold-candidate while idle: if a print/focus is running, this press
  // must fall straight through to handleMainTouch()'s instant STOP dispatch.
  exposureHoldCandidate = (uiScreen == UI_MAIN && runState == RUN_IDLE && pointInMainExposureButton(x, y));
  if (measBlkHoldCandidate || measLitHoldCandidate || exposureHoldCandidate) {
    clearHoldRepeat();
    return;
  }

  const uint32_t now = millis();
  if (now - lastTouchActionMs < 120) {
    return;
  }
  lastTouchActionMs = now;
  lastTouchX = x;
  lastTouchY = y;

  switch (uiScreen) {
    case UI_CAL:
      handleCalTouch(x, y);
      break;
    case UI_XOVER:
      handleXoverTouch(x, y);
      break;
    case UI_LEDCFG:
      handleLedCfgTouch(x, y);
      break;
    default:
      handleMainTouch(x, y);
      break;
  }
}

static void updateTimer() {
  if (runState == RUN_IDLE || runState == RUN_FOCUS) {
    return;
  }
  if (millis() < nextTickAtMs) {
    return;
  }
  nextTickAtMs += kTickMs;
  if (remainingHalf > 0) {
    --remainingHalf;
    if ((remainingHalf % 2) == 0) {
      rp2040BeepPulse(35);
    }
    displayDirty = true;
  }
  if (remainingHalf != 0) {
    return;
  }
  if (runState == RUN_SPLIT_HARD && splitSoftHalf > 0) {
    runState = RUN_SPLIT_SOFT;
    remainingHalf = splitSoftHalf;
    splitSoftHalf = 0;
    displayDirty = true;
    return;
  }
  runState = RUN_IDLE;
  rp2040BeepPulse(120);
  displayDirty = true;
}

static void initEspNow() {
  WiFi.mode(WIFI_STA);
  esp_err_t startRet = esp_wifi_start();
  if (startRet != ESP_OK && startRet != ESP_ERR_WIFI_CONN) {
    Serial.printf("esp_wifi_start failed: %d\n", (int)startRet);
  }

  esp_wifi_set_promiscuous(true);
  esp_err_t chRet = esp_wifi_set_channel(kEspNowChannel, WIFI_SECOND_CHAN_NONE);
  esp_wifi_set_promiscuous(false);

  if (esp_now_init() != ESP_OK) {
    Serial.println("ESP-NOW init failed");
    return;
  }

  esp_now_peer_info_t peer{};
  memcpy(peer.peer_addr, kDeviceMacA, 6);
  peer.ifidx = WIFI_IF_STA;
  peer.channel = 0; // current channel
  peer.encrypt = false;
  esp_err_t p1 = esp_now_add_peer(&peer);

  memset(&peer, 0, sizeof(peer));
  memcpy(peer.peer_addr, kDeviceMacB, 6);
  peer.ifidx = WIFI_IF_STA;
  peer.channel = 0;
  peer.encrypt = false;
  esp_err_t p2 = esp_now_add_peer(&peer);

  memset(&peer, 0, sizeof(peer));
  memcpy(peer.peer_addr, kBroadcastMac, 6);
  peer.ifidx = WIFI_IF_STA;
  peer.channel = 0;
  peer.encrypt = false;
  esp_err_t pb = esp_now_add_peer(&peer);

  if (esp_wifi_get_mac(WIFI_IF_STA, selfMac) == ESP_OK) {
    selfMacReady = true;
    Serial.printf("Self MAC: %02X:%02X:%02X:%02X:%02X:%02X\n",
                  selfMac[0], selfMac[1], selfMac[2],
                  selfMac[3], selfMac[4], selfMac[5]);
  }
  uint8_t ch = 0;
  wifi_second_chan_t second = WIFI_SECOND_CHAN_NONE;
  esp_wifi_get_channel(&ch, &second);
  Serial.printf("ESP-NOW ready, set_channel=%d, channel=%u, peers=%d/%d/%d\n",
                (int)chRet, (unsigned)ch, (int)p1, (int)p2, (int)pb);
}

void setup() {
  Serial.begin(115200);
  delay(500);
  Serial.println("darkroom-sender boot");

  prefs.begin("darkroom", false);
  loadPrefs();

  if (!initDisplay()) {
    Serial.println("Display init failed");
  }

  touch_init(SCREEN_W, SCREEN_H, 0);
  touch_set_orientation_defaults(kDisplayRotation);
  applyTouchOrientation();
  initRp2040Link();
  initEspNow();
  setBacklight(backlightDuty);
  displayDirty = true;
}

void loop() {
  updateRp2040Link();
  handleTouch();
  updateTimer();

  if (displayDirty) {
    redraw();
    displayDirty = false;
  }

  delay(10);
}
