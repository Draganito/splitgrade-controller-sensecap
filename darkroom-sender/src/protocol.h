#pragma once

/**
 * ESP-NOW control protocol (fire-and-forget, no ACK).
 * Shared between darkroom-sender and darkroom-receiver.
 */

#include <Arduino.h>

static constexpr uint8_t kMagic0 = 0xDA;
static constexpr uint8_t kMagic1 = 0x52;
static constexpr uint8_t kProtocolVersion = 1;
static constexpr uint8_t kEspNowChannel = 1;
static constexpr uint16_t kMaxHalfSeconds = 1998;
static constexpr uint32_t kTickMs = 500;

// LED panel configuration (sent from the hidden UI_LEDCFG menu here to
// this repo's paired receiver board -- the XIAO ESP32-S3 receiver in
// darkroom-enlarger-head/). Kept in sync with the receiver's copy so both
// sides always agree on valid ranges.
//
// The data GPIO is a free numeric field (+/- adjustable in UI_LEDCFG), not
// a fixed toggle between two factory pins -- this only ever configures a
// *remote* board's LED output over ESP-NOW, so a bad choice here cannot
// brick this sender's own boot. The receiver still enforces
// [kLedConfigMinPin, kLedConfigMaxPin] as a basic sanity bound (valid
// ESP32(-S3) GPIO numbers); picking a pin that isn't actually broken out
// or wired on the receiver board is the user's responsibility, same as the
// BLE app's config-write path (which has never restricted the pin either).
static constexpr uint16_t kLedConfigMinPixels = 1;
static constexpr uint16_t kLedConfigMaxPixels = 500; // generous headroom over the 169-pixel reference panel
static constexpr uint8_t kLedConfigMinPin = 0;
static constexpr uint8_t kLedConfigMaxPin = 48; // covers the full ESP32-S3 GPIO range

enum Command : uint8_t {
  CMD_PAIR = 0x01,
  CMD_FOCUS_ON = 0x10,
  CMD_METER_BLUE_ON = 0x11,   // steady blue light, no timer -- for shadow-point metering
  CMD_METER_GREEN_ON = 0x12,  // steady green light, no timer -- for highlight-point metering
  CMD_EXPOSE_HARD = 0x20,
  CMD_EXPOSE_SOFT = 0x21,
  CMD_EXPOSE_SPLIT = 0x22,
  CMD_STOP = 0x30,
  CMD_SET_LED_CONFIG = 0x40, // arg0 = pixel count, arg1 = data GPIO (freely chosen)
};

struct __attribute__((packed)) ControlPacket {
  uint8_t magic0;
  uint8_t magic1;
  uint8_t version;
  uint8_t cmd;
  uint16_t arg0;
  uint16_t arg1;
  uint16_t crc16;
};

static inline uint16_t crc16Ccitt(const uint8_t *data, size_t len) {
  uint16_t crc = 0xFFFF;
  for (size_t i = 0; i < len; ++i) {
    crc ^= (uint16_t)data[i] << 8;
    for (uint8_t j = 0; j < 8; ++j) {
      crc = (crc & 0x8000) ? (uint16_t)((crc << 1) ^ 0x1021) : (uint16_t)(crc << 1);
    }
  }
  return crc;
}

static inline void fillPacket(ControlPacket &pkt, Command cmd, uint16_t arg0 = 0, uint16_t arg1 = 0) {
  pkt.magic0 = kMagic0;
  pkt.magic1 = kMagic1;
  pkt.version = kProtocolVersion;
  pkt.cmd = cmd;
  pkt.arg0 = arg0;
  pkt.arg1 = arg1;
  pkt.crc16 = crc16Ccitt((const uint8_t *)&pkt, sizeof(ControlPacket) - sizeof(pkt.crc16));
}
