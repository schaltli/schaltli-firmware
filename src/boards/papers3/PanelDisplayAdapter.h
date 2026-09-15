#pragma once
#include <Arduino.h>
#include <U8g2_for_Adafruit_GFX.h>
#include <lgfx/utility/lgfx_qrcode.h>
#include <vector>
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
  }

  void showLines(std::initializer_list<String> lines) override
  {
    canvas_->fillRect(0, 0, width_, height_, BACKGROUND);
    u8g2_.setFont(u8g2_font_fur35_tf);
    // AFTER setFont(), never before. u8g2_SetFont() silently resets the mode
    // to solid/opaque whenever the font actually changes, so a one-time
    // setFontMode(1) in the constructor holds only until the first
    // setFont() call - which is every call of this method.
    //
    // In solid mode each glyph's whole bounding box is filled with the
    // background colour before the ink is drawn. The 4.3B has carried this
    // same ordering since it was written and nothing showed, because its
    // setup screens are white on black and u8g2's unset background colour is
    // black too. Flip to black on white, as an e-ink panel wants, and every
    // letter gets a black box behind it. Seen on the real PaperS3 on
    // 2026-09-13; the same bug is recorded in the e-paper firmware's
    // ScreenRenderer from 2026-07-20, where it turned a transparent text box
    // into a solid white block.
    u8g2_.setFontMode(1);
    u8g2_.setForegroundColor(FOREGROUND);
    // Belt and braces: if some future font change resets the mode again,
    // filling with the panel's own background is invisible rather than ugly.
    u8g2_.setBackgroundColor(BACKGROUND);

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
    lastWasQr_ = false;
    present(true);
  }

  // The AP setup screen: a WIFI: QR code on the left, what it encodes in
  // readable text on the right.
  //
  // Until 2026-09-14 this board inherited IDisplay's text-only default, which
  // cost it more than the code. WiFiSetupServer redraws this screen once a
  // second for its countdown, the default went through showLines(), and
  // showLines() paints clean - so setup mode flashed the whole panel black
  // and white every second for two minutes. Now only the first paint of a
  // setup screen is clean; a redraw of the same screen, which only ever
  // changes the status line, goes out as a partial update.
  //
  // The generator is the one M5GFX already carries for its own qrcode()
  // (lgfx_qrcode), called directly because this screen draws into the
  // shared canvas rather than onto the panel. No new dependency. (The 4.3B,
  // which has no M5GFX, uses ESP-IDF's esp_qrcode from the Arduino core.)
  void showQrScreen(const String &qrData, const String &caption,
                    std::initializer_list<String> lines) override
  {
    QRCode qr;
    static uint8_t modules[512]; // version 10 needs 407 bytes; the SSID never gets near it
    bool encoded = false;
    for (uint8_t version = 1; version <= 10 && !encoded; version++)
      encoded = lgfx_qrcode_initText(&qr, modules, version, ECC_MEDIUM, qrData.c_str()) == 0;
    if (!encoded)
    {
      showLines(lines);
      return;
    }

    std::vector<String> text;
    text.push_back(caption);
    text.push_back("");
    // Every line keeps its slot even when empty - the status line is blank
    // in some states - so nothing moves between a clean paint and the
    // partial redraws that follow it.
    for (const String &line : lines) text.push_back(line);

    // The same screen only if this adapter painted the last frame, too. The
    // project renderer shares the canvas, and a partial update over its
    // picture would leave the project's ghost under the setup screen.
    const bool sameScreen = lastWasQr_ && panel_.paints() == lastPaint_ && qrData == lastQrData_ &&
                            caption == lastCaption_;

    const int16_t quietZone = 4;
    const int16_t modulesWithQuiet = (int16_t)(qr.size + 2 * quietZone);
    const int16_t scale = (int16_t)max(1, (height_ - 2 * MARGIN) / modulesWithQuiet);
    const int16_t qrPixels = (int16_t)(modulesWithQuiet * scale);
    const int16_t qrX = MARGIN;
    const int16_t qrY = (int16_t)((height_ - qrPixels) / 2);
    const int16_t textX = (int16_t)(qrX + qrPixels + MARGIN);
    const int16_t textWidth = (int16_t)(width_ - textX - MARGIN);

    // The font is chosen once per screen, on its clean paint. Re-fitting on
    // every redraw would let a shorter status line pick a bigger font and
    // shift the whole column under a partial update.
    if (!sameScreen) fontIndex_ = fitFont(text, textWidth);

    canvas_->fillRect(0, 0, width_, height_, BACKGROUND);
    for (int my = 0; my < qr.size; my++)
      for (int mx = 0; mx < qr.size; mx++)
        if (lgfx_qrcode_getModule(&qr, mx, my))
          canvas_->fillRect((int16_t)(qrX + (quietZone + mx) * scale),
                            (int16_t)(qrY + (quietZone + my) * scale), scale, scale, FOREGROUND);

    selectFont(fontIndex_);
    const int16_t lineHeight = currentLineHeight();
    int16_t baseline = (int16_t)((height_ - (int16_t)text.size() * lineHeight) / 2 + u8g2_.getFontAscent());
    for (const String &line : text)
    {
      u8g2_.setCursor(textX, baseline);
      u8g2_.print(line);
      baseline = (int16_t)(baseline + lineHeight);
    }

    lastWasQr_ = true;
    lastQrData_ = qrData;
    lastCaption_ = caption;
    present(!sameScreen);
    lastPaint_ = panel_.paints();
  }

  void setCanvas(ClippedCanvas16 &canvas)
  {
    canvas_ = &canvas;
    u8g2_.begin(*canvas_);
  }

  void clear() override
  {
    canvas_->fillRect(0, 0, width_, height_, BACKGROUND);
    lastWasQr_ = false;
    present(true);
  }

private:
  // A new setup screen replaces everything, so it is worth a clean paint -
  // and new ones are rare enough that the second it costs does not matter.
  // A redraw of the one already showing is not, see showQrScreen().
  void present(bool clean)
  {
    if (clean) panel_.forceFullNext();
    panel_.blit(canvas_->getBuffer());
  }

  // Largest font at which every line fits the column and the block fits the
  // panel. A line silently too wide is the worst failure a setup screen can
  // have: it looks fine and is missing the password.
  int fitFont(const std::vector<String> &text, int16_t columnWidth)
  {
    for (int i = 0; i < FONT_COUNT; i++)
    {
      selectFont(i);
      bool fits = (int16_t)text.size() * currentLineHeight() <= height_ - 2 * MARGIN;
      for (const String &line : text)
        if ((int16_t)u8g2_.getUTF8Width(line.c_str()) > columnWidth) fits = false;
      if (fits) return i;
    }
    return FONT_COUNT - 1;
  }

  void selectFont(int index)
  {
    static const uint8_t *const FONTS[FONT_COUNT] = {u8g2_font_fur42_tf, u8g2_font_fur35_tf,
                                                     u8g2_font_fur30_tf, u8g2_font_fur25_tf,
                                                     u8g2_font_fur20_tf, u8g2_font_fur17_tf};
    u8g2_.setFont(FONTS[index]);
    // After setFont(), for the reason showLines() gives.
    u8g2_.setFontMode(1);
    u8g2_.setForegroundColor(FOREGROUND);
    u8g2_.setBackgroundColor(BACKGROUND);
  }

  int16_t currentLineHeight() { return (int16_t)(u8g2_.getFontAscent() - u8g2_.getFontDescent() + 16); }

  static const int FONT_COUNT = 6;
  static const int16_t MARGIN = 40;
  int fontIndex_ = 0;
  uint32_t lastPaint_ = 0;
  bool lastWasQr_ = false;
  String lastQrData_;
  String lastCaption_;

  static const uint16_t BACKGROUND = 0xFFFF;
  static const uint16_t FOREGROUND = 0x0000;

  ClippedCanvas16 *canvas_;
  EpdPanel &panel_;
  U8G2_FOR_ADAFRUIT_GFX u8g2_;
  int16_t width_;
  int16_t height_;
};
