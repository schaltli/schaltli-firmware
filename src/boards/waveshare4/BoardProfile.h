#pragma once
#include "../rgbpanel/RgbPanel.h"
#include "Ch32v003.h"
#include "St7701.h"

// Everything about the Waveshare ESP32-S3-Touch-LCD-4 (Rev 4.0, SKU 28154)
// that the shared RGB-board firmware (src/boards/rgbpanel/) needs to know.
// See waveshare4v3b/BoardProfile.h for the 4.3B's.
//
// Pins and timings are Waveshare's BSP (component waveshare/esp32_s3_touch_lcd_4
// 3.0.0, include/bsp/esp32_s3_touch_lcd_4.h; Apache-2.0) and espressif's
// ST7701_480_480_PANEL_60HZ_RGB_TIMING from esp_lcd_st7701 2.0.3 - the
// configuration the factory demo on this very board runs with
// (ESP-Brookesia, boot log 2026-10-30). The Arduino examples in the same
// repository use other porches; the BSP's are what was seen working.

// Prefix of this board's own serial lines.
#define BOARD_LOG "[ws4] "

// --- panel geometry -------------------------------------------------------
static const int16_t PANEL_W = 480;
static const int16_t PANEL_H = 480;

// --- RGB timings (ST7701, 480x480) ----------------------------------------
//
// 8 MHz, not the BSP's 16, for the 4.3B's reason (waveshare4v3b/BoardProfile.h)
// and harder: the parallel bus slows this board's own WiFi, and this board's
// WiFi has less to give - its link is weaker than the 4.3B's beside it and
// touchy about how the board lies.
//
// Seen on 2026-10-30, on channel 6 at -60 dBm: at 16 MHz pings took 276-581 ms
// with losses, a firmware upload 42-45 s against the 4.3B's 20, and a refused
// upload ended in a reset connection instead of its answer. 12 MHz then
// looked right (pixel-clock-radio.js 350 against 201 KB/s), until on
// 2026-10-31, at -75 dBm, the loop hung in MQTT for up to 57 s at 12. A sweep
// with ?set=pclk=, two interleaved passes, three snapshots each: 8 MHz 534
// KB/s, 9 496, 11 503 (but 0-75 in its first pass), 12 309, and from 13 on
// nothing got through at all. 10 had been 64 the evening before - not
// monotonic, which harmonics fall inside the channel decides. 8 was the
// steadiest; the picture at 8 is steady and flicker-free (judged on the
// glass, 2026-10-31), where the 4.3B's panel showed only white.
// 8 MHz / (520 x 500) = 31 Hz.
static const int32_t PCLK_HZ = 8 * 1000 * 1000;
static const uint16_t HSYNC_PULSE_WIDTH = 10;
static const uint16_t HSYNC_BACK_PORCH = 10;
static const uint16_t HSYNC_FRONT_PORCH = 20;
static const uint16_t VSYNC_PULSE_WIDTH = 10;
static const uint16_t VSYNC_BACK_PORCH = 10;
static const uint16_t VSYNC_FRONT_PORCH = 10;

static const size_t BOUNCE_BUFFER_PX = PANEL_W * 10;

// The Camera's pictures no closer than this; 0: as fast as they are decoded
// (about 10 a second here, within the spec's loop pause - HIL 2026-11-09).
static const uint32_t CAMERA_MIN_PERIOD_MS = 0;

// --- shared I2C bus -------------------------------------------------------
// Touch (GT911 at 0x5D), the CH32V003 (0x24) and the RTC (PCF85063, 0x51).
static const int I2C_SDA = 15;
static const int I2C_SCL = 7;
// The GT911's INT is not wired to the S3 on this board (BSP: GPIO_NUM_NC).
// It is polled anyway; without INT its address is whatever it powers up
// with, and Gt911::begin probes both.
static const int TP_INT = -1;

// --- pin map --------------------------------------------------------------
// The BSP's DATA0..DATA15, B, G, R as on the 4.3B: DATA0-4 blue, DATA5-10
// green, DATA11-15 red.
static const RgbPanel::Pins PANEL_PINS = {
    /* de */ 40, /* vsync */ 39, /* hsync */ 38, /* pclk */ 41,
    /* r0..r4 */ {46, 3, 8, 18, 17},
    /* g0..g5 */ {14, 13, 12, 11, 10, 9},
    /* b0..b4 */ {5, 45, 48, 47, 21}};

// Idle levels and the clock edge: espressif's timing sets pclk_active_neg
// false and leaves the rest at zero, which differs from the 4.3B on all
// three that are set there.
static const RgbPanel::Timing PANEL_TIMING = {
    PCLK_HZ,
    HSYNC_PULSE_WIDTH, HSYNC_BACK_PORCH, HSYNC_FRONT_PORCH,
    VSYNC_PULSE_WIDTH, VSYNC_BACK_PORCH, VSYNC_FRONT_PORCH,
    /* hsyncIdleLow */ false, /* vsyncIdleLow */ false, /* pclkActiveNeg */ false};

// --- the ST7701's configuration line --------------------------------------
static const int LCD_SPI_CS = 42;
static const int LCD_SPI_SCL = 2;
static const int LCD_SPI_SDA = 1;

using BoardExpander = Ch32v003;

// The panel needs its power and reset from the CH32V003, and its
// configuration over SPI, before the RGB bus means anything to it.
static const bool EXPANDER_BEFORE_PANEL = true;

// After the expander is up, before the RGB panel starts. False stops the
// boot: without the CH32V003 there is no panel power and no backlight, and
// the one board that has none at 0x24 is the V3, which this firmware does
// not support (tasks/waveshare-4-plan.md, decision 3).
static bool boardPanelInit(bool expanderUp)
{
  if (!expanderUp)
  {
    Serial.println(BOARD_LOG "no CH32V003 at 0x24 - unsupported board revision; this firmware is for "
                             "the ESP32-S3-Touch-LCD-4 Rev 4.0 (a V3 has a TCA9554 instead)");
    return false;
  }
  St7701 lcd(LCD_SPI_CS, LCD_SPI_SCL, LCD_SPI_SDA);
  lcd.begin();
  return true;
}
