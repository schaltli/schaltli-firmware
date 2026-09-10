#pragma once
#include <Arduino.h>
#include <U8g2_for_Adafruit_GFX.h>
#include "RgbPanel.h"
#include "../../interfaces/IDisplay.h"
#include "../../ClippedCanvas16.h"

// IDisplay for this board's setup-mode text: AP instructions, connection
// status, the countdown while a hold is being recognised.
//
// Draws into the same canvas the project renderer uses and pushes it to the
// panel, rather than writing to the display directly. Two reasons: the panel
// is only ever updated from one place, so there is no ordering question
// between setup text and a rendered screen; and it is the same split the
// knob makes with CanvasDisplayAdapter, for the same reason recorded there -
// this is throwaway setup UI with no designer-side counterpart, so it
// deliberately does NOT go through ColorScreenRenderer's pixel-exact BDF
// pipeline, which exists to match the designer and has nothing to match here.
//
// It does use the DDF's large font, though, because setup text is read at
// the same distance as everything else and 2.8mm was the problem this
// board's font work set out to fix.
//
// showQrScreen is left to the interface's own default, which falls back to
// text. That is a real gap, not a design choice: a QR carrying the WIFI:
// URI is the difference between joining the AP in one tap and transcribing
// a password. The generator LVGL ships is what the knob uses, and pulling
// LVGL into this build for one file is not worth it before the rest works.
class PanelDisplayAdapter : public IDisplay {
public:
  PanelDisplayAdapter(ClippedCanvas16 &canvas, RgbPanel &panel, int16_t w, int16_t h)
      : canvas_(canvas), panel_(panel), width_(w), height_(h)
  {
    u8g2_.begin(canvas_);
    u8g2_.setFontMode(1);
  }

  void showLines(std::initializer_list<String> lines) override
  {
    canvas_.fillRect(0, 0, width_, height_, BACKGROUND);
    u8g2_.setForegroundColor(FOREGROUND);
    u8g2_.setFont(u8g2_font_fur35_tf);

    // Vertically centred as a block, so one line does not sit at the top of
    // an empty panel and four lines do not run off the bottom.
    const int16_t lineHeight = 52;
    const int16_t total = (int16_t)(lines.size() * lineHeight);
    int16_t y = (int16_t)((height_ - total) / 2 + 37); // +ascent to baseline

    for (const String &line : lines)
    {
      const int16_t textWidth = (int16_t)u8g2_.getUTF8Width(line.c_str());
      u8g2_.setCursor((int16_t)((width_ - textWidth) / 2), y);
      u8g2_.print(line);
      y += lineHeight;
    }
    present();
  }

  void clear() override
  {
    canvas_.fillRect(0, 0, width_, height_, BACKGROUND);
    present();
  }

private:
  void present() { panel_.blit(canvas_.getBuffer()); }

  static const uint16_t BACKGROUND = 0x0000;
  static const uint16_t FOREGROUND = 0xFFFF;

  ClippedCanvas16 &canvas_;
  RgbPanel &panel_;
  U8G2_FOR_ADAFRUIT_GFX u8g2_;
  int16_t width_;
  int16_t height_;
};
