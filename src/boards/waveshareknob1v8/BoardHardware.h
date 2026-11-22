#pragma once

// The Waveshare Knob-Touch LCD 1.8's hardware, as the round firmware
// (src/main.cpp) uses it: panel, backlight, touch chip, encoder. Included by
// main.cpp only - it defines objects. Moved out of main.cpp unchanged on
// 2026-11-22 so a second round board could bring its own
// (src/boards/lilygotencoderpro/BoardHardware.h).
//
// Board quirks that still bite, in case they resurface: the ST77916 needs
// the *150* init table (not the library default), a stale
// rtc_gpio_hold_en() from an old deep-sleep experiment can latch LCD/touch
// reset low across reflashes, the CST816 needs no reset pulse but does need
// one write of 0x00 to register 0x00.

#include <Arduino.h>
#include <Wire.h>
#include <Arduino_GFX_Library.h>
#include "driver/rtc_io.h"
#include "BoardProfile.h"
#include "RotaryEncoderReader.h"

// LCD (ST77916, QSPI)
static const int8_t LCD_CS = 14;
static const int8_t LCD_SCK = 13;
static const int8_t LCD_D0 = 15;
static const int8_t LCD_D1 = 16;
static const int8_t LCD_D2 = 17;
static const int8_t LCD_D3 = 18;
static const int8_t LCD_RST = 21;
static const uint8_t LCD_BL = 47;

// Touch (CST816, I2C)
static const int8_t TOUCH_SDA = 11;
static const int8_t TOUCH_SCL = 12;
static const int8_t TOUCH_RST = 10;
static const int8_t TOUCH_INT = 9;
static const uint8_t CST816_ADDR = 0x15;

// The encoder's two pins, for the waveform recorder in main.cpp. The
// decoder (RotaryEncoderReader) has them itself.
#define BOARD_ENC_PIN_A 7
#define BOARD_ENC_PIN_B 8

// No push key on this knob.
#define BOARD_KEY_PIN -1

Arduino_DataBus *bus = new Arduino_ESP32QSPI(LCD_CS, LCD_SCK, LCD_D0, LCD_D1, LCD_D2, LCD_D3);
Arduino_GFX *gfx = new Arduino_ST77916(
    bus, LCD_RST, 0 /* rotation */, true /* IPS */, BOARD_SCREEN_SIZE, BOARD_SCREEN_SIZE,
    0, 0, 0, 0, st77916_150_init_operations, sizeof(st77916_150_init_operations));

// Before anything touches a pin.
static void boardEarlyInit() {
  // A previous deep-sleep experiment latched LCD_RST/TOUCH_RST low via
  // rtc_gpio_hold_en(); that hold survives normal resets and reflashes, so
  // release it before gfx->begin() tries to pulse the display out of reset.
  rtc_gpio_hold_dis(GPIO_NUM_21);
  rtc_gpio_hold_dis(GPIO_NUM_10);
  // GPIO7 too - it was this board's EXT0 wake source in the earlier
  // deep-sleep experiment, and rtc_gpio_deinit() returns it to normal GPIO
  // control rather than merely unlatching it. Kept as insurance even
  // though the "GPIO7 stuck at 1 while GPIO8 toggled" observation that
  // originally motivated this turned out to be the encoder's actual
  // protocol, not a held pad: only the turned direction's channel pulses
  // (see RotaryEncoderReader.h), and that scan was done turning right.
  rtc_gpio_hold_dis(GPIO_NUM_7);
  rtc_gpio_deinit(GPIO_NUM_7);
  gpio_deep_sleep_hold_dis();
  pinMode(LCD_BL, OUTPUT);
  digitalWrite(LCD_BL, HIGH);
}

// 80MHz, up from 40MHz (2026-08-21). The QSPI push is the largest single
// cost of a screen change (22.2ms of 39.9ms) and it is not memory-bound -
// moving the draw buffer into internal RAM barely touched it - so the
// clock is the next thing to rule in or out. A panel that cannot keep up
// shows it immediately and unmistakably as garbled pixels, and the
// measurement in GET /api/debug says whether the rate actually changed at
// all, which a datasheet reading alone would not.
static bool boardPanelBegin() { return gfx->begin(80000000); }

// Lit or dark: the backlight. The panel keeps its frame either way.
static void boardPanelLit(bool lit) { digitalWrite(LCD_BL, lit ? HIGH : LOW); }

// Matches the official Waveshare demo (cst816.cpp/Touch_Init): no reset pin
// is ever driven - just switch the chip to normal mode over I2C. Without
// that one write the chip ACKs and returns an all-zero block forever, which
// looks exactly like a dead sensor.
static void boardTouchBegin() {
  Wire.begin(TOUCH_SDA, TOUCH_SCL);
  Wire.beginTransmission(CST816_ADDR);
  Wire.write(0x00);
  Wire.write((uint8_t)0x00);
  Wire.endTransmission(true);
}

// One reading of the touch chip, as main.cpp's sampleTouch() interprets it:
// fingers > 0 means touched. gesture is for the debug probe only.
struct BoardTouch {
  uint8_t gesture;
  uint8_t fingers;
  int16_t x, y;
};

// CST816 register map: read 7 bytes from 0x00. Bytes 3-4 = X, 5-6 = Y.
// Byte 1 is the gesture id, byte 2 the finger count. Byte 2 ONLY means
// "touched" - the official demo's criterion, confirmed by this unit's own
// probe data (2026-08-20, one recorded swipe at 7ms cadence): b2 holds 1
// through the whole drag with live coordinates, while b1 (gesture id)
// pulses only when the chip's gesture engine fires AND stays latched at 5
// (click) for ~450ms after the finger lifts, with frozen coordinates.
// Keying on b1 is why swipes never worked: mid-drag it is mostly 0 (instant
// phantom release), and after release its latch fabricates a stationary
// phantom touch.
static bool boardTouchRead(BoardTouch &t) {
  uint8_t buf[7];
  Wire.beginTransmission(CST816_ADDR);
  Wire.write(0x00);
  if (Wire.endTransmission(false) != 0) return false;
  if (Wire.requestFrom((int)CST816_ADDR, 7) != 7) return false;
  for (uint8_t i = 0; i < 7; i++) buf[i] = Wire.read();
  t.gesture = buf[1];
  t.fingers = buf[2];
  t.x = ((buf[3] & 0x0F) << 8) | buf[4];
  t.y = ((buf[5] & 0x0F) << 8) | buf[6];
  return true;
}

// The knob's direction-pulse decoder (RotaryEncoderReader.h).
static void boardEncoderBegin() { RotaryEncoderReader::begin(); }
static int32_t boardEncoderDetents() { return RotaryEncoderReader::readDetents(); }
