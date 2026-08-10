#pragma once

/**
 * Indicator_SWSPI - expander-aware 9-bit software SPI bus for the
 * SenseCAP Indicator D1 (Seeed) ST7701S panel.
 */

#include <Arduino.h>
#include <Wire.h>
#include <Arduino_GFX_Library.h>

#define PCA9535_INPUT_PORT_0 0x00
#define PCA9535_OUTPUT_PORT_0 0x02
#define PCA9535_CONFIG_PORT_0 0x06

class Indicator_SWSPI : public Arduino_DataBus
{
public:
  Indicator_SWSPI(int8_t rst, int8_t cs, int8_t sck, int8_t mosi,
                  TwoWire *wire = &Wire, uint8_t addr = 0x20,
                  int8_t sda = 39, int8_t scl = 40)
      : _rst(rst), _cs(cs), _sck(sck), _mosi(mosi),
        _wire(wire), _addr(addr), _sda(sda), _scl(scl) {}

  bool isFound() const { return _found; }
  uint8_t detectedAddress() const { return _activeAddr; }

  bool begin(int32_t = GFX_NOT_DEFINED, int8_t = GFX_NOT_DEFINED) override
  {
    ::pinMode(_sck, OUTPUT);
    ::digitalWrite(_sck, HIGH);
    ::pinMode(_mosi, OUTPUT);
    ::digitalWrite(_mosi, HIGH);
    _wire->begin(_sda, _scl);

    _activeAddr = _addr;
    _found = probe(_activeAddr);
    if (!_found)
    {
      const uint8_t fallbackAddrs[] = {0x20, 0x21, 0x38};
      for (uint8_t a : fallbackAddrs)
      {
        if (a == _addr) continue;
        if (probe(a)) { _activeAddr = a; _found = true; break; }
      }
    }
    if (!_found) return false;

    _out0 = 0xFF;
    writeReg(PCA9535_OUTPUT_PORT_0, _out0);
    uint8_t cfg0 = readReg(PCA9535_CONFIG_PORT_0);
    cfg0 &= ~((1 << _cs) | (1 << _rst));
    writeReg(PCA9535_CONFIG_PORT_0, cfg0);
    expWrite(_cs, HIGH);
    expWrite(_rst, HIGH);
    delay(20);
    expWrite(_rst, LOW);
    delay(20);
    expWrite(_rst, HIGH);
    delay(120);
    return true;
  }

  void beginWrite() override { expWrite(_cs, LOW); }
  void endWrite() override { expWrite(_cs, HIGH); }
  void writeCommand(uint8_t c) override { transfer9(c, false); }
  void write(uint8_t d) override { transfer9(d, true); }
  void writeCommand16(uint16_t) override {}
  void writeCommandBytes(uint8_t *, uint32_t) override {}
  void write16(uint16_t) override {}
  void writeRepeat(uint16_t, uint32_t) override {}
  void writeBytes(uint8_t *, uint32_t) override {}
  void writePixels(uint16_t *, uint32_t) override {}

private:
  inline void transfer9(uint8_t v, bool dc)
  {
    ::digitalWrite(_mosi, dc ? HIGH : LOW);
    ::digitalWrite(_sck, LOW);
    ::digitalWrite(_sck, HIGH);
    for (uint8_t bit = 0x80; bit; bit >>= 1)
    {
      ::digitalWrite(_mosi, (v & bit) ? HIGH : LOW);
      ::digitalWrite(_sck, LOW);
      ::digitalWrite(_sck, HIGH);
    }
  }

  inline void expWrite(uint8_t pin, uint8_t val)
  {
    if (val) _out0 |= (1 << pin);
    else _out0 &= ~(1 << pin);
    writeReg(PCA9535_OUTPUT_PORT_0, _out0);
  }

  void writeReg(uint8_t reg, uint8_t val)
  {
    _wire->beginTransmission(_activeAddr);
    _wire->write(reg);
    _wire->write(val);
    _wire->endTransmission();
  }

  uint8_t readReg(uint8_t reg)
  {
    _wire->beginTransmission(_activeAddr);
    _wire->write(reg);
    _wire->endTransmission();
    _wire->requestFrom((int)_activeAddr, 1);
    return _wire->available() ? _wire->read() : 0xFF;
  }

  bool probe(uint8_t addr)
  {
    _wire->beginTransmission(addr);
    return (_wire->endTransmission() == 0);
  }

  int8_t _rst, _cs, _sck, _mosi;
  TwoWire *_wire;
  uint8_t _addr;
  uint8_t _activeAddr = 0;
  int8_t _sda, _scl;
  uint8_t _out0 = 0xFF;
  bool _found = false;
};
