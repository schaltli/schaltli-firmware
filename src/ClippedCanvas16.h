#pragma once
#include <Adafruit_GFX.h>

// Ported verbatim from screenbee-m5dial@d73d0ff (tag
// pre-memory-optimizations) on 2026-08-19. Copied as files rather than
// branched: this repo owns the board-specific truth (PSRAM/flash flags,
// lv_conf.h workarounds, CST816 driver, pin map), which is far less
// portable than the project stack. Keep changes here in sync with that
// repo by hand - the two are expected to stay near-identical, since none
// of this class is board-specific.
//
// GFXcanvas16 (RGB565) with an optional clip rectangle - the color
// (24bit/RGB565) counterpart to MqttEPaperDisplay2's ClippedCanvas1
// (1-bit/monochrome). Same reasoning applies: U8g2_for_Adafruit_GFX (and
// every other Adafruit_GFX drawing primitive) ultimately draws through
// drawPixel()/drawFastVLine()/drawFastHLine(), so overriding those here is
// enough to clip glyph rendering to an object's own bounds - Adafruit_GFX/
// U8g2_for_Adafruit_GFX have no built-in clip-window concept otherwise.
//
// setClip()/clearClip() are meant to bracket just the text-drawing calls
// for a single object, not the whole screen render - background fills and
// border strokes are already sized exactly to their object's bounds and
// don't need clipping.
class ClippedCanvas16 : public GFXcanvas16 {
public:
  ClippedCanvas16(int16_t w, int16_t h) : GFXcanvas16(w, h) {}

  // Eine *Region* ist ein aeusserer Clip, der ueber einen ganzen Bildaufbau
  // steht - fuer das Neuzeichnen nur eines Ausschnitts. Die Objekt-Renderer
  // setzen ihren eigenen Clip fuer Text; ohne diese Ebene wuerde ihr
  // clearClip() den aeusseren Ausschnitt aufheben und sie duerften wieder
  // ueber das ganze Bild schreiben.
  void setRegion(int16_t x, int16_t y, int16_t w, int16_t h) {
    regionActive_ = true;
    regionX_ = x;
    regionY_ = y;
    regionW_ = w;
    regionH_ = h;
    clipActive_ = true;
    clipX_ = x;
    clipY_ = y;
    clipW_ = w;
    clipH_ = h;
  }

  void clearRegion() {
    regionActive_ = false;
    clipActive_ = false;
  }

  // Schneidet mit der Region, statt sie zu ersetzen. Ohne gesetzte Region
  // genau das Verhalten von vorher.
  void setClip(int16_t x, int16_t y, int16_t w, int16_t h) {
    if (regionActive_) {
      const int16_t x0 = x > regionX_ ? x : regionX_;
      const int16_t y0 = y > regionY_ ? y : regionY_;
      const int16_t x1 = (int16_t)(x + w) < (int16_t)(regionX_ + regionW_) ? (int16_t)(x + w)
                                                                          : (int16_t)(regionX_ + regionW_);
      const int16_t y1 = (int16_t)(y + h) < (int16_t)(regionY_ + regionH_) ? (int16_t)(y + h)
                                                                          : (int16_t)(regionY_ + regionH_);
      x = x0;
      y = y0;
      w = (int16_t)(x1 > x0 ? x1 - x0 : 0);
      h = (int16_t)(y1 > y0 ? y1 - y0 : 0);
    }
    clipActive_ = true;
    clipX_ = x;
    clipY_ = y;
    clipW_ = w;
    clipH_ = h;
  }

  void clearClip() {
    if (regionActive_) {
      clipX_ = regionX_;
      clipY_ = regionY_;
      clipW_ = regionW_;
      clipH_ = regionH_;
      clipActive_ = true;
      return;
    }
    clipActive_ = false;
  }

  // Damit ein Renderer seine Schleife auf den Ausschnitt begrenzen kann.
  // Nur zu clippen wuerde das Schreiben unterdruecken, nicht das Rechnen -
  // und beim Ring steckt die ganze Zeit im Rechnen (16-fache Abtastung je
  // Pixel), nicht im Schreiben.
  bool clipActive() const { return clipActive_; }
  int16_t clipX() const { return clipX_; }
  int16_t clipY() const { return clipY_; }
  int16_t clipW() const { return clipW_; }
  int16_t clipH() const { return clipH_; }

  void drawPixel(int16_t x, int16_t y, uint16_t color) override {
    if (clipActive_ && (x < clipX_ || x >= clipX_ + clipW_ || y < clipY_ || y >= clipY_ + clipH_)) {
      return;
    }
    GFXcanvas16::drawPixel(x, y, color);
  }

  // GFXcanvas16 provides its own direct-to-buffer overrides of both these
  // (never routed through drawPixel) - see ClippedCanvas1.h's identical
  // comment for why overriding drawPixel alone would silently clip nothing
  // for text.
  void drawFastVLine(int16_t x, int16_t y, int16_t h, uint16_t color) override {
    if (clipActive_) {
      if (h < 0) {
        h = (int16_t)(-h);
        y = (int16_t)(y - (h - 1));
      }
      if (x < clipX_ || x >= clipX_ + clipW_) return;
      int16_t y0 = y < clipY_ ? clipY_ : y;
      int16_t y1 = (int16_t)(y + h) > (int16_t)(clipY_ + clipH_) ? (int16_t)(clipY_ + clipH_) : (int16_t)(y + h);
      if (y1 <= y0) return;
      y = y0;
      h = (int16_t)(y1 - y0);
    }
    GFXcanvas16::drawFastVLine(x, y, h, color);
  }

  void drawFastHLine(int16_t x, int16_t y, int16_t w, uint16_t color) override {
    if (clipActive_) {
      if (w < 0) {
        w = (int16_t)(-w);
        x = (int16_t)(x - (w - 1));
      }
      if (y < clipY_ || y >= clipY_ + clipH_) return;
      int16_t x0 = x < clipX_ ? clipX_ : x;
      int16_t x1 = (int16_t)(x + w) > (int16_t)(clipX_ + clipW_) ? (int16_t)(clipX_ + clipW_) : (int16_t)(x + w);
      if (x1 <= x0) return;
      x = x0;
      w = (int16_t)(x1 - x0);
    }
    GFXcanvas16::drawFastHLine(x, y, w, color);
  }

private:
  bool clipActive_ = false;
  bool regionActive_ = false;
  int16_t regionX_ = 0, regionY_ = 0, regionW_ = 0, regionH_ = 0;
  int16_t clipX_ = 0;
  int16_t clipY_ = 0;
  int16_t clipW_ = 0;
  int16_t clipH_ = 0;
};
