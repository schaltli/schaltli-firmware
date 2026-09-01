#pragma once
#include <Arduino.h>
#include <U8g2_for_Adafruit_GFX.h>
#include "../../ClippedCanvas16.h"

// A font sample sheet drawn on the panel itself, at real size.
//
// The point is the medium, not the picture. Whether 4.1mm of cap height is
// readable from where a panel hangs in a vehicle cannot be judged from a PNG
// on a monitor - the scale is wrong and the viewing distance is wrong. It can
// only be judged by standing where the reader will stand and looking at this
// glass.
//
// No DDF is needed for this: every candidate is already compiled into
// U8g2_for_Adafruit_GFX, which is the same library the real rendering path
// uses. So this shows the actual glyphs the firmware would draw, not a
// preview of them.
//
// Millimetre figures are the font's own ascent times this panel's 0.117mm
// pixel pitch (800x480 across 93.6mm), i.e. cap height - the dimension that
// decides legibility, not the line height, which is always larger and
// flatters the numbers.
struct FontSampleEntry {
  const uint8_t *font;
  const char *name;
  const char *mm;
};

inline void drawFontSample(ClippedCanvas16 &canvas, U8G2_FOR_ADAFRUIT_GFX &u8g2,
                           int16_t panelW, int16_t panelH)
{
  canvas.fillRect(0, 0, panelW, panelH, 0x0000);

  u8g2.begin(canvas);
  u8g2.setFontMode(1);
  u8g2.setForegroundColor(0xFFFF);

  // Ordered small to large, so the eye can find the first line it can still
  // read comfortably from where it is standing - that is the answer.
  static const FontSampleEntry entries[] = {
      {u8g2_font_helvR24_tf, "helvR24 / fur25", "2.9 mm"},
      {u8g2_font_fur35_tf, "fur35", "4.1 mm"},
      {u8g2_font_fub35_tf, "fub35 (bold)", "4.1 mm"},
      {u8g2_font_fur42_tf, "fur42", "4.9 mm"},
      {u8g2_font_logisoso50_tf, "logisoso50", "5.9 mm"},
  };

  int16_t y = 44;
  for (const auto &e : entries)
  {
    u8g2.setFont(e.font);
    u8g2.setCursor(16, y);
    // A real label, not "The quick brown fox": the judgement being made is
    // about this panel's actual job.
    u8g2.print("Frischwasser 78%");

    u8g2.setFont(u8g2_font_helvR12_tf);
    u8g2.setForegroundColor(0x8410);
    u8g2.setCursor(560, y);
    u8g2.print(e.name);
    u8g2.setCursor(720, y);
    u8g2.print(e.mm);
    u8g2.setForegroundColor(0xFFFF);

    y += 62;
  }

  // Digits only above this size - upstream ships no full glyph set that
  // large, which happens to match what a panel needs: words stay small,
  // measured values get the room.
  u8g2.setFont(u8g2_font_logisoso62_tn);
  u8g2.setCursor(16, panelH - 24);
  u8g2.print("13.4 78 21");

  u8g2.setFont(u8g2_font_helvR12_tf);
  u8g2.setForegroundColor(0x8410);
  u8g2.setCursor(560, panelH - 24);
  u8g2.print("logisoso62 (digits)");
  u8g2.setCursor(720, panelH - 24);
  u8g2.print("7.3 mm");
}
