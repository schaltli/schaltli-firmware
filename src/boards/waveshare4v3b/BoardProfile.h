#pragma once
#include "../rgbpanel/RgbPanel.h"
#include "Ch422g.h"

// Everything about the Waveshare ESP32-S3-Touch-LCD-4.3B that the shared
// RGB-board firmware (src/boards/rgbpanel/) needs to know: geometry, RGB
// timings, pin map, the I2C bus and the IO expander. main.cpp names no pin
// and no expander chip; each board env puts its own directory on the
// include path and this file is found there, the same way DeviceInfo.h and
// ddf_zip.h are.

// Prefix of this board's own serial lines.
#define BOARD_LOG "[4v3b] "


// --- panel geometry -------------------------------------------------------
static const int16_t PANEL_W = 800;
static const int16_t PANEL_H = 480;

// --- RGB timings (ST7262, 800x480) ---------------------------------------
//
// 12 MHz, not the 16 Waveshare's example uses, because at 16 the parallel
// bus slows this board's own WiFi (2026-09-14). Measured against an access
// point on channel 6, switching live with ?set=pclk= and interleaving:
// snapshots ran at 330-480 KB/s at 16 MHz, and in stretches of several
// minutes as low as 20-30 KB/s with ping answers up to a second, while a
// PaperS3 on the same access point at the same signal stayed above 800 KB/s
// throughout. At 12 MHz the 4.3B moved 690-800 KB/s. The CPU was idle and
// memory unchanged in the slow stretches, which is what pointed at the radio
// path and away from the firmware.
//
// Two limits on what that proves. It was one channel: which harmonics of
// the pixel clock land inside the WiFi channel depends on the channel, so a
// board on another access point should be measured again (?set=pclk= is
// kept for exactly that). And 8 MHz showed no picture at all - only white,
// which on a white test screen passed for "looks normal" until a coloured
// one was installed - so the margin below 12 is unknown. 12 was judged by
// eye on that coloured project, swiping included, and looked right.
//
// Refresh is 12 MHz / (820 x 500) = 29 Hz, against 39 at 16; a follow-swipe
// composes at about 14 frames a second on this board either way.
static const int32_t PCLK_HZ = 12 * 1000 * 1000;
static const uint16_t HSYNC_PULSE_WIDTH = 4;
static const uint16_t HSYNC_BACK_PORCH = 8;
static const uint16_t HSYNC_FRONT_PORCH = 8;
static const uint16_t VSYNC_PULSE_WIDTH = 4;
static const uint16_t VSYNC_BACK_PORCH = 8;
static const uint16_t VSYNC_FRONT_PORCH = 8;

// Bounce buffer, in pixels. The RGB peripheral streams the framebuffer out
// of PSRAM continuously; without a bounce buffer through internal SRAM the
// picture drifts sideways under bus contention. Waveshare uses width * 10.
static const size_t BOUNCE_BUFFER_PX = PANEL_W * 10;

// The Camera's pictures no closer than this. Unlimited (7.5 a second) the loop
// paused up to 389 ms against 136 without one, beyond the spec's +150 ms
// (designer docs/2026-11-09-camera.md, criterion 4; HIL 2026-11-09). 170 ms
// then, 290-293 ms of 286 allowed; since a picture waits out a turn that drew
// values, 130 ms gives 6.1-6.3 shown a second and 240-242 ms (2026-11-10).
static const uint32_t CAMERA_MIN_PERIOD_MS = 130;

// --- shared I2C bus -------------------------------------------------------
// Touch, the CH422G expander and the RTC all sit on the same two lines.
static const int I2C_SDA = 8;
static const int I2C_SCL = 9;
static const int TP_INT = 4;

// --- pin map --------------------------------------------------------------
// Waveshare's DATA0..DATA15 is an RGB565 bus in the order B, G, R:
// DATA0-4 = blue, DATA5-10 = green, DATA11-15 = red. Arduino_GFX wants them
// split, hence the regrouping. The wiki names the same lines B3..B7, G2..G7
// and R3..R7 - the panel's own bit numbering, same physical pins.
static const RgbPanel::Pins PANEL_PINS = {
    /* de */ 5, /* vsync */ 3, /* hsync */ 46, /* pclk */ 7,
    /* r0..r4 */ {1, 2, 42, 41, 40},
    /* g0..g5 */ {39, 0, 45, 48, 47, 21},
    /* b0..b4 */ {14, 38, 18, 17, 10}};

static const RgbPanel::Timing PANEL_TIMING = {
    PCLK_HZ,
    HSYNC_PULSE_WIDTH, HSYNC_BACK_PORCH, HSYNC_FRONT_PORCH,
    VSYNC_PULSE_WIDTH, VSYNC_BACK_PORCH, VSYNC_FRONT_PORCH};

// No panel reset line on this board - LCD_RST is on the expander and the
// panel comes up without it, as Waveshare's own example also assumes.

// The CH422G owns the backlight and both reset lines (Ch422g.h).
using BoardExpander = Ch422g;

// The ST7262 runs from the RGB bus alone: the panel starts first, the
// expander after it, as the board has done since its port.
static const bool EXPANDER_BEFORE_PANEL = false;

// Nothing the panel needs from the expander, and a CH422G that does not
// answer costs the backlight and the touch, not the boot.
static bool boardPanelInit(bool /*expanderUp*/) { return true; }
