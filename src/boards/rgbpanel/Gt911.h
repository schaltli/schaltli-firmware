#pragma once
#include <Arduino.h>
#include <Wire.h>
#include "PanelExpander.h"

// GT911 capacitive touch reader for the Waveshare RGB boards (4.3B, 4").
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
  bool begin(PanelExpander &expander, int intPin, TwoWire &wire = Wire)
  {
    wire_ = &wire;
    intPin_ = intPin;

    // A board without INT on a GPIO (the 4") gets a plain reset; the
    // controller then comes up on whichever address it likes, and the probe
    // below finds it.
    if (intPin_ >= 0)
    {
      pinMode(intPin_, OUTPUT);
      digitalWrite(intPin_, LOW);
    }

    expander.setTouchReset(false);
    delay(10);
    if (intPin_ >= 0) digitalWrite(intPin_, LOW);
    delay(1);
    expander.setTouchReset(true);
    delay(10);

    if (intPin_ >= 0) pinMode(intPin_, INPUT);
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

  // Three answers, not two, and the difference is the whole gesture layer.
  //
  // The controller reports at its own pace. Between reports the data-ready
  // bit is clear, and that says nothing about whether a finger is on the
  // glass - only that there is nothing new to say. Folding that into the
  // same 0 an empty panel gives is fine for a caller polling every 30ms and
  // looking for taps, and wrong for one polling every 5ms to follow a finger.
  //
  // Measured before this existed (2026-09-10): twenty deliberate swipes
  // arrived as 46 contacts, and no missed one lasted more than three samples.
  // Every gap between reports ended a contact, the next report began a fresh
  // one, and travel never accumulated far enough to read as a swipe. The nine
  // of twenty that did work were the ones where a 68ms compose sat between
  // two reads - long enough that a report was always ready.
  enum Status {
    NoNewSample,  // nothing reported since the last read - says nothing more
    Lifted,       // reported, and there is no finger
    Touched,      // reported points, written to the caller's array
  };

  Status poll(Point *points, uint8_t maxPoints, uint8_t &count)
  {
    count = 0;

    uint8_t status = 0;
    if (!readRegister(0x814E, &status, 1)) return NoNewSample;

    // Not cleared here, deliberately: the reference driver clears only after
    // it has read a report, and writing to the status register while the
    // controller is preparing one is not something to do on a hunch.
    if ((status & 0x80) == 0) return NoNewSample;

    const uint8_t reported = status & 0x0F;
    if (reported == 0)
    {
      writeRegister(0x814E, 0);
      return Lifted;
    }
    if (reported > MAX_POINTS)
    {
      // A count this controller cannot produce. Cleared so it keeps
      // reporting, but believed in neither direction.
      writeRegister(0x814E, 0);
      return NoNewSample;
    }

    uint8_t buf[MAX_POINTS * 8] = {0};
    if (!readRegister(0x814F, buf, (uint8_t)(reported * 8)))
    {
      writeRegister(0x814E, 0);
      return NoNewSample;
    }
    writeRegister(0x814E, 0);

    // Offsets are one lower than the reference driver's because it reads
    // into buf[1..] to keep the status byte at buf[0]; this reads the point
    // block on its own.
    const uint8_t n = reported < maxPoints ? reported : maxPoints;
    for (uint8_t i = 0; i < n; i++)
    {
      const uint8_t *p = &buf[i * 8];
      points[i].trackId = p[0];
      points[i].x = (uint16_t)(p[2] << 8) | p[1];
      points[i].y = (uint16_t)(p[4] << 8) | p[3];
      points[i].strength = (uint16_t)(p[6] << 8) | p[5];
    }
    count = n;
    return Touched;
  }

  // Points if a report was waiting, 0 otherwise. Enough for a caller that
  // only wants to know whether the glass is being touched right now, and not
  // enough to follow a gesture: use poll() for that.
  uint8_t read(Point *points, uint8_t maxPoints)
  {
    uint8_t count = 0;
    return poll(points, maxPoints, count) == Touched ? count : 0;
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
