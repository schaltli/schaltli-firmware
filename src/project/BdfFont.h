#pragma once
#include <Arduino.h>

#include "../ClippedCanvas16.h"

// Draws text from the .bdf files this device ships inside its own DDF,
// instead of from a compiled-in u8g2 font array picked by name.
//
// Why this exists (2026-08-22, found on hardware): the two were not the
// same font. The DDF's helvR18.bdf carries 754 glyphs - Latin Extended,
// typographic punctuation, currency symbols - and the designer renders
// from exactly those bytes, because it fetches them from this device.
// u8g2's `helvR18_tf` has 191, the Latin-1 set. So a label with a euro
// sign, an em dash or an ellipsis looked correct in the designer and came
// out with holes on the glass, and nothing could detect it: both sides
// believed they were using "helvR18".
//
// The device was already carrying the full fonts - 576KB of BDF inside the
// 74KB DDF_ZIP already in flash, served to the designer on request - and
// then rendering with a smaller second copy. Reading the fonts the device
// actually publishes makes "does the device have this character?" a
// question that cannot be answered two different ways, because there is
// only one answer left.
//
// **Everything is stored in PSRAM, in two flat allocations.** The first
// version used std::map<uint16_t, Glyph> with a std::vector per glyph, and
// those go to the default heap - internal RAM, of which this board has
// about 160KB free. One font's worth of map nodes and vectors is over
// 100KB of it, so the first font fit and the second one (pre-rendering a
// neighbouring screen) did not, and the device boot-looped. Flat arrays in
// PSRAM cost nothing internal and make the failure mode a clean nullptr.
//
// **This deliberately reproduces one off-by-one from the designer's
// lib/bdffont.ts.** Its row loop runs `for (i = bits; i >= 0; i--, x++)`,
// so the most significant bit - the glyph's leftmost pixel - is drawn at
// x+1 rather than x. The firmware's old u8g2 path compensated with
// `textX = obj.x + 1`, and the two errors cancelled: the HIL pixel diff
// has been 0/129600 throughout. Reproducing the shift here, and dropping
// that +1, keeps every existing project pixel-identical. Correcting it
// would move the text in every label of every project by one pixel, which
// is a decision about the product, not a rendering fix - see drawGlyph().
class BdfFont {
public:
  ~BdfFont();

  // Parses BDF source text into PSRAM. The source may be released
  // afterwards. Returns false and leaves the font empty - never
  // half-built - if anything is missing or does not fit.
  bool parse(const char* text, size_t length);

  bool isLoaded() const { return glyphCount_ > 0; }
  size_t glyphCount() const { return glyphCount_; }

  // FONT_ASCENT/FONT_DESCENT from the file's PROPERTIES block. Typographic
  // metrics, not ink extents - see inkAscentOf().
  int16_t fontAscent() const { return fontAscent_; }
  int16_t fontDescent() const { return fontDescent_; }
  // The font's CAP_HEIGHT, or its ascent where the file does not say - the
  // same fallback the designer takes.
  int16_t capHeight() const { return capHeight_ > 0 ? capHeight_ : fontAscent_; }

  // How far above the baseline the glyphs of *this string* actually reach.
  //
  // Per string, not per font, on purpose. The font-wide maximum in
  // helvR18 is 29 against a nominal ascent of 22 - some stacked diacritic
  // in Latin Extended - so clipping to the font-wide figure would grow
  // every label's box by seven pixels, including labels of plain ASCII
  // where nothing overhangs at all. Ä Ö Ü need exactly one.
  int16_t inkAscentOf(const String& utf8) const;
  // How far the last glyph's ink reaches past its advance, in px - a
  // right-aligned text moves left by it (designer lib/bdffont.ts
  // inkRightOverhangOf; luRS10's «%» is 11 px of ink on an advance of 9).
  int16_t inkRightOverhangOf(const String& utf8) const;
  // The highest any glyph of the font reaches above the baseline.
  int16_t maxInkAscent() const;

  // Whether the font has a glyph of its own for `codepoint` - a hidden text
  // field's bullet, which the number fonts lack.
  bool hasGlyph(uint16_t codepoint) const { return find(codepoint) != nullptr; }

  // 1 for every font until 2026-11-01; 4 for a font with BITS_PER_PIXEL 4,
  // whose rows hold one pixel's coverage per hex digit.
  uint8_t bitsPerPixel() const { return bitsPerPixel_; }

  // Cursor advance for one codepoint (BDF DWIDTH), 0 if absent.
  int16_t advance(uint16_t codepoint) const;

  // Total advance for a UTF-8 string - what the designer's measureText()
  // sums, so alignment matches.
  int16_t textWidth(const String& utf8) const;

  // Draws one glyph with its baseline origin at (bx, by), returning the
  // cursor advance. Nothing is drawn for an absent codepoint and the
  // cursor does not move; the designer behaves the same way, so a missing
  // character leaves the same gap on both sides.
  int16_t drawGlyph(ClippedCanvas16& canvas, uint16_t codepoint, int16_t bx, int16_t by, uint16_t color) const;

  // Draws a UTF-8 string, left edge at bx, baseline at by.
  void drawText(ClippedCanvas16& canvas, const String& utf8, int16_t bx, int16_t by, uint16_t color) const;

  // Decodes the next codepoint from a UTF-8 string, advancing `index`.
  // Invalid bytes are returned as themselves, so mis-encoded text degrades
  // to mojibake rather than vanishing.
  static uint16_t nextCodepoint(const String& utf8, size_t& index);

private:
  struct Glyph {
    uint16_t codepoint;
    int16_t dwidth;
    int16_t bbw, bbh, bbox, bboy;
    uint16_t rowCount;
    uint16_t bitsPerRow;
    // A row takes as many 32-bit words as its hex digits need, so a glyph
    // may be wider than 32 px (2026-09-30: FreeUniversal 42, DSEG7). Word 0
    // of a row holds its rightmost 32 bits.
    uint16_t wordsPerRow;
    uint32_t rowOffset;  // into rows_, in words
  };

  const Glyph* find(uint16_t codepoint) const;
  void release();
  void drawCoverageRow(ClippedCanvas16& canvas, const uint32_t* words, uint16_t wordsPerRow, uint16_t digits,
                       int16_t ox, int16_t y, uint16_t color) const;

  Glyph* glyphs_ = nullptr;   // PSRAM, ascending by codepoint when sorted_
  // BDF files list glyphs in ascending ENCODING order and find() binary
  // searches on that. Checked rather than assumed at parse time: a file
  // that broke the assumption would show up as randomly missing
  // characters, which is precisely the class of bug this class exists to
  // end. When it is false, find() scans linearly instead - correct for any
  // order, and no BDF seen so far needs it.
  bool sorted_ = true;
  uint32_t* rows_ = nullptr;  // PSRAM, wordsPerRow words per bitmap row
  uint32_t glyphCount_ = 0;
  uint32_t rowCount_ = 0;
  int16_t fontAscent_ = 0;
  int16_t fontDescent_ = 0;
  int16_t capHeight_ = 0;
  uint8_t bitsPerPixel_ = 1;
};
