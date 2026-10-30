#pragma once
#include <Arduino.h>

// Brings up the ST7701 on the Waveshare ESP32-S3-Touch-LCD-4.
//
// The ST7701 takes its pixels over the RGB bus like the 4.3B's ST7262, but
// unlike that one it does nothing until it has been configured over a
// separate 3-wire SPI line: 9-bit words, the first bit saying command (0) or
// parameter (1), MSB first, sampled on the rising clock edge. That is three
// GPIOs and a few hundred bytes once at boot, so it is bit-banged here
// rather than pulling in esp_lcd's 3-wire panel IO.
//
// The sequence is Waveshare's own (component waveshare/esp32_s3_touch_lcd_4
// 3.0.0, esp32_s3_touch_lcd_4.c, lcd_init_cmds; Apache-2.0), byte for byte;
// the framing is that of espressif/esp_lcd_st7701 2.0.3
// (ST7701_PANEL_IO_3WIRE_SPI_CONFIG: use_dc_bit, MSB first, mode 0). It runs
// after the CH32V003 has powered the panel and released its reset.
//
// COLMOD 0x66 (18 bit) is theirs too: the bus carries RGB565 on 16 lines,
// and the panel's two low bits per channel are not wired.
class St7701 {
public:
  St7701(int cs, int scl, int sda) : cs_(cs), scl_(scl), sda_(sda) {}

  void begin()
  {
    pinMode(cs_, OUTPUT);
    pinMode(scl_, OUTPUT);
    pinMode(sda_, OUTPUT);
    digitalWrite(cs_, HIGH);
    digitalWrite(scl_, LOW);
    digitalWrite(sda_, LOW);

    for (const Cmd &c : INIT)
    {
      send(c.cmd, c.data, c.len);
      if (c.delayMs) delay(c.delayMs);
    }

    // Free the lines: on this board GPIO1 and GPIO2 are also the SD card's
    // CMD and CLK.
    pinMode(cs_, INPUT);
    pinMode(scl_, INPUT);
    pinMode(sda_, INPUT);
  }

private:
  struct Cmd {
    uint8_t cmd;
    uint8_t data[16];
    uint8_t len;
    uint8_t delayMs;
  };

  void send(uint8_t cmd, const uint8_t *data, uint8_t len)
  {
    digitalWrite(cs_, LOW);
    sendWord(false, cmd);
    for (uint8_t i = 0; i < len; i++) sendWord(true, data[i]);
    digitalWrite(cs_, HIGH);
    delayMicroseconds(2);
  }

  // One 9-bit word. 2 us per half clock is 250 kHz, under the driver's
  // 500 kHz maximum.
  void sendWord(bool isData, uint8_t value)
  {
    sendBit(isData);
    for (int b = 7; b >= 0; b--) sendBit((value >> b) & 1);
  }

  void sendBit(bool high)
  {
    digitalWrite(sda_, high ? HIGH : LOW);
    delayMicroseconds(2);
    digitalWrite(scl_, HIGH);
    delayMicroseconds(2);
    digitalWrite(scl_, LOW);
  }

  int cs_, scl_, sda_;

  static constexpr Cmd INIT[] = {
      {0x11, {}, 0, 120},
      {0xFF, {0x77, 0x01, 0x00, 0x00, 0x10}, 5, 0},
      {0xC0, {0x3B, 0x00}, 2, 0},
      {0xC1, {0x0D, 0x02}, 2, 0},
      {0xC2, {0x21, 0x08}, 2, 0},
      {0xCD, {0x08}, 1, 0},
      {0xB0, {0x00, 0x11, 0x18, 0x0E, 0x11, 0x06, 0x07, 0x08, 0x07, 0x22, 0x04, 0x12, 0x0F, 0xAA, 0x31, 0x18}, 16, 0},
      {0xB1, {0x00, 0x11, 0x19, 0x0E, 0x12, 0x07, 0x08, 0x08, 0x08, 0x22, 0x04, 0x11, 0x11, 0xA9, 0x32, 0x18}, 16, 0},
      {0xFF, {0x77, 0x01, 0x00, 0x00, 0x11}, 5, 0},
      {0xB0, {0x60}, 1, 0},
      {0xB1, {0x30}, 1, 0},
      {0xB2, {0x87}, 1, 0},
      {0xB3, {0x80}, 1, 0},
      {0xB5, {0x49}, 1, 0},
      {0xB7, {0x85}, 1, 0},
      {0xB8, {0x21}, 1, 0},
      {0xC1, {0x78}, 1, 0},
      {0xC2, {0x78}, 1, 20},
      {0xE0, {0x00, 0x1B, 0x02}, 3, 0},
      {0xE1, {0x08, 0xA0, 0x00, 0x00, 0x07, 0xA0, 0x00, 0x00, 0x00, 0x44, 0x44}, 11, 0},
      {0xE2, {0x11, 0x11, 0x44, 0x44, 0xED, 0xA0, 0x00, 0x00, 0xEC, 0xA0, 0x00, 0x00}, 12, 0},
      {0xE3, {0x00, 0x00, 0x11, 0x11}, 4, 0},
      {0xE4, {0x44, 0x44}, 2, 0},
      {0xE5, {0x0A, 0xE9, 0xD8, 0xA0, 0x0C, 0xEB, 0xD8, 0xA0, 0x0E, 0xED, 0xD8, 0xA0, 0x10, 0xEF, 0xD8, 0xA0}, 16, 0},
      {0xE6, {0x00, 0x00, 0x11, 0x11}, 4, 0},
      {0xE7, {0x44, 0x44}, 2, 0},
      {0xE8, {0x09, 0xE8, 0xD8, 0xA0, 0x0B, 0xEA, 0xD8, 0xA0, 0x0D, 0xEC, 0xD8, 0xA0, 0x0F, 0xEE, 0xD8, 0xA0}, 16, 0},
      {0xEB, {0x02, 0x00, 0xE4, 0xE4, 0x88, 0x00, 0x40}, 7, 0},
      {0xEC, {0x3C, 0x00}, 2, 0},
      {0xED, {0xAB, 0x89, 0x76, 0x54, 0x02, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0x20, 0x45, 0x67, 0x98, 0xBA}, 16, 0},
      {0xFF, {0x77, 0x01, 0x00, 0x00, 0x00}, 5, 0},
      {0x36, {0x00}, 1, 0},
      {0x3A, {0x66}, 1, 0},
      {0x21, {}, 0, 120},
      {0x29, {}, 0, 0},
  };
};
