#pragma once
#include <Arduino.h>
#include <Wire.h>
#include "../rgbpanel/PanelExpander.h"

// The CH32V003 on the Waveshare ESP32-S3-Touch-LCD-4 (Rev 4.0).
//
// Not an IO expander chip but a small RISC-V microcontroller running
// Waveshare's own firmware, which makes it behave like one: a register
// pointer, then a value. It owns the backlight (as PWM, not a line), both
// reset lines and the power rail the panel hangs on:
//
//   EXIO1 TP_RST    touch reset, 1 releases
//   EXIO3 LCD_RST   panel reset, 1 releases
//   EXIO5 SYS_EN    power enable for the panel's rails
//   EXIO6 BEE_EN    buzzer, 1 sounds - kept low
//   EXIO7 RTC_INT   input, the PCF85063's interrupt
//   PWM             backlight, 0 brightest .. 255 off
//
// The PWM is inverted, whatever Waveshare's README says ("0 off, 255
// brightest"): their own BSP turns the backlight on with
// bsp_display_brightness_set(100), which writes 100 - 100 = 0. Seen on the
// glass on 2026-10-30: written as 255 for "on", the QR code of the setup
// screen was faintly there and the backlight dark.
//
// Registers, bits and the power-up order are taken from Waveshare's
// WS_CH32_IO library and BSP (waveshareteam/ESP32-S3-Touch-LCD-4,
// examples/arduino/libraries/WS_CH32_IO, and the component
// waveshare/esp32_s3_touch_lcd_4 3.0.0; both Apache-2.0), not reconstructed.
// The library's own retry on a NACK is kept: the CH32 answers late right
// after power-up.
//
// The V3 board has a TCA9554 at 0x20 instead and nothing at 0x24, so a
// failed begin() is also how a V3 is recognised (BoardProfile.h).
class Ch32v003 : public PanelExpander {
public:
  static const uint8_t ADDR = 0x24;

  static const uint8_t REG_DIRECTION = 0x02;
  static const uint8_t REG_OUTPUT = 0x03;
  static const uint8_t REG_INPUT = 0x04;
  static const uint8_t REG_PWM = 0x05;

  static const uint8_t EXIO_TP_RST = 1 << 1;
  static const uint8_t EXIO_LCD_RST = 1 << 3;
  static const uint8_t EXIO_SYS_EN = 1 << 5;
  static const uint8_t EXIO_BEE_EN = 1 << 6;

  // Waveshare's order: everything low for 200 ms, then power and both
  // resets released, buzzer off, another 200 ms before anyone talks to the
  // panel. Then the backlight full on, as the 4.3B's CH422G comes up: what
  // goes dark later is main.cpp's display-off.
  bool begin(TwoWire &wire) override
  {
    wire_ = &wire;
    wire_->beginTransmission(ADDR);
    if (wire_->endTransmission() != 0) return false;

    if (!writeRegister(REG_DIRECTION, 0xFF)) return false;
    out_ = 0;
    if (!writeRegister(REG_OUTPUT, out_)) return false;
    delay(200);
    out_ = EXIO_SYS_EN | EXIO_LCD_RST | EXIO_TP_RST;
    if (!writeRegister(REG_OUTPUT, out_)) return false;
    delay(200);
    return setBacklight(true);
  }

  bool setBacklight(bool on) override { return writeRegister(REG_PWM, on ? 0 : 255); }

  bool setTouchReset(bool released) override { return setLine(EXIO_TP_RST, released); }

  const char *name() const override { return "CH32V003"; }

private:
  bool setLine(uint8_t mask, bool high)
  {
    if (high) out_ |= mask;
    else out_ &= (uint8_t)~mask;
    return writeRegister(REG_OUTPUT, out_);
  }

  bool writeRegister(uint8_t reg, uint8_t value)
  {
    if (!wire_) return false;
    for (uint8_t attempt = 0; attempt < 5; attempt++)
    {
      wire_->beginTransmission(ADDR);
      wire_->write(reg);
      wire_->write(value);
      if (wire_->endTransmission() == 0) return true;
      delay(10 + attempt * 10);
    }
    return false;
  }

  TwoWire *wire_ = nullptr;
  uint8_t out_ = 0;
};
