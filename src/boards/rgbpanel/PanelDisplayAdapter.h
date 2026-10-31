#pragma once
#include <Arduino.h>
#include <U8g2_for_Adafruit_GFX.h>
#include <qrcode.h>
#include <vector>
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
// Until 2026-09-15 showQrScreen was left to the interface's text-only
// default, because the only generator anyone had looked for was LVGL's, and
// this build has no LVGL. The Arduino core had one all along: ESP-IDF's
// qrcode component (esp_qrcode_generate), already on the include path and
// linked into every S3 build. Same Nayuki encoder the knob gets through
// LVGL, no new dependency.
class PanelDisplayAdapter : public IDisplay {
public:
  PanelDisplayAdapter(ClippedCanvas16 &canvas, RgbPanel &panel, int16_t w, int16_t h)
      : canvas_(&canvas), panel_(panel), width_(w), height_(h)
  {
    u8g2_.begin(*canvas_);
  }

  void showLines(std::initializer_list<String> lines) override
  {
    canvas_->fillRect(0, 0, width_, height_, BACKGROUND);
    u8g2_.setFont(u8g2_font_fur35_tf);
    // AFTER setFont(), never before - u8g2_SetFont() silently resets the
    // mode to solid/opaque on a font change, so the constructor's one-time
    // setFontMode(1) never survived to here.
    //
    // Nothing ever showed on this board: in solid mode each glyph's box is
    // filled with u8g2's own background colour, which is black because
    // nobody set it, and these screens are white on black. The PaperS3
    // inherited this file, flipped to black on white as an e-ink panel
    // wants, and every letter came out in a black box (2026-09-13). Fixed
    // here too rather than only there, because the next board to copy this
    // adapter should not inherit a trap that happens to be invisible.
    u8g2_.setFontMode(1);
    u8g2_.setForegroundColor(FOREGROUND);
    u8g2_.setBackgroundColor(BACKGROUND);

    // Vertically centred as a block, so one line does not sit at the top of
    // an empty panel and four lines do not run off the bottom.
    int16_t lineHeight = 52;
    int16_t ascent = 37;
    // A line wider than the panel takes the next smaller font for all of
    // them: on the 4"'s 480 «loslassen zum Abbrechen» under the setup hold's
    // countdown lost both ends at fur35 (2026-10-31). Where fur35 fits - the
    // 4.3B, every line it shows - nothing here changes.
    int16_t widest = 0;
    for (const String &line : lines) widest = (int16_t)max((int)widest, (int)u8g2_.getUTF8Width(line.c_str()));
    if (widest > width_ - 2 * MARGIN)
    {
      for (int i = 1; i < FONT_COUNT_STACKED; i++)
      {
        selectFont(i);
        widest = 0;
        for (const String &line : lines) widest = (int16_t)max((int)widest, (int)u8g2_.getUTF8Width(line.c_str()));
        if (widest <= width_ - 2 * MARGIN) break;
      }
      lineHeight = currentLineHeight();
      ascent = (int16_t)u8g2_.getFontAscent();
    }
    const int16_t total = (int16_t)(lines.size() * lineHeight);
    int16_t y = (int16_t)((height_ - total) / 2 + ascent); // +ascent to baseline

    for (const String &line : lines)
    {
      const int16_t textWidth = (int16_t)u8g2_.getUTF8Width(line.c_str());
      u8g2_.setCursor((int16_t)((width_ - textWidth) / 2), y);
      u8g2_.print(line);
      y += lineHeight;
    }
    present();
  }

  // The AP setup screen: a WIFI: QR code on the left, what it encodes in
  // readable text on the right - the PaperS3's layout, in this panel's
  // white on black. The code itself sits on a white square with its quiet
  // zone, because a phone reads dark modules on light and not the reverse.
  //
  // WiFiSetupServer calls this once a second for its countdown. The QR code
  // and the layout are worked out on the first call for a given code and
  // caption and then reused, so only the status line changes from one
  // second to the next and nothing else on the screen moves.
  void showQrScreen(const String &qrData, const String &caption,
                    std::initializer_list<String> lines) override
  {
    std::vector<String> text;
    text.push_back(caption);
    text.push_back("");
    // Every line keeps its slot even when empty - the status line is blank
    // in some states - so the block does not jump when it fills in.
    for (const String &line : lines) text.push_back(line);

    if (qrData != qrData_ || caption != caption_)
    {
      qrData_ = qrData;
      caption_ = caption;
      encodeQr(qrData);
      fitLayout(text);
    }
    if (qrSize_ == 0)
    {
      showLines(lines);
      return;
    }

    canvas_->fillRect(0, 0, width_, height_, BACKGROUND);

    const int16_t modulesWithQuiet = (int16_t)(qrSize_ + 2 * QUIET_ZONE);
    const int16_t qrPixels = (int16_t)(modulesWithQuiet * scale_);
    selectFont(fontIndex_);
    const int16_t lineHeight = currentLineHeight();
    const int16_t textHeight = (int16_t)((int16_t)text.size() * lineHeight);
    // Beside the code, or under it when the panel is too narrow for that
    // (fitLayout); the block centred either way.
    const int16_t qrX = stacked_ ? (int16_t)((width_ - qrPixels) / 2) : MARGIN;
    const int16_t qrY = stacked_ ? (int16_t)((height_ - qrPixels - MARGIN - textHeight) / 2)
                                 : (int16_t)((height_ - qrPixels) / 2);
    canvas_->fillRect(qrX, qrY, qrPixels, qrPixels, FOREGROUND);
    for (int my = 0; my < qrSize_; my++)
      for (int mx = 0; mx < qrSize_; mx++)
        if (qrModules_[my * qrSize_ + mx])
          canvas_->fillRect((int16_t)(qrX + (QUIET_ZONE + mx) * scale_),
                            (int16_t)(qrY + (QUIET_ZONE + my) * scale_), scale_, scale_, BACKGROUND);

    int16_t textX = (int16_t)(qrX + qrPixels + MARGIN);
    int16_t baseline = (int16_t)((height_ - textHeight) / 2 + u8g2_.getFontAscent());
    if (stacked_)
    {
      // One left edge for the whole block, so the lines read as a list.
      int16_t widest = 0;
      for (const String &line : text) widest = (int16_t)max((int)widest, (int)u8g2_.getUTF8Width(line.c_str()));
      textX = (int16_t)((width_ - widest) / 2);
      baseline = (int16_t)(qrY + qrPixels + MARGIN + u8g2_.getFontAscent());
    }
    for (const String &line : text)
    {
      u8g2_.setCursor(textX, baseline);
      u8g2_.print(line);
      baseline = (int16_t)(baseline + lineHeight);
    }
    present();
  }

  // Follows the board when it swaps canvases.
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
  void present() { panel_.blit(canvas_->getBuffer()); }

  // esp_qrcode_generate hands the finished code to a callback and frees it
  // when that returns, with no context pointer on the way - hence the static
  // target, which is only ever set for the length of one call.
  void encodeQr(const String &qrData)
  {
    qrSize_ = 0;
    qrModules_.clear();
    encoding_ = this;
    esp_qrcode_config_t cfg = ESP_QRCODE_CONFIG_DEFAULT();
    cfg.display_func = &PanelDisplayAdapter::copyModules;
    cfg.max_qrcode_version = 10;
    cfg.qrcode_ecc_level = ESP_QRCODE_ECC_MED; // what the knob and the PaperS3 encode at
    if (esp_qrcode_generate(&cfg, qrData.c_str()) != ESP_OK) qrSize_ = 0;
    encoding_ = nullptr;
  }

  static void copyModules(esp_qrcode_handle_t qr)
  {
    if (!encoding_) return;
    const int size = esp_qrcode_get_size(qr);
    encoding_->qrModules_.assign((size_t)(size * size), 0);
    for (int y = 0; y < size; y++)
      for (int x = 0; x < size; x++)
        encoding_->qrModules_[(size_t)(y * size + x)] = esp_qrcode_get_module(qr, x, y) ? 1 : 0;
    encoding_->qrSize_ = size;
  }

  // The largest font at which every line fits beside the code, and then the
  // largest code that still leaves the column that wide.
  //
  // Text before code, unlike the PaperS3, which simply takes the full height
  // for its code: that panel is 960 wide, this one 800, and at full height
  // the column beside it is too narrow for this board's SSID at any size
  // anyone can read. A line silently too wide is the worst failure a setup
  // screen can have - it looks fine and is missing the password. The code
  // never goes below MIN_MODULE_PX, about 25mm across on this panel, which
  // is still far larger than the knob's, and that one scans.
  void fitLayout(const std::vector<String> &text)
  {
    stacked_ = false;
    if (qrSize_ == 0) return;
    const int16_t modulesWithQuiet = (int16_t)(qrSize_ + 2 * QUIET_ZONE);
    const int16_t tallest = (int16_t)((height_ - 2 * MARGIN) / modulesWithQuiet);
    for (int i = 0; i < FONT_COUNT; i++)
    {
      selectFont(i);
      if ((int16_t)text.size() * currentLineHeight() > height_ - 2 * MARGIN) continue;
      int16_t widest = 0;
      for (const String &line : text)
        widest = (int16_t)max((int)widest, (int)u8g2_.getUTF8Width(line.c_str()));
      const int16_t byWidth = (int16_t)((width_ - 3 * MARGIN - widest) / modulesWithQuiet);
      const int16_t scale = (int16_t)min(tallest, byWidth);
      if (scale >= MIN_MODULE_PX)
      {
        fontIndex_ = i;
        scale_ = scale;
        return;
      }
    }
    // Nothing fits beside it: the panel is too narrow, as the 4"'s square
    // 480 is (2026-10-30 - the SSID ran off the right edge). The code goes
    // above the text instead, from the largest font down, and may come down
    // to MIN_STACKED_MODULE_PX: 4px is 0.6mm a module there, 25mm for the
    // whole code, which a phone reads. Only reached when the side-by-side
    // search above found nothing, so a board that fit before draws as before.
    stacked_ = true;
    for (int i = 0; i < FONT_COUNT_STACKED; i++)
    {
      selectFont(i);
      int16_t widest = 0;
      for (const String &line : text)
        widest = (int16_t)max((int)widest, (int)u8g2_.getUTF8Width(line.c_str()));
      if (widest > width_ - 2 * MARGIN) continue;
      const int16_t textHeight = (int16_t)((int16_t)text.size() * currentLineHeight());
      const int16_t byHeight = (int16_t)((height_ - 3 * MARGIN - textHeight) / modulesWithQuiet);
      const int16_t byWidth = (int16_t)((width_ - 2 * MARGIN) / modulesWithQuiet);
      const int16_t scale = (int16_t)min(byHeight, byWidth);
      if (scale >= MIN_STACKED_MODULE_PX)
      {
        fontIndex_ = i;
        scale_ = scale;
        return;
      }
    }
    stacked_ = false;
    fontIndex_ = FONT_COUNT - 1;
    scale_ = (int16_t)max(1, min((int)tallest, (int)MIN_MODULE_PX));
  }

  void selectFont(int index)
  {
    // The last two only for the stacked layout (FONT_COUNT_STACKED).
    static const uint8_t *const FONTS[FONT_COUNT_STACKED] = {u8g2_font_fur35_tf, u8g2_font_fur30_tf,
                                                             u8g2_font_fur25_tf, u8g2_font_fur20_tf,
                                                             u8g2_font_fur17_tf, u8g2_font_fur14_tf,
                                                             u8g2_font_fur11_tf};
    u8g2_.setFont(FONTS[index]);
    // After setFont(), for the reason showLines() gives.
    u8g2_.setFontMode(1);
    u8g2_.setForegroundColor(FOREGROUND);
    u8g2_.setBackgroundColor(BACKGROUND);
  }

  int16_t currentLineHeight() { return (int16_t)(u8g2_.getFontAscent() - u8g2_.getFontDescent() + 14); }

  static inline PanelDisplayAdapter *encoding_ = nullptr;
  static const int FONT_COUNT = 5;
  static const int FONT_COUNT_STACKED = 7;
  static const int16_t MARGIN = 32;
  static const int16_t QUIET_ZONE = 4;
  static const int16_t MIN_MODULE_PX = 5;
  static const int16_t MIN_STACKED_MODULE_PX = 4;
  String qrData_;
  String caption_;
  std::vector<uint8_t> qrModules_;
  int qrSize_ = 0;
  int16_t scale_ = 1;
  int fontIndex_ = 0;
  bool stacked_ = false;

  static const uint16_t BACKGROUND = 0x0000;
  static const uint16_t FOREGROUND = 0xFFFF;

  // A pointer rather than a reference, because this board swaps which canvas
  // is current when a screen transition commits. Holding the one handed over
  // at construction meant the setup countdown would be drawn into the canvas
  // that is no longer on the panel, after any swipe.
  ClippedCanvas16 *canvas_;
  RgbPanel &panel_;
  U8G2_FOR_ADAFRUIT_GFX u8g2_;
  int16_t width_;
  int16_t height_;
};
