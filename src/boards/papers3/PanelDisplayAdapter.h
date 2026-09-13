#pragma once
#include <Arduino.h>
#include <U8g2_for_Adafruit_GFX.h>
#include "EpdPanel.h"
#include "../../interfaces/IDisplay.h"
#include "../../ClippedCanvas16.h"

// IDisplay for this board's setup-mode text: AP instructions, connection
// status, the countdown while a hold is being recognised.
//
// Same split as the 4.3B's adapter of the same name, and for the same
// reason: it draws into the canvas the project renderer uses and pushes
// that, so the panel is only ever updated from one place. This is throwaway
// setup UI with no designer-side counterpart, so it deliberately does NOT go
// through ColorScreenRenderer's pixel-exact BDF pipeline, which exists to
// match the designer and has nothing to match here.
//
// Black on white, unlike the 4.3B's white on black. Not a style choice: an
// e-ink panel at rest is white, and a full-bleed black fill is both the
// slowest thing it can draw and the one that leaves the worst trace behind.
class PanelDisplayAdapter : public IDisplay {
public:
  PanelDisplayAdapter(ClippedCanvas16 &canvas, EpdPanel &panel, int16_t w, int16_t h)
      : canvas_(&canvas), panel_(panel), width_(w), height_(h)
  {
    u8g2_.begin(*canvas_);
    u8g2_.setFontMode(1);
  }

  void showLines(std::initializer_list<String> lines) override
  {
    canvas_->fillRect(0, 0, width_, height_, BACKGROUND);
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

  void setCanvas(ClippedCanvas16 &canvas)
  {
    canvas_ = &canvas;
    u8g2_.begin(*canvas_);
  }

  void clear() override
  {
    canvas_->fillRect(0, 0, width_, height_, BACKGROUND);
    present();
  }

private:
  // Setup screens replace everything, so they are worth a clean paint -
  // and they are rare enough that the second it costs does not matter.
  void present()
  {
    panel_.forceFullNext();
    panel_.blit(canvas_->getBuffer());
  }

  static const uint16_t BACKGROUND = 0xFFFF;
  static const uint16_t FOREGROUND = 0x0000;

  ClippedCanvas16 *canvas_;
  EpdPanel &panel_;
  U8G2_FOR_ADAFRUIT_GFX u8g2_;
  int16_t width_;
  int16_t height_;
};
