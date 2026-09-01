#pragma once
#include <Arduino.h>
#include <Wire.h>
#include "Ch422g.h"

// GT911 capacitive touch reader for the Waveshare 4.3B.
//
// Hand-rolled rather than pulled in as a library, for the same reason the
// knob port hand-rolls its CST816: the touch path is where board ports go
// wrong quietly, and a driver small enough to read end to end is worth more
// than one that hides its assumptions. (On the knob it was reading the
// touched flag from the wrong byte - data[2], not data[1] - which no
// library would have told us either.)
//
// Register layout and decode taken from espressif/esp-bsp's
// esp_lcd_touch_gt911.c, not from memory:
//   0x814E  status: bit 7 = data ready, bits 0-3 = point count
//   0x814F  point data, 8 bytes per point
//           +1 track id, +2/+3 x low/high, +4/+5 y low/high, +6/+7 strength
//   after reading, write 0 to 0x814E or the controller never reports again
//
// Addresses are 16-bit and big-endian on the wire: high byte first.
class Gt911 {
public:
  struct Point {
    uint16_t x;
    uint16_t y;
    uint16_t strength;
    uint8_t trackId;
  };

  static const uint8_t ADDR_PRIMARY = 0x5D;   // INT low during reset
  static const uint8_t ADDR_ALTERNATE = 0x14; // INT high during reset
  static const uint8_t MAX_POINTS = 5;

  // Resets the controller and finds which of the two addresses it answers
  // on. The address is chosen by the INT level while RST is released, so
  // this drives INT low for the primary address - but it then *probes* both
  // rather than trusting that, because a board that wires INT through
  // anything unexpected would otherwise fail with a silent no-touch.
  bool begin(Ch422g &expander, int intPin, TwoWire &wire = Wire)
  {
    wire_ = &wire;
    intPin_ = intPin;

    pinMode(intPin_, OUTPUT);
    digitalWrite(intPin_, LOW);

    expander.write(Ch422g::EXIO_TP_RST, false);
    delay(10);
    digitalWrite(intPin_, LOW);
    delay(1);
    expander.write(Ch422g::EXIO_TP_RST, true);
    delay(10);

    pinMode(intPin_, INPUT);
    delay(50);

    for (uint8_t candidate : {ADDR_PRIMARY, ADDR_ALTERNATE})
    {
      wire_->beginTransmission(candidate);
      if (wire_->endTransmission() == 0)
      {
        address_ = candidate;
        return true;
      }
    }
    return false;
  }

  uint8_t address() const { return address_; }

  // Reads the product id string ("911" on a genuine GT911). Cheap proof
  // that the bus works and we are talking to what we think we are, rather
  // than to something that merely acknowledges its address.
  bool readProductId(char out[5])
  {
    uint8_t buf[4] = {0};
    if (!readRegister(0x8140, buf, 4)) return false;
    memcpy(out, buf, 4);
    out[4] = '\0';
    return true;
  }

  // Returns the number of points read (0 when nothing is touched).
  uint8_t read(Point *points, uint8_t maxPoints)
  {
    uint8_t status = 0;
    if (!readRegister(0x814E, &status, 1)) return 0;

    if ((status & 0x80) == 0)
    {
      // No data pending. The clear still has to happen: leaving a stale
      // status byte is how the controller ends up never reporting again.
      writeRegister(0x814E, 0);
      return 0;
    }

    uint8_t count = status & 0x0F;
    if (count == 0 || count > MAX_POINTS)
    {
      writeRegister(0x814E, 0);
      return 0;
    }

    uint8_t buf[MAX_POINTS * 8] = {0};
    if (!readRegister(0x814F, buf, (uint8_t)(count * 8)))
    {
      writeRegister(0x814E, 0);
      return 0;
    }
    writeRegister(0x814E, 0);

    // Offsets are one lower than the reference driver's because it reads
    // into buf[1..] to keep the status byte at buf[0]; this reads the point
    // block on its own.
    uint8_t n = count < maxPoints ? count : maxPoints;
    for (uint8_t i = 0; i < n; i++)
    {
      const uint8_t *p = &buf[i * 8];
      points[i].trackId = p[0];
      points[i].x = (uint16_t)(p[2] << 8) | p[1];
      points[i].y = (uint16_t)(p[4] << 8) | p[3];
      points[i].strength = (uint16_t)(p[6] << 8) | p[5];
    }
    return n;
  }

private:
  bool readRegister(uint16_t reg, uint8_t *data, uint8_t len)
  {
    if (!wire_ || !address_) return false;
    wire_->beginTransmission(address_);
    wire_->write((uint8_t)(reg >> 8));
    wire_->write((uint8_t)(reg & 0xFF));
    if (wire_->endTransmission(false) != 0) return false;
    if (wire_->requestFrom((int)address_, (int)len) != len) return false;
    for (uint8_t i = 0; i < len; i++) data[i] = (uint8_t)wire_->read();
    return true;
  }

  bool writeRegister(uint16_t reg, uint8_t value)
  {
    if (!wire_ || !address_) return false;
    wire_->beginTransmission(address_);
    wire_->write((uint8_t)(reg >> 8));
    wire_->write((uint8_t)(reg & 0xFF));
    wire_->write(value);
    return wire_->endTransmission() == 0;
  }

  TwoWire *wire_ = nullptr;
  uint8_t address_ = 0;
  int intPin_ = -1;
};
