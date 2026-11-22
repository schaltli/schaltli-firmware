#pragma once

// The LilyGO T-Encoder Pro's hardware, as the round firmware (src/main.cpp)
// uses it (designer tasks/lilygo-t-encoder-pro-plan.md, 2026-11-22). Included
// by main.cpp only - it defines objects. The knob's counterpart is
// src/boards/waveshareknob1v8/BoardHardware.h.
//
// Pins from LilyGO's pin_config.h (T-Encoder-Pro, branch
// arduino-esp32-libs_V3.0.7), checked on the bench unit with a probe
// firmware on 2026-11-22. This is the revision LilyGO calls DXQ120MYB2416A:
// SH8601 panel, CHSC5816 touch. Their newer TFD12MASBCTB4 has a CO5300 and a
// CST816 on the same pins and is not supported here (plan, decision 5).

#include <Arduino.h>
#include <Wire.h>
#include <Arduino_GFX_Library.h>
#include "BoardProfile.h"
#include "QuadratureEncoderReader.h"

// Panel (SH8601 AMOLED, QSPI)
static const int8_t LCD_CS = 10;
static const int8_t LCD_SCK = 12;
static const int8_t LCD_D0 = 11;
static const int8_t LCD_D1 = 13;
static const int8_t LCD_D2 = 7;
static const int8_t LCD_D3 = 14;
static const int8_t LCD_RST = 4;
// The panel's supply (LilyGO: «VCI EN»). There is no backlight on an AMOLED.
static const int8_t LCD_EN = 3;

// Touch (CHSC5816, I2C)
static const int8_t TOUCH_SDA = 5;
static const int8_t TOUCH_SCL = 6;
static const int8_t TOUCH_RST = 8;
static const int8_t TOUCH_INT = 9;
static const uint8_t CHSC5816_ADDR = 0x2E;

// Encoder: quadrature on 1/2, two steps per detent (probe, 2026-11-22).
#define BOARD_ENC_PIN_A 1
#define BOARD_ENC_PIN_B 2
static const uint8_t ENC_STEPS_PER_DETENT = 2;

// The ring is also a push key, low when pressed. GPIO 0 is the boot strap:
// held while powering up, the board starts in download mode, as every ESP32
// does.
#define BOARD_KEY_PIN 0

Arduino_DataBus *bus = new Arduino_ESP32QSPI(LCD_CS, LCD_SCK, LCD_D0, LCD_D1, LCD_D2, LCD_D3);
Arduino_GFX *gfx = new Arduino_SH8601(bus, LCD_RST, 0 /* rotation */, BOARD_SCREEN_SIZE, BOARD_SCREEN_SIZE);

static void boardEarlyInit() {
  pinMode(LCD_EN, OUTPUT);
  digitalWrite(LCD_EN, HIGH);
}

// LilyGO's own examples run the bus at 40 MHz; the probe drew clean at it.
static bool boardPanelBegin() { return gfx->begin(40000000); }

// Lit or dark. An AMOLED has no backlight: dark is display off and sleep in,
// which keeps the frame memory, lit is the reverse. Supply stays on, so the
// panel needs no new init.
static void boardPanelLit(bool lit) {
  if (lit) gfx->displayOn();
  else gfx->displayOff();
}

// A reset pulse, then the chip reports on its own; no mode write is needed
// (the probe read coordinates right after it).
static void boardTouchBegin() {
  pinMode(TOUCH_INT, INPUT_PULLUP);
  pinMode(TOUCH_RST, OUTPUT);
  digitalWrite(TOUCH_RST, LOW);
  delay(20);
  digitalWrite(TOUCH_RST, HIGH);
  delay(100);
  Wire.begin(TOUCH_SDA, TOUCH_SCL, 400000);
}

struct BoardTouch {
  uint8_t gesture;
  uint8_t fingers;
  int16_t x, y;
};

// CHSC5816: a 32-bit register address, 0x2000002C, then 8 bytes - status
// (0xFF when the block is valid), finger count, x low, y low, pressure,
// x high | y high << 4, id | event << 4, reserved. Seen on the probe: 0x80 in
// byte 6 while a finger rests, 0x40 with 0 fingers as it lifts. gesture
// carries byte 6, for the debug probe only.
static bool boardTouchRead(BoardTouch &t) {
  uint8_t b[8];
  Wire.beginTransmission(CHSC5816_ADDR);
  Wire.write(0x20);
  Wire.write(0x00);
  Wire.write(0x00);
  Wire.write(0x2C);
  if (Wire.endTransmission(false) != 0) return false;
  if (Wire.requestFrom((int)CHSC5816_ADDR, 8) != 8) return false;
  for (uint8_t i = 0; i < 8; i++) b[i] = Wire.read();
  t.gesture = b[6];
  t.fingers = b[0] == 0xFF ? b[1] : 0;
  t.x = (int16_t)(((b[5] & 0x0F) << 8) | b[2]);
  t.y = (int16_t)(((b[5] & 0xF0) << 4) | b[3]);
  return true;
}

static void boardEncoderBegin() {
  QuadratureEncoderReader::begin(BOARD_ENC_PIN_A, BOARD_ENC_PIN_B, ENC_STEPS_PER_DETENT);
}
static int32_t boardEncoderDetents() { return QuadratureEncoderReader::readDetents(); }
