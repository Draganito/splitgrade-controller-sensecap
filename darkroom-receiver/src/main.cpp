#include <Arduino.h>
#include <Wire.h>
#include <WiFi.h>
#include <esp_now.h>
#include <esp_wifi.h>
#include <Preferences.h>
#include <Arduino_GFX_Library.h>
#include "Indicator_SWSPI.h"
#include "protocol.h"

// ---------------------------------------------------------------------------
// darkroom-receiver — simulates the enlarger head (SenseCAP D2).
//
// Receives fire-and-forget ESP-NOW commands, validates CRC locally, runs an
// autonomous millis() timer (0.5 s steps). Split grade: HARD (blue) then SOFT
// (green). Full-screen colour at 100 % brightness; minimal red countdown text.
//
// Notes:
// - This side must stay autonomous: timers keep running even if sender power
//   drops right after command transmission.
// - Display init uses the same expander-aware bus strategy as sender, with
//   retry logic to avoid backlight-only cold-boot regressions.
// ---------------------------------------------------------------------------

#define GFX_BL 45

Indicator_SWSPI *bus = new Indicator_SWSPI(
    5 /* RST (expander) */, 4 /* CS (expander) */,
    41 /* SCK */, 48 /* MOSI */);

Arduino_ESP32RGBPanel *rgbpanel = new Arduino_ESP32RGBPanel(
    18, 17, 16, 21,
    4, 3, 2, 1, 0,
    10, 9, 8, 7, 6, 5,
    15, 14, 13, 12, 11,
    1, 10, 8, 50,
    1, 10, 8, 20);

Arduino_RGB_Display *gfx = new Arduino_RGB_Display(
    480, 480, rgbpanel, 2, true,
    bus, GFX_NOT_DEFINED,
    st7701_type1_init_operations, sizeof(st7701_type1_init_operations));

enum RunMode : uint8_t {
  MODE_IDLE = 0,
  MODE_FOCUS,
  MODE_METER_BLUE,   // steady blue light, no timer -- shadow-point metering
  MODE_METER_GREEN,  // steady green light, no timer -- highlight-point metering
  MODE_HARD,
  MODE_SOFT,
  MODE_SPLIT_HARD,
  MODE_SPLIT_SOFT,
};

Preferences prefs;
RunMode runMode = MODE_IDLE;
uint16_t remainingHalfSeconds = 0;
uint16_t splitSoftHalfSeconds = 0;
uint32_t nextTickAtMs = 0;
bool displayDirty = true;
bool displayReady = false;
bool paired = false;
bool pairAnimationPending = false;
uint8_t pairedSenderMac[6] = {0};
volatile uint32_t rxPacketCount = 0;
volatile uint8_t lastCmd = 0;

// Deferred, same reasoning as pairAnimationPending above: onEspNowRecv() (and
// therefore handleCommand()) runs in the ESP-NOW/WiFi task context, not the
// main loop() task. Calling gfx-> drawing calls directly from there caused
// visible framebuffer corruption (checkerboard noise) on this RGB-panel
// display, since its DMA scanout and a foreign task's writes to the same
// framebuffer aren't synchronized. The actual drawing now only ever happens
// from runLedConfigFlash(), called from loop() -- see showLedConfigReceived().
volatile bool ledConfigFlashPending = false;
volatile uint16_t ledConfigFlashCount = 0;
volatile uint8_t ledConfigFlashPin = 0;

// sendLedConfig() on the sender fires this command as up to 3 retry attempts
// (sendCommandRetry(), 12 ms apart) and each attempt itself broadcasts *and*
// unicasts (sendCommand()) -- deliberate redundancy against ESP-NOW's lack
// of delivery guarantees. That means one real SAVE tap can arrive here as
// several identical back-to-back packets within well under 100 ms, all long
// before the first one's 1500 ms confirmation display finishes. Without
// de-duplication, each identical duplicate re-arms ledConfigFlashPending,
// so the display briefly returns to the normal IDLE status (visible black
// flash) and then shows "LED CONFIG RECEIVED" a second time for the same
// logical action. This tracks the last value actually acted on and ignores
// exact repeats seen again within kLedConfigDedupeMs -- short enough to
// only ever swallow same-burst retries, not a genuine second SAVE tap.
static constexpr uint32_t kLedConfigDedupeMs = 500;
uint16_t lastLedConfigCount = 0;
uint8_t lastLedConfigPin = 0;
uint32_t lastLedConfigAtMs = 0;
bool lastLedConfigSeen = false;

static bool initDisplay() {
  pinMode(GFX_BL, OUTPUT);
  digitalWrite(GFX_BL, LOW);

  for (uint8_t attempt = 1; attempt <= 4; ++attempt) {
    if (gfx->begin()) {
      digitalWrite(GFX_BL, HIGH);
      return true;
    }
    delay(120);
  }
  return false;
}

static void printMac(const uint8_t *mac) {
  Serial.printf("%02X:%02X:%02X:%02X:%02X:%02X\n",
                mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
}

static bool macEqual(const uint8_t *a, const uint8_t *b) {
  return memcmp(a, b, 6) == 0;
}

static void savePairedMac(const uint8_t *mac) {
  memcpy(pairedSenderMac, mac, 6);
  prefs.putBytes("sender_mac", pairedSenderMac, sizeof(pairedSenderMac));
  paired = true;
}

static void loadPairedMac() {
  paired = true;
}

static void setRunState(RunMode mode, uint16_t halfSeconds, uint16_t softHalfSeconds = 0) {
  runMode = mode;
  remainingHalfSeconds = halfSeconds;
  splitSoftHalfSeconds = softHalfSeconds;
  nextTickAtMs = millis() + kTickMs;
  displayDirty = true;
}

static void drawStatus() {
  if (!displayReady) {
    return;
  }

  uint16_t bg = BLACK;
  const char *phase = "IDLE";

  switch (runMode) {
    case MODE_FOCUS:
      bg = WHITE;
      phase = "FOCUS";
      break;
    case MODE_METER_BLUE:
      bg = BLUE;
      phase = "METER BLUE";
      break;
    case MODE_METER_GREEN:
      bg = GREEN;
      phase = "METER GREEN";
      break;
    case MODE_HARD:
    case MODE_SPLIT_HARD:
      bg = BLUE;
      phase = "HARD";
      break;
    case MODE_SOFT:
    case MODE_SPLIT_SOFT:
      bg = GREEN;
      phase = "SOFT";
      break;
    default:
      bg = BLACK;
      phase = paired ? "IDLE" : "PAIR";
      break;
  }

  gfx->fillScreen(bg);
  gfx->setTextWrap(false);
  gfx->setTextColor(RED);
  gfx->setTextSize(4);
  gfx->setCursor(20, 30);
  gfx->println(phase);

  if (runMode == MODE_IDLE) {
    gfx->setTextSize(3);
    gfx->setCursor(20, 110);
    gfx->println(paired ? "READY" : "WAIT PAIR");
  } else if (runMode == MODE_FOCUS) {
    gfx->setTextSize(3);
    gfx->setCursor(20, 110);
    gfx->println("press STOP");
  } else if (runMode == MODE_METER_BLUE || runMode == MODE_METER_GREEN) {
    gfx->setTextSize(3);
    gfx->setCursor(20, 110);
    gfx->println("metering...");
  } else {
    char buf[16];
    snprintf(buf, sizeof(buf), "%.1f", halfSecondsToSeconds(remainingHalfSeconds));
    gfx->setTextSize(6);
    gfx->setCursor(20, 130);
    gfx->println(buf);
  }

  gfx->drawFastHLine(20, 400, 440, RED);
  gfx->setTextSize(2);
  gfx->setCursor(20, 430);
  gfx->setTextColor(RED);
  gfx->printf("RX %lu CMD 0x%02X", (unsigned long)rxPacketCount, lastCmd);
}

static void runPairAnimation() {
  if (!displayReady || !pairAnimationPending) {
    return;
  }
  pairAnimationPending = false;

  gfx->fillScreen(WHITE);
  delay(300);
  gfx->fillScreen(GREEN);
  delay(300);
  gfx->fillScreen(BLUE);
  delay(300);
  displayDirty = true;
}

// Bench-verification-only helper: this simulated head has a display, not an
// LED strip, so it obviously can't apply CMD_SET_LED_CONFIG the way the real
// darkroom-gledopto head does (see docs/GLEDOPTO_BRINGUP_NOTES.md for that).
// This just proves the command actually arrives correctly over ESP-NOW and
// with the right values, by briefly showing them full-screen -- the display
// equivalent of the real head's RGBW confirmation flash. Purely diagnostic;
// nothing here is persisted or fed back into this receiver's own state.
//
// IMPORTANT: only ever call this from loop() (via runLedConfigFlash()),
// never directly from handleCommand()/onEspNowRecv() -- those run in the
// ESP-NOW/WiFi task context, and drawing to this RGB-panel display from
// that foreign task caused visible framebuffer corruption (checkerboard
// noise), since its DMA scanout isn't synchronized against writes from
// another task. Same reasoning as pairAnimationPending/runPairAnimation().
static void showLedConfigReceived(uint16_t pixelCount, uint8_t pin) {
  if (!displayReady) {
    return;
  }
  gfx->fillScreen(WHITE);
  gfx->setTextWrap(false);
  gfx->setTextColor(BLUE);
  gfx->setTextSize(3);
  gfx->setCursor(20, 120);
  gfx->println("LED CONFIG");
  gfx->setCursor(20, 160);
  gfx->println("RECEIVED");
  gfx->setTextColor(RED);
  gfx->setTextSize(5);
  gfx->setCursor(20, 230);
  gfx->printf("%u px", pixelCount);
  gfx->setCursor(20, 300);
  gfx->printf("GPIO%u", pin);
  delay(1500);
  displayDirty = true; // restores the normal status screen afterward
}

static void runLedConfigFlash() {
  if (!displayReady || !ledConfigFlashPending) {
    return;
  }
  ledConfigFlashPending = false;
  showLedConfigReceived(ledConfigFlashCount, ledConfigFlashPin);
}

static void handleCommand(const ControlPacket &pkt) {
  switch (pkt.cmd) {
    case CMD_FOCUS_ON:
      // Ignore focus requests while an exposure is running.
      if (runMode == MODE_IDLE || runMode == MODE_FOCUS) {
        setRunState(MODE_FOCUS, 0);
      }
      break;

    case CMD_METER_BLUE_ON:
      // Only allowed from idle or the other meter mode -- never during a timed exposure.
      if (runMode == MODE_IDLE || runMode == MODE_METER_BLUE || runMode == MODE_METER_GREEN) {
        setRunState(MODE_METER_BLUE, 0);
      }
      break;

    case CMD_METER_GREEN_ON:
      // Only allowed from idle or the other meter mode -- never during a timed exposure.
      if (runMode == MODE_IDLE || runMode == MODE_METER_BLUE || runMode == MODE_METER_GREEN) {
        setRunState(MODE_METER_GREEN, 0);
      }
      break;

    case CMD_EXPOSE_HARD:
      if (runMode != MODE_FOCUS && runMode != MODE_METER_BLUE && runMode != MODE_METER_GREEN &&
          pkt.arg0 >= 1 && pkt.arg0 <= kMaxHalfSeconds) {
        setRunState(MODE_HARD, pkt.arg0);
      }
      break;

    case CMD_EXPOSE_SOFT:
      if (runMode != MODE_FOCUS && runMode != MODE_METER_BLUE && runMode != MODE_METER_GREEN &&
          pkt.arg0 >= 1 && pkt.arg0 <= kMaxHalfSeconds) {
        setRunState(MODE_SOFT, pkt.arg0);
      }
      break;

    case CMD_EXPOSE_SPLIT:
      if (runMode != MODE_FOCUS && runMode != MODE_METER_BLUE && runMode != MODE_METER_GREEN &&
          pkt.arg0 >= 1 && pkt.arg0 <= kMaxHalfSeconds &&
          pkt.arg1 >= 1 && pkt.arg1 <= kMaxHalfSeconds) {
        setRunState(MODE_SPLIT_HARD, pkt.arg0, pkt.arg1);
      }
      break;

    case CMD_STOP:
      setRunState(MODE_IDLE, 0);
      break;

    case CMD_SET_LED_CONFIG: {
      // Same acceptance guard as the real darkroom-gledopto head (idle-only,
      // same range/pin validation) so this bench stand-in behaves the same
      // way for verification purposes. Only sets a pending flag here --
      // handleCommand() runs in the ESP-NOW/WiFi task context, so the
      // actual display draw must happen later from loop() (see
      // runLedConfigFlash()/showLedConfigReceived() above).
      const bool pinValid = (pkt.arg1 == kLedConfigPinA || pkt.arg1 == kLedConfigPinB);
      const bool countValid = (pkt.arg0 >= kLedConfigMinPixels && pkt.arg0 <= kLedConfigMaxPixels);
      const uint32_t now = millis();
      const bool isDuplicateRetry = lastLedConfigSeen && pkt.arg0 == lastLedConfigCount &&
                                     pkt.arg1 == lastLedConfigPin && (now - lastLedConfigAtMs) < kLedConfigDedupeMs;
      if (runMode == MODE_IDLE && pinValid && countValid && !isDuplicateRetry) {
        Serial.printf("LED CONFIG received (display-only bench check): %u px on GPIO%u\n",
                      pkt.arg0, pkt.arg1);
        lastLedConfigCount = pkt.arg0;
        lastLedConfigPin = (uint8_t)pkt.arg1;
        lastLedConfigAtMs = now;
        lastLedConfigSeen = true;
        ledConfigFlashCount = pkt.arg0;
        ledConfigFlashPin = (uint8_t)pkt.arg1;
        ledConfigFlashPending = true;
      }
      break;
    }

    default:
      break;
  }
}

static void onEspNowRecv(const uint8_t *mac, const uint8_t *data, int len) {
  ControlPacket pkt{};
  if (!validatePacket(data, len, pkt)) {
    return;
  }
  rxPacketCount++;
  lastCmd = pkt.cmd;
  displayDirty = true;

  // Accept any valid sender for now and show who sent the packet.
  Serial.print("RX from: ");
  printMac(mac);
  Serial.printf("CMD=0x%02X arg0=%u arg1=%u\n", pkt.cmd, pkt.arg0, pkt.arg1);

  handleCommand(pkt);
}

static void updateTimer() {
  if (runMode == MODE_IDLE || runMode == MODE_FOCUS) {
    return;
  }
  if (millis() < nextTickAtMs) {
    return;
  }

  nextTickAtMs += kTickMs;
  if (remainingHalfSeconds > 0) {
    --remainingHalfSeconds;
    displayDirty = true;
  }

  if (remainingHalfSeconds != 0) {
    return;
  }

  if (runMode == MODE_SPLIT_HARD && splitSoftHalfSeconds > 0) {
    runMode = MODE_SPLIT_SOFT;
    remainingHalfSeconds = splitSoftHalfSeconds;
    splitSoftHalfSeconds = 0;
    displayDirty = true;
    return;
  }

  runMode = MODE_IDLE;
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
  esp_now_register_recv_cb(onEspNowRecv);
  uint8_t ch = 0;
  wifi_second_chan_t second = WIFI_SECOND_CHAN_NONE;
  esp_wifi_get_channel(&ch, &second);
  Serial.printf("ESP-NOW ready, recv callback active, set_channel=%d, channel=%u\n",
                (int)chRet, (unsigned)ch);
}

void setup() {
  Serial.begin(115200);
  delay(500);
  Serial.println("darkroom-receiver boot");

  prefs.begin("darkroom", false);
  loadPairedMac();

  displayReady = initDisplay();
  if (!displayReady) {
    Serial.println("Display init failed");
  }

  initEspNow();
  displayDirty = true;
}

void loop() {
  runPairAnimation();
  runLedConfigFlash();
  updateTimer();

  if (displayDirty) {
    drawStatus();
    displayDirty = false;
  }

  delay(5);
}
