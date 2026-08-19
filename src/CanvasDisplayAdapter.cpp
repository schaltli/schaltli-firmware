#include "CanvasDisplayAdapter.h"

// The panel is round: only the r=180 circle inscribed in the 360x360
// framebuffer can show anything. Text is laid out inside a safe box rather
// than across the full width, or the ends of longer lines (an SSID, a URL)
// fall off the visible area entirely.
static const int16_t SAFE_LEFT = 46;
static const int16_t SAFE_WIDTH = 268;
static const int16_t LINE_HEIGHT = 24;

void CanvasDisplayAdapter::clear() {
  ClippedCanvas16& canvas = presenter_.canvas();
  canvas.fillScreen(0x0000);
  presenter_.present();
}

void CanvasDisplayAdapter::showLines(std::initializer_list<String> lines) {
  ClippedCanvas16& canvas = presenter_.canvas();
  canvas.fillScreen(0x0000);
  canvas.setTextColor(0xFFFF);
  canvas.setTextSize(2);
  canvas.setTextWrap(false);

  // Vertically centred as a block, so a two-line message does not sit at the
  // top of a round screen with nothing under it.
  int count = (int)lines.size();
  int16_t y = 180 - (count * LINE_HEIGHT) / 2;

  for (const String& line : lines) {
    // Adafruit_GFX's built-in font is a fixed 6x8 cell, scaled by
    // setTextSize - so 12px per character here. Centre on that, and clip to
    // the safe box so an over-long line is cut rather than wrapping into the
    // bezel.
    int16_t textWidth = (int16_t)(line.length() * 12);
    int16_t x = textWidth < SAFE_WIDTH ? (int16_t)(180 - textWidth / 2) : SAFE_LEFT;
    canvas.setClip(SAFE_LEFT, y, SAFE_WIDTH, LINE_HEIGHT);
    canvas.setCursor(x, y);
    canvas.print(line);
    canvas.clearClip();
    y += LINE_HEIGHT;
  }

  presenter_.present();
  // present() only marks the area dirty - pump LVGL so the message is
  // actually on screen when this returns, since callers show status right
  // before doing something blocking.
  lv_timer_handler();
}
