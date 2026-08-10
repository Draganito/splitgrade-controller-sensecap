#include <Arduino.h>
#include <Wire.h>
#include <PacketSerial.h>
#include <Adafruit_TSL2591.h>
#include <Adafruit_AS7343.h>

// Packet types shared with darkroom-sender (ESP32 side).
static constexpr uint8_t PKT_TYPE_CMD_BEEP_ON = 0xA1;
static constexpr uint8_t PKT_TYPE_CMD_BEEP_OFF = 0xA2;
static constexpr uint8_t PKT_TYPE_CMD_EXT_SAMPLE_NOW = 0xA6;
static constexpr uint8_t PKT_TYPE_SENSOR_EXT_TSL2591_LUX = 0xB6;
// Packet byte values (0xB7/0xB8) are unchanged for wire compatibility, but the
// underlying AS7343 channel and naming were corrected to match the real
// SK6812RGBW-NW LED peaks planned for the future enlarger head:
// Blue 460-470nm -> AS7343 F3 (475nm), Green 515-530nm -> AS7343 F4 (515nm).
// Previously this used F2 (425nm, violet-blue) and F5 (550nm, green-yellow),
// which were both a poor spectral match for this specific LED's real peaks.
static constexpr uint8_t PKT_TYPE_SENSOR_EXT_AS7343_BLUE = 0xB7;
static constexpr uint8_t PKT_TYPE_SENSOR_EXT_AS7343_GREEN = 0xB8;

static constexpr uint8_t kBuzzerPin = 19;
static constexpr uint16_t kDefaultBeepMs = 70;
static constexpr uint16_t kBeepHz = 2600;
static constexpr uint8_t kBeepDuty = 127;

PacketSerial packet;
Adafruit_TSL2591 tsl = Adafruit_TSL2591(2591);
Adafruit_AS7343 as7343;

bool hasTsl = false;
bool hasAs = false;
uint32_t nextPeriodicReadMs = 0;
uint32_t buzzerOffAtMs = 0;

// TSL2591 AGC (gain only; integration time stays fixed at 100ms so response
// latency remains bounded/predictable). The Adafruit library normalizes lux
// by gain/integration time internally (see calculateLux's `cpl` factor), so
// changing gain between reads does not bias the sender's ratio-based
// exposure math (CAL REF / READ comparisons stay valid across gain changes).
static tsl2591Gain_t tslGain = TSL2591_GAIN_MED;
static constexpr uint16_t kTslLowSignalCounts = 100;  // near noise floor, out of 65535

static bool tslStepGainDown() {
  switch (tslGain) {
    case TSL2591_GAIN_MAX:
      tslGain = TSL2591_GAIN_HIGH;
      break;
    case TSL2591_GAIN_HIGH:
      tslGain = TSL2591_GAIN_MED;
      break;
    case TSL2591_GAIN_MED:
      tslGain = TSL2591_GAIN_LOW;
      break;
    default:
      return false;  // already at LOW; sensor is simply too bright for it
  }
  tsl.setGain(tslGain);
  return true;
}

static bool tslStepGainUp() {
  switch (tslGain) {
    case TSL2591_GAIN_LOW:
      tslGain = TSL2591_GAIN_MED;
      break;
    case TSL2591_GAIN_MED:
      tslGain = TSL2591_GAIN_HIGH;
      break;
    case TSL2591_GAIN_HIGH:
      tslGain = TSL2591_GAIN_MAX;
      break;
    default:
      return false;  // already at MAX
  }
  tsl.setGain(tslGain);
  return true;
}

static void sendFloatPacket(uint8_t type, float value) {
  uint8_t buf[5];
  buf[0] = type;
  memcpy(&buf[1], &value, sizeof(value));
  packet.send(buf, sizeof(buf));
}

static void buzzerOn(uint16_t durationMs) {
  analogWriteFreq(kBeepHz);
  analogWrite(kBuzzerPin, kBeepDuty);
  buzzerOffAtMs = millis() + durationMs;
}

static void buzzerOff() {
  analogWrite(kBuzzerPin, 0);
  buzzerOffAtMs = 0;
}

static void readAndSendSensors() {
  if (hasTsl) {
    uint32_t lum = tsl.getFullLuminosity();
    uint16_t ir = (uint16_t)(lum >> 16);
    uint16_t full = (uint16_t)(lum & 0xFFFF);

    if (full == 0xFFFF || ir == 0xFFFF) {
      // Saturated: drop gain one step and resample once before giving up on
      // this cycle. If still saturated at TSL2591_GAIN_LOW, the scene is
      // simply brighter than the sensor's floor range; calculateLux() below
      // will correctly report that as invalid (-1) and it won't be sent.
      if (tslStepGainDown()) {
        lum = tsl.getFullLuminosity();
        ir = (uint16_t)(lum >> 16);
        full = (uint16_t)(lum & 0xFFFF);
      }
    } else if (full < kTslLowSignalCounts) {
      // Weak signal: raise gain for the *next* cycle rather than delaying
      // this one with another resample; this reading is still valid, just
      // lower-precision.
      tslStepGainUp();
    }

    const float lux = tsl.calculateLux(full, ir);
    if (lux >= 0.0f) {
      sendFloatPacket(PKT_TYPE_SENSOR_EXT_TSL2591_LUX, lux);
    }
  }

  if (hasAs) {
    const float blueRef = (float)as7343.readChannel(AS7343_CHANNEL_F3);   // 475nm
    const float greenRef = (float)as7343.readChannel(AS7343_CHANNEL_F4);  // 515nm
    sendFloatPacket(PKT_TYPE_SENSOR_EXT_AS7343_BLUE, blueRef);
    sendFloatPacket(PKT_TYPE_SENSOR_EXT_AS7343_GREEN, greenRef);
  }
}

static void onPacketReceived(const uint8_t *buffer, size_t size) {
  if (size < 1) {
    return;
  }

  uint32_t value = 0;
  if (size >= 5) {
    memcpy(&value, &buffer[1], sizeof(value));
  }

  switch (buffer[0]) {
    case PKT_TYPE_CMD_BEEP_ON:
      buzzerOn(value > 0 ? (uint16_t)value : kDefaultBeepMs);
      break;
    case PKT_TYPE_CMD_BEEP_OFF:
      buzzerOff();
      break;
    case PKT_TYPE_CMD_EXT_SAMPLE_NOW:
      readAndSendSensors();
      break;
    default:
      break;
  }
}

void setup() {
  Serial.begin(115200);

  // RP2040 side wiring on SenseCAP Indicator.
  Serial1.setRX(17);
  Serial1.setTX(16);
  Serial1.begin(115200);
  packet.setStream(&Serial1);
  packet.setPacketHandler(&onPacketReceived);

  pinMode(kBuzzerPin, OUTPUT);
  buzzerOff();

  Wire.setSDA(20);
  Wire.setSCL(21);
  Wire.begin();

  hasTsl = tsl.begin();
  if (hasTsl) {
    tslGain = TSL2591_GAIN_MED;
    tsl.setGain(tslGain);
    tsl.setTiming(TSL2591_INTEGRATIONTIME_100MS);
  }

  hasAs = as7343.begin();
  if (hasAs) {
    as7343.setATIME(29);
    as7343.setASTEP(599);
    as7343.setGain(AS7343_GAIN_64X);
  }

  nextPeriodicReadMs = millis() + 800;
}

void loop() {
  packet.update();
  if (packet.overflow()) {
    // Ignore occasional overflow; next packets recover.
  }

  const uint32_t now = millis();
  if (buzzerOffAtMs != 0 && now >= buzzerOffAtMs) {
    buzzerOff();
  }

  if (now >= nextPeriodicReadMs) {
    nextPeriodicReadMs = now + 800;
    readAndSendSensors();
  }
}
