#pragma once

// SenseCAP Indicator D1 — direct I2C touch reader (no TouchLib dependency).
//
// Known-good findings from bring-up/debug:
// - On this hardware the touchscreen is FT5x06 at I2C 0x48.
// - FT5x06 reports pixel-like coordinates for the 480x480 panel, so avoid
//   synthetic linear calibration maps (those caused half-screen errors).
// - Reliability comes from debounced sampling + orientation transform only.

#define TOUCH_SDA 39
#define TOUCH_SCL 40

int16_t touch_max_x = 479;
int16_t touch_max_y = 479;
int16_t touch_raw_x = 0;
int16_t touch_raw_y = 0;
int16_t touch_last_x = 0;
int16_t touch_last_y = 0;
static constexpr uint8_t kDisplayRotation = 2; // must match main.cpp display rotation

// FT5x06 on the SenseCAP Indicator already reports coordinates in display
// pixels (0..479), so no scaling calibration is needed — only orientation.
// These flags express the orientation transform and are persisted.
bool touch_swap_xy = false;
bool touch_flip_x = false;
bool touch_flip_y = false;

bool touch_down = false;
uint8_t touch_down_stable = 0;
static constexpr uint8_t kTouchReadSamples = 4;
static constexpr int16_t kTouchJitterMax = 28;

bool touch_init_ok = false;
uint8_t touch_addr = 0;
enum TouchReadMode : uint8_t {
  TOUCH_READ_CST = 0,
  TOUCH_READ_FT,
  TOUCH_READ_AUTO
};
TouchReadMode touch_read_mode = TOUCH_READ_AUTO;

static inline void expanderWriteReg(uint8_t reg, uint8_t val) {
  Wire.beginTransmission(0x20);
  Wire.write(reg);
  Wire.write(val);
  Wire.endTransmission();
}

static inline void touchResetViaExpander() {
  Wire.begin(TOUCH_SDA, TOUCH_SCL);
  uint8_t cfg = 0xFF;
  Wire.beginTransmission(0x20);
  Wire.write(0x06);
  Wire.endTransmission(false);
  Wire.requestFrom(0x20, 1);
  if (Wire.available()) {
    cfg = Wire.read();
  }
  cfg &= ~(1 << 7);
  expanderWriteReg(0x06, cfg);
  expanderWriteReg(0x02, 0xFF);
  delay(10);
  expanderWriteReg(0x02, 0x7F);
  delay(20);
  expanderWriteReg(0x02, 0xFF);
  delay(50);
}

static bool i2cExists(uint8_t addr) {
  Wire.beginTransmission(addr);
  return Wire.endTransmission() == 0;
}

static bool cst820ReadPoint(int16_t &x, int16_t &y) {
  uint8_t buf[7] = {0};
  Wire.beginTransmission(touch_addr);
  Wire.write(0x01);
  if (Wire.endTransmission(false) != 0) {
    return false;
  }
  if (Wire.requestFrom((int)touch_addr, 7) != 7) {
    return false;
  }
  for (int i = 0; i < 7; ++i) {
    buf[i] = Wire.read();
  }
  const uint8_t points = buf[2] & 0x0F;
  if (points < 1 || points > 5) {
    return false;
  }
  x = (int16_t)(((buf[3] & 0x0F) << 8) | buf[4]);
  y = (int16_t)(((buf[5] & 0x0F) << 8) | buf[6]);
  return true;
}

static bool ft5x06ReadPoint(int16_t &x, int16_t &y) {
  uint8_t points = 0;
  Wire.beginTransmission(touch_addr);
  Wire.write(0x02);
  if (Wire.endTransmission(false) != 0) {
    return false;
  }
  if (Wire.requestFrom((int)touch_addr, 1) != 1) {
    return false;
  }
  points = Wire.read() & 0x0F;
  if (points < 1 || points > 5) {
    return false;
  }

  uint8_t p[4] = {0};
  Wire.beginTransmission(touch_addr);
  Wire.write(0x03);
  if (Wire.endTransmission(false) != 0) {
    return false;
  }
  if (Wire.requestFrom((int)touch_addr, 4) != 4) {
    return false;
  }
  for (int i = 0; i < 4; ++i) {
    p[i] = Wire.read();
  }
  x = (int16_t)(((p[0] & 0x0F) << 8) | p[1]);
  y = (int16_t)(((p[2] & 0x0F) << 8) | p[3]);
  return true;
}

static bool readTouchPointRaw(int16_t &x, int16_t &y) {
  if (touch_read_mode == TOUCH_READ_CST) {
    return cst820ReadPoint(x, y);
  }
  if (touch_read_mode == TOUCH_READ_FT) {
    return ft5x06ReadPoint(x, y);
  }
  if (cst820ReadPoint(x, y)) {
    return true;
  }
  return ft5x06ReadPoint(x, y);
}

// Orientation defaults derived from the official Seeed sample: for the
// rotation-2 panel the FT5x06 pixel coordinates map 1:1 to the screen.
static inline void touch_set_orientation_defaults(uint8_t rotation) {
  switch (rotation) {
    case 1:
      touch_swap_xy = true;
      touch_flip_x = false;
      touch_flip_y = true;
      break;
    case 2:
      touch_swap_xy = false;
      touch_flip_x = true;
      touch_flip_y = true;
      break;
    case 3:
      touch_swap_xy = true;
      touch_flip_x = true;
      touch_flip_y = false;
      break;
    default: // 0
      touch_swap_xy = false;
      touch_flip_x = true;
      touch_flip_y = true;
      break;
  }
}

void touch_set_orientation(bool swap_xy, bool flip_x, bool flip_y) {
  touch_swap_xy = swap_xy;
  touch_flip_x = flip_x;
  touch_flip_y = flip_y;
}

void touch_init(int16_t w, int16_t h, uint8_t) {
  touch_max_x = w - 1;
  touch_max_y = h - 1;
  touchResetViaExpander();
  Wire.begin(TOUCH_SDA, TOUCH_SCL);
  delay(10);

  if (i2cExists(0x15)) {
    touch_addr = 0x15;
    touch_read_mode = TOUCH_READ_CST;
    touch_init_ok = true;
  } else if (i2cExists(0x38)) {
    touch_addr = 0x38;
    touch_read_mode = TOUCH_READ_FT;
    touch_init_ok = true;
  } else if (i2cExists(0x48)) {
    touch_addr = 0x48;
    // SenseCAP Indicator uses FT5x06 at 0x48.
    touch_read_mode = TOUCH_READ_FT;
    touch_init_ok = true;
  } else {
    touch_addr = 0;
    touch_read_mode = TOUCH_READ_AUTO;
    touch_init_ok = false;
  }
}

bool touch_is_ready() { return touch_init_ok; }

bool touch_has_signal() { return true; }

bool touch_touched() {
  if (!touch_init_ok) {
    return false;
  }

  int16_t sx[kTouchReadSamples];
  int16_t sy[kTouchReadSamples];
  uint8_t n = 0;
  for (uint8_t i = 0; i < kTouchReadSamples; ++i) {
    int16_t rx = 0;
    int16_t ry = 0;
    if (readTouchPointRaw(rx, ry)) {
      sx[n] = rx;
      sy[n] = ry;
      n++;
    }
    delayMicroseconds(1200);
  }

  if (n < 2) {
    touch_down_stable = 0;
    touch_down = false;
    return false;
  }

  int16_t minX = sx[0], maxX = sx[0];
  int16_t minY = sy[0], maxY = sy[0];
  int32_t sumX = 0;
  int32_t sumY = 0;
  for (uint8_t i = 0; i < n; ++i) {
    if (sx[i] < minX) minX = sx[i];
    if (sx[i] > maxX) maxX = sx[i];
    if (sy[i] < minY) minY = sy[i];
    if (sy[i] > maxY) maxY = sy[i];
    sumX += sx[i];
    sumY += sy[i];
  }

  if ((maxX - minX) > kTouchJitterMax || (maxY - minY) > kTouchJitterMax) {
    touch_down_stable = 0;
    touch_down = false;
    return false;
  }

  const int16_t x = (int16_t)(sumX / n);
  const int16_t y = (int16_t)(sumY / n);

  if (touch_down_stable < 3) {
    touch_down_stable++;
  }
  touch_down = true;
  if (touch_down_stable < 2) {
    return false;
  }

  touch_raw_x = x;
  touch_raw_y = y;

  int16_t mx = touch_raw_x;
  int16_t my = touch_raw_y;
  if (touch_swap_xy) {
    const int16_t t = mx;
    mx = my;
    my = t;
  }
  if (touch_flip_x) {
    mx = touch_max_x - mx;
  }
  if (touch_flip_y) {
    my = touch_max_y - my;
  }
  touch_last_x = (int16_t)constrain((long)mx, 0L, (long)touch_max_x);
  touch_last_y = (int16_t)constrain((long)my, 0L, (long)touch_max_y);
  return true;
}

bool touch_released() { return false; }
