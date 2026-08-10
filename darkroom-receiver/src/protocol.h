#pragma once

/**
 * ESP-NOW control protocol (fire-and-forget, no ACK).
 * Shared between darkroom-sender and darkroom-receiver.
 *
 * Packet layout (10 bytes, fixed size):
 *   [magic0][magic1][version][cmd][arg0:2][arg1:2][crc16:2]
 *
 * Time values are in 0.5 s units: 4.0 s = 8, 12.0 s = 24, max 999.0 s = 1998.
 */

#include <Arduino.h>

static constexpr uint8_t kMagic0 = 0xDA;
static constexpr uint8_t kMagic1 = 0x52; // "RK" — darkRoom
static constexpr uint8_t kProtocolVersion = 1;
static constexpr uint8_t kEspNowChannel = 1;
static constexpr uint16_t kMaxHalfSeconds = 1998; // 999.0 s
static constexpr uint32_t kTickMs = 500;

// LED panel configuration (darkroom-gledopto only; sent from darkroom-sender's
// hidden UI_LEDCFG menu). Kept here too, not just in darkroom-gledopto, so
// all sides always agree on valid ranges even though darkroom-receiver
// itself ignores this command (it has no LED panel to configure).
static constexpr uint16_t kLedConfigMinPixels = 1;
static constexpr uint16_t kLedConfigMaxPixels = 500; // generous headroom over the 169-pixel reference panel
static constexpr uint8_t kLedConfigPinA = 16;  // factory-default "D" terminal
static constexpr uint8_t kLedConfigPinB = 2;   // secondary output

enum Command : uint8_t {
  CMD_PAIR = 0x01,
  CMD_FOCUS_ON = 0x10,
  CMD_METER_BLUE_ON = 0x11,   // steady blue light, no timer -- for shadow-point metering
  CMD_METER_GREEN_ON = 0x12,  // steady green light, no timer -- for highlight-point metering
  CMD_EXPOSE_HARD = 0x20,
  CMD_EXPOSE_SOFT = 0x21,
  CMD_EXPOSE_SPLIT = 0x22,
  CMD_STOP = 0x30,
  CMD_SET_LED_CONFIG = 0x40, // arg0 = pixel count, arg1 = data GPIO (16 or 2)
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

static inline bool validatePacket(const uint8_t *data, int len, ControlPacket &out) {
  if (len != (int)sizeof(ControlPacket)) {
    return false;
  }
  memcpy(&out, data, sizeof(ControlPacket));
  if (out.magic0 != kMagic0 || out.magic1 != kMagic1) {
    return false;
  }
  if (out.version != kProtocolVersion) {
    return false;
  }
  const uint16_t expected = crc16Ccitt(data, sizeof(ControlPacket) - sizeof(out.crc16));
  return expected == out.crc16;
}

static inline float halfSecondsToSeconds(uint16_t halfSeconds) {
  return halfSeconds / 2.0f;
}
