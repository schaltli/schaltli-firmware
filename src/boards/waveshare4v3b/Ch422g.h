#pragma once
#include <Arduino.h>
#include <Wire.h>
#include "../rgbpanel/PanelExpander.h"

// Minimal driver for the CH422G IO expander on the Waveshare 4.3B.
//
// On this board the expander is not a convenience - it owns the backlight,
// both reset lines and the SD card select, so nothing on the panel or the
// touch controller can be brought up without it:
//
//   EXIO1 TP_RST    touch reset
//   EXIO2 LCD_BL    backlight enable
//   EXIO3 LCD_RST   panel reset
//   EXIO4 SD_CS     TF card select, active low
//   EXIO5 USB_SEL   USB routing
//
// Every line starts high (begin()) unless the board says otherwise with
// CH422G_IO_AT_START. The 7" does: there USB_SEL high routes GPIO19/20 to
// the CAN transceiver and the USB-C socket marked USB goes dead
// (waveshare7/BoardProfile.h).
//
// The chip is unusual: it has no register pointer. Each "register" is its
// own I2C address that you write a single byte to. Getting that wrong is
// how you end up with a dark panel and no idea whether the fault is the
// expander, the backlight or the RGB timings - which is why milestone 1
// deliberately avoided touching it at all.
//
// Addresses and bit meanings are taken from esp-arduino-libs/ESP32_IO_Expander
// (Apache-2.0), src/port/esp_io_expander_ch422g.c, rather than reconstructed:
//   WR_SET = 0x48 >> 1, WR_OC = 0x46 >> 1, WR_IO = 0x70 >> 1, RD_IO = 0x4D >> 1
#ifndef CH422G_IO_AT_START
#define CH422G_IO_AT_START 0xFF
#endif

class Ch422g : public PanelExpander {
public:
  static const uint8_t ADDR_WR_SET = 0x24; // system settings
  static const uint8_t ADDR_WR_OC = 0x23;  // OC0-3 output
  static const uint8_t ADDR_WR_IO = 0x38;  // IO0-7 output
  static const uint8_t ADDR_RD_IO = 0x26;  // IO0-7 input

  // WR_SET bits. Bit 0 is the one that matters here: without it IO0-7 stay
  // inputs and every write to WR_IO is silently ignored.
  static const uint8_t SET_IO_OUTPUT_ENABLE = 1 << 0;
  static const uint8_t SET_OPEN_DRAIN = 1 << 2;
  static const uint8_t SET_SLEEP = 1 << 3;

  // Named lines, so no call site repeats a bit number.
  static const uint8_t EXIO_TP_RST = 1;
  static const uint8_t EXIO_LCD_BL = 2;
  static const uint8_t EXIO_LCD_RST = 3;
  static const uint8_t EXIO_SD_CS = 4;
  static const uint8_t EXIO_USB_SEL = 5;

  // Power-on state of IO0-7 per the reference driver: all high. Starting
  // from that rather than from zero matters - a zero would pull TP_RST and
  // LCD_RST low and hold both devices in reset.
  bool begin(TwoWire &wire = Wire) override
  {
    wire_ = &wire;
    ioState_ = CH422G_IO_AT_START;
    if (!writeByte(ADDR_WR_SET, SET_IO_OUTPUT_ENABLE)) return false;
    return writeByte(ADDR_WR_IO, ioState_);
  }

  bool write(uint8_t exio, bool high)
  {
    if (high) ioState_ |= (uint8_t)(1u << exio);
    else ioState_ &= (uint8_t)~(1u << exio);
    return writeByte(ADDR_WR_IO, ioState_);
  }

  bool setBacklight(bool on) override { return write(EXIO_LCD_BL, on); }
  bool setTouchReset(bool released) override { return write(EXIO_TP_RST, released); }
  const char *name() const override { return "CH422G"; }

  uint8_t state() const { return ioState_; }

  // Reads IO0-7. Only meaningful for lines left as inputs; with
  // SET_IO_OUTPUT_ENABLE set this reports what was driven.
  bool read(uint8_t &out)
  {
    if (!wire_) return false;
    if (wire_->requestFrom((int)ADDR_RD_IO, 1) != 1) return false;
    out = (uint8_t)wire_->read();
    return true;
  }

private:
  bool writeByte(uint8_t addr, uint8_t value)
  {
    if (!wire_) return false;
    wire_->beginTransmission(addr);
    wire_->write(value);
    return wire_->endTransmission() == 0;
  }

  TwoWire *wire_ = nullptr;
  uint8_t ioState_ = 0xFF;
};
