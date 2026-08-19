#include "CanvasDisplayAdapter.h"

// The panel is round: only the r=180 circle inscribed in the 360x360
// framebuffer can show anything, and the usable width shrinks the further a
// line sits from the vertical centre. Both the text size and the per-line
// safe width account for that - a fixed box would either waste most of the
// screen or push the ends of longer lines (an SSID, a URL) into the bezel.
static const int16_t CENTRE = 180;
static const int16_t RADIUS = 168; // 180 minus a margin, so glyphs don't touch the edge

// Adafruit_GFX's built-in font is a fixed 6x8 cell scaled by setTextSize.
static int16_t charWidthFor(uint8_t size) { return (int16_t)(6 * size); }
static int16_t lineHeightFor(uint8_t size) { return (int16_t)(8 * size + 6); }

// Half the chord of the visible circle at a given row - i.e. how much width
// is actually on screen there.
static int16_t halfChordAt(int16_t y) {
  int32_t dy = y - CENTRE;
  int32_t inside = (int32_t)RADIUS * RADIUS - dy * dy;
  if (inside <= 0) return 0;
  return (int16_t)sqrt((double)inside);
}

void CanvasDisplayAdapter::clear() {
  ClippedCanvas16& canvas = presenter_.canvas();
  canvas.fillScreen(0x0000);
  presenter_.present();
}

void CanvasDisplayAdapter::showLines(std::initializer_list<String> lines) {
  ClippedCanvas16& canvas = presenter_.canvas();
  canvas.fillScreen(0x0000);
  canvas.setTextColor(0xFFFF);
  canvas.setTextWrap(false);

  int count = (int)lines.size();
  size_t longest = 0;
  for (const String& line : lines) longest = max(longest, (size_t)line.length());

  // Pick the largest size that still fits both ways, rather than a fixed
  // one: the setup screen is 10 lines and needs the small font, while a
  // two-line status message would be needlessly tiny at that size. Checked
  // against the widest line at the *outermost* row it will occupy, since
  // that is where the circle is narrowest.
  uint8_t size = 3;
  while (size > 1) {
    int16_t blockHeight = (int16_t)(count * lineHeightFor(size));
    int16_t topY = (int16_t)(CENTRE - blockHeight / 2);
    int16_t worstHalfChord = min(halfChordAt(topY), halfChordAt((int16_t)(topY + blockHeight)));
    bool fitsVertically = blockHeight <= 2 * RADIUS;
    bool fitsHorizontally = (int16_t)(longest * charWidthFor(size)) <= 2 * worstHalfChord;
    if (fitsVertically && fitsHorizontally) break;
    size--;
  }
  canvas.setTextSize(size);

  const int16_t lineHeight = lineHeightFor(size);
  const int16_t charWidth = charWidthFor(size);
  // Centred as a block, so a short message does not sit at the top of a
  // round screen with nothing under it.
  int16_t y = (int16_t)(CENTRE - (count * lineHeight) / 2);

  for (const String& line : lines) {
    int16_t textWidth = (int16_t)(line.length() * charWidth);
    int16_t half = halfChordAt((int16_t)(y + lineHeight / 2));
    int16_t safeLeft = (int16_t)(CENTRE - half);
    int16_t x = (int16_t)(CENTRE - textWidth / 2);
    if (x < safeLeft) x = safeLeft;
    // Clip rather than wrap: a line that still does not fit gets cut at the
    // circle instead of spilling into the bezel where nothing is visible.
    canvas.setClip(safeLeft, y, (int16_t)(2 * half), lineHeight);
    canvas.setCursor(x, y);
    canvas.print(line);
    canvas.clearClip();
    y += lineHeight;
  }

  presenter_.present();
  // present() only marks the area dirty - pump LVGL so the message is
  // actually on screen when this returns, since callers show status right
  // before doing something blocking.
  lv_timer_handler();
}
