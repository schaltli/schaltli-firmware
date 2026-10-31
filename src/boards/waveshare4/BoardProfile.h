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
// 12 MHz, not the BSP's 16, for the 4.3B's reason (waveshare4v3b/BoardProfile.h):
// at 16 the parallel bus slows this board's own WiFi. Seen on 2026-10-30, on
// channel 6 at -60 dBm: at 16 MHz pings took 276-581 ms with losses, a
// firmware upload 42-45 s against the 4.3B's 20, and a refused upload ended
// in a reset connection instead of its answer; switched live to 12 with
// ?set=pclk=, pings fell to 2-8 ms. The picture at 12 is steady, colours and
// edges right (judged on the glass). 12 MHz / (520 x 500) = 46 Hz.
//
// hil/waveshare4v3b/pixel-clock-radio.js on this board: 12 MHz 350 KB/s,
// 16 MHz 201. Snapshots by hand, same evening: 10 MHz 64 KB/s, 11 389,
// 12 399-407, 14 442 - not monotonic, so lower is not safer; which harmonics
// fall inside the channel decides, as on the 4.3B. 12 stays, with margin to
// both of the bad ones.
static const int32_t PCLK_HZ = 12 * 1000 * 1000;
static const uint16_t HSYNC_PULSE_WIDTH = 10;
static const uint16_t HSYNC_BACK_PORCH = 10;
static const uint16_t HSYNC_FRONT_PORCH = 20;
static const uint16_t VSYNC_PULSE_WIDTH = 10;
static const uint16_t VSYNC_BACK_PORCH = 10;
static const uint16_t VSYNC_FRONT_PORCH = 10;

static const size_t BOUNCE_BUFFER_PX = PANEL_W * 10;

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
