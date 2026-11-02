#include "BdfFont.h"

#include <esp_heap_caps.h>
#include <string.h>

#include "GlyphPixels.h"

using glyphpixels::parseHexRow;
using glyphpixels::rowBit;
using glyphpixels::rowCoverage;
using glyphpixels::wordsFor;

namespace {

// Walks the source line by line without copying it - these files are
// 100-200KB and there are four of them.
struct LineReader : BdfLineSource {
  const char* begin;
  const char* p;
  const char* end;
  LineReader(const char* text, const char* stop) : begin(text), p(text), end(stop) {}

  bool rewind() override {
    p = begin;
    return true;
  }

  bool next(const char*& lineStart, size_t& lineLen) override {
    if (p >= end) return false;
    lineStart = p;
    const char* nl = p;
    while (nl < end && *nl != '\n' && *nl != '\r') nl++;
    lineLen = (size_t)(nl - lineStart);
    p = nl;
    while (p < end && (*p == '\n' || *p == '\r')) p++;
    while (lineLen > 0 && (lineStart[lineLen - 1] == ' ' || lineStart[lineLen - 1] == '\t')) lineLen--;
    return true;
  }
};

bool startsWith(const char* s, size_t len, const char* prefix) {
  const size_t n = strlen(prefix);
  return len >= n && memcmp(s, prefix, n) == 0;
}

// Reads up to `count` whitespace-separated integers from a line.
int readInts(const char* s, size_t len, long* out, int count) {
  int found = 0;
  size_t i = 0;
  while (i < len && found < count) {
    while (i < len && (s[i] == ' ' || s[i] == '\t')) i++;
    if (i >= len) break;
    bool neg = false;
    if (s[i] == '-') {
      neg = true;
      i++;
    }
    if (i >= len || s[i] < '0' || s[i] > '9') {
      while (i < len && s[i] != ' ' && s[i] != '\t') i++;
      continue;
    }
    long v = 0;
    while (i < len && s[i] >= '0' && s[i] <= '9') {
      v = v * 10 + (s[i] - '0');
      i++;
    }
    out[found++] = neg ? -v : v;
  }
  return found;
}

bool isHexLine(const char* s, size_t len) {
  if (len == 0) return false;
  for (size_t i = 0; i < len; i++) {
    const char c = s[i];
    const bool hex = (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F');
    if (!hex) return false;
  }
  return true;
}

}  // namespace

BdfFont::~BdfFont() { release(); }

void BdfFont::release() {
  if (glyphs_ != nullptr) free(glyphs_);
  if (rows_ != nullptr) free(rows_);
  glyphs_ = nullptr;
  rows_ = nullptr;
  glyphCount_ = 0;
  rowCount_ = 0;
}

bool BdfFont::parse(const char* text, size_t length) {
  LineReader reader(text, text + length);
  return parse(reader);
}

bool BdfFont::parse(BdfLineSource& source) {
  release();
  fontAscent_ = 0;
  fontDescent_ = 0;
  capHeight_ = 0;
  bitsPerPixel_ = 1;

  // Pass 1: how many glyphs, how many bitmap rows. Counting first means one
  // allocation of the right size for each, instead of growth that would
  // fragment PSRAM for everything after it.
  uint32_t glyphs = 0;
  uint32_t rows = 0;
  {
    const char* line;
    size_t len;
    bool inChar = false;
    bool inBitmap = false;
    while (source.next(line, len)) {
      if (len == 0) continue;
      if (!inChar) {
        if (startsWith(line, len, "STARTCHAR")) {
          inChar = true;
          inBitmap = false;
        }
        continue;
      }
      if (startsWith(line, len, "ENDCHAR")) {
        glyphs++;
        inChar = false;
        inBitmap = false;
      } else if (inBitmap) {
        if (isHexLine(line, len)) rows += wordsFor(len);
      } else if (startsWith(line, len, "BITMAP")) {
        inBitmap = true;
      }
    }
  }
  if (glyphs == 0) return false;

  glyphs_ = (Glyph*)heap_caps_malloc((size_t)glyphs * sizeof(Glyph), MALLOC_CAP_SPIRAM);
  rows_ = (uint32_t*)heap_caps_malloc((size_t)(rows == 0 ? 1 : rows) * sizeof(uint32_t), MALLOC_CAP_SPIRAM);
  if (glyphs_ == nullptr || rows_ == nullptr) {
    Serial.printf("[bdf] no PSRAM for %u glyphs / %u row words\n", (unsigned)glyphs, (unsigned)rows);
    release();
    return false;
  }

  // Pass 2: fill.
  if (!source.rewind()) {
    release();
    return false;
  }
  const char* line;
  size_t len;
  bool inChar = false;
  bool inBitmap = false;
  Glyph current;
  long encoding = -1;
  uint32_t glyphIndex = 0;
  uint32_t rowIndex = 0;

  while (source.next(line, len)) {
    if (len == 0) continue;

    if (!inChar) {
      if (startsWith(line, len, "STARTCHAR")) {
        inChar = true;
        inBitmap = false;
        memset(&current, 0, sizeof(current));
        current.rowOffset = rowIndex;
        encoding = -1;
      } else if (startsWith(line, len, "FONT_ASCENT")) {
        long v[1];
        if (readInts(line + 11, len - 11, v, 1) == 1) fontAscent_ = (int16_t)v[0];
      } else if (startsWith(line, len, "FONT_DESCENT")) {
        long v[1];
        if (readInts(line + 12, len - 12, v, 1) == 1) fontDescent_ = (int16_t)v[0];
      } else if (startsWith(line, len, "CAP_HEIGHT")) {
        // How tall a capital stands on the baseline, which is what every
        // control here draws an icon at (designer lib/level-shape.ts,
        // fontMetricsOf). In every font shipped it is exactly the H's own
        // bitmap height, so a reader of glyph boxes gets the same number.
        long v[1];
        if (readInts(line + 10, len - 10, v, 1) == 1) capHeight_ = (int16_t)v[0];
      } else if (startsWith(line, len, "BITS_PER_PIXEL")) {
        // A 4-bit font: each hex digit of a row is one pixel's coverage
        // (designer docs/device-contract.md, «Font depth»). Anything but 4
        // reads as 1 bit, as every font before it did.
        long v[1];
        if (readInts(line + 14, len - 14, v, 1) == 1 && v[0] == 4) bitsPerPixel_ = 4;
      }
      continue;
    }

    if (startsWith(line, len, "ENDCHAR")) {
      if (encoding >= 0 && encoding <= 0xFFFF && glyphIndex < glyphs) {
        current.codepoint = (uint16_t)encoding;
        glyphs_[glyphIndex++] = current;
      } else {
        // Unusable or unencoded glyph: give its rows back so the next one
        // starts where this began, rather than leaving a hole.
        rowIndex = current.rowOffset;
      }
      inChar = false;
      inBitmap = false;
      continue;
    }

    if (inBitmap) {
      if (isHexLine(line, len) && rowIndex + wordsFor(len) <= rows) {
        // BDF pads each row to whole hex digits, and the designer derives
        // its shift width from that padded length rather than from BBw -
        // drawGlyph depends on matching it exactly.
        current.bitsPerRow = (uint16_t)(len * 4);
        current.wordsPerRow = wordsFor(len);
        parseHexRow(line, len, &rows_[rowIndex]);
        rowIndex += current.wordsPerRow;
        current.rowCount++;
      }
      continue;
    }

    if (startsWith(line, len, "ENCODING")) {
      long v[1];
      if (readInts(line + 8, len - 8, v, 1) == 1) encoding = v[0];
    } else if (startsWith(line, len, "DWIDTH")) {
      long v[2];
      if (readInts(line + 6, len - 6, v, 2) >= 1) current.dwidth = (int16_t)v[0];
    } else if (startsWith(line, len, "BBX")) {
      long v[4];
      if (readInts(line + 3, len - 3, v, 4) == 4) {
        current.bbw = (int16_t)v[0];
        current.bbh = (int16_t)v[1];
        current.bbox = (int16_t)v[2];
        current.bboy = (int16_t)v[3];
      }
    } else if (startsWith(line, len, "BITMAP")) {
      inBitmap = true;
    }
  }

  glyphCount_ = glyphIndex;
  rowCount_ = rowIndex;
  if (glyphCount_ == 0) {
    release();
    return false;
  }

  sorted_ = true;
  for (uint32_t i = 1; i < glyphCount_; i++) {
    if (glyphs_[i - 1].codepoint > glyphs_[i].codepoint) {
      sorted_ = false;
      Serial.println("[bdf] glyphs are not in ascending order - using linear lookup");
      break;
    }
  }
  return true;
}

const BdfFont::Glyph* BdfFont::find(uint16_t codepoint) const {
  if (glyphCount_ == 0) return nullptr;
  if (!sorted_) {
    for (uint32_t i = 0; i < glyphCount_; i++) {
      if (glyphs_[i].codepoint == codepoint) return &glyphs_[i];
    }
    return nullptr;
  }
  uint32_t lo = 0, hi = glyphCount_;
  while (lo < hi) {
    const uint32_t mid = lo + (hi - lo) / 2;
    const uint16_t c = glyphs_[mid].codepoint;
    if (c == codepoint) return &glyphs_[mid];
    if (c < codepoint) lo = mid + 1;
    else hi = mid;
  }
  return nullptr;
}

int16_t BdfFont::advance(uint16_t codepoint) const {
  const Glyph* g = find(codepoint);
  return g == nullptr ? 0 : g->dwidth;
}

uint16_t BdfFont::nextCodepoint(const String& utf8, size_t& index) {
  const size_t n = utf8.length();
  if (index >= n) return 0;
  const uint8_t b0 = (uint8_t)utf8[index];
  if (b0 < 0x80) {
    index++;
    return b0;
  }
  if ((b0 & 0xE0) == 0xC0 && index + 1 < n) {
    const uint8_t b1 = (uint8_t)utf8[index + 1];
    if ((b1 & 0xC0) == 0x80) {
      index += 2;
      return (uint16_t)(((b0 & 0x1F) << 6) | (b1 & 0x3F));
    }
  }
  if ((b0 & 0xF0) == 0xE0 && index + 2 < n) {
    const uint8_t b1 = (uint8_t)utf8[index + 1];
    const uint8_t b2 = (uint8_t)utf8[index + 2];
    if ((b1 & 0xC0) == 0x80 && (b2 & 0xC0) == 0x80) {
      index += 3;
      return (uint16_t)(((b0 & 0x0F) << 12) | ((b1 & 0x3F) << 6) | (b2 & 0x3F));
    }
  }
  index++;
  return b0;
}

int16_t BdfFont::textWidth(const String& utf8) const {
  int16_t width = 0;
  size_t i = 0;
  while (i < utf8.length()) width += advance(nextCodepoint(utf8, i));
  return width;
}

int16_t BdfFont::inkAscentOf(const String& utf8) const {
  int16_t top = 0;
  size_t i = 0;
  while (i < utf8.length()) {
    const Glyph* g = find(nextCodepoint(utf8, i));
    if (g == nullptr) continue;
    const int16_t glyphTop = (int16_t)(g->bbh + g->bboy);
    if (glyphTop > top) top = glyphTop;
  }
  return top;
}

int16_t BdfFont::inkRightOverhangOf(const String& utf8) const {
  const Glyph* last = nullptr;
  size_t i = 0;
  while (i < utf8.length()) last = find(nextCodepoint(utf8, i));
  if (last == nullptr) return 0;
  const int16_t over = (int16_t)(last->bbox + last->bbw + 1 - last->dwidth);
  return over > 0 ? over : 0;
}

int16_t BdfFont::maxInkAscent() const {
  int16_t top = fontAscent_;
  for (uint32_t i = 0; i < glyphCount_; i++) {
    const int16_t glyphTop = (int16_t)(glyphs_[i].bbh + glyphs_[i].bboy);
    if (glyphTop > top) top = glyphTop;
  }
  return top;
}

int16_t BdfFont::drawGlyph(ClippedCanvas16& canvas, uint16_t codepoint, int16_t bx, int16_t by,
                           uint16_t color) const {
  const Glyph* g = find(codepoint);
  if (g == nullptr) return 0;

  const int16_t ox = (int16_t)(bx + g->bbox);
  const int16_t oy = (int16_t)(by - g->bboy - g->bbh);

  for (uint16_t row = 0; row < g->rowCount; row++) {
    const uint32_t* words = &rows_[g->rowOffset + (uint32_t)row * g->wordsPerRow];
    // Blank rows are most of a glyph's box - the space above an 'a', below
    // everything without a descender - and skipping them costs a test per
    // word.
    bool blank = true;
    for (uint16_t w = 0; w < g->wordsPerRow && blank; w++) blank = words[w] == 0;
    if (blank) continue;

    const int16_t y = (int16_t)(oy + (int16_t)row);
    if (bitsPerPixel_ == 4) {
      drawCoverageRow(canvas, words, g->wordsPerRow, (uint16_t)(g->bitsPerRow / 4), ox, y, color);
      continue;
    }
    // Emitted as horizontal runs rather than one drawPixel per lit pixel.
    // Both are correct; a run is one clip test and one tight write loop
    // inside GFXcanvas16 instead of a virtual call and a bounds check per
    // pixel, which is also how u8g2 drew (draw_hv_line). The pixels chosen
    // are identical either way - verified against the previous build with
    // a full-screen diff, 0 of 129600 differing.
    //
    // The bit-to-x mapping mirrors lib/bdffont.ts exactly, including that
    // its loop starts at `bitsPerRow` rather than `bitsPerRow - 1`: the
    // first iteration tests a bit above the row's width, always 0, so
    // every drawn pixel lands one to the right of where BBX says. See this
    // class's header - the designer does the same, the old u8g2 path
    // cancelled it with textX + 1, and reproducing it keeps existing
    // projects identical.
    int runStart = -1;
    for (int bit = (int)g->bitsPerRow, x = 0; bit >= 0; bit--, x++) {
      const bool on = rowBit(words, g->wordsPerRow, bit);
      if (on) {
        if (runStart < 0) runStart = x;
      } else if (runStart >= 0) {
        canvas.drawFastHLine((int16_t)(ox + runStart), y, (int16_t)(x - runStart), color);
        runStart = -1;
      }
    }
    if (runStart >= 0) {
      const int end = (int)g->bitsPerRow + 1;
      canvas.drawFastHLine((int16_t)(ox + runStart), y, (int16_t)(end - runStart), color);
    }
  }
  return g->dwidth;
}

// One row of a 4-bit glyph. Pixel i of the row lands at ox + 1 + i - the
// same one-to-the-right as the 1-bit loop above, whose first iteration tests
// a bit beyond the row, so both kinds of font sit on the same pixel and the
// designer (lib/bdffont.ts) reproduces it for both. Full coverage is written
// as runs, as the 1-bit path does; an edge pixel is mixed into what is
// already on the canvas there - objects are drawn back to front, and a
// region redrawn under text redraws the text too, as the level glow relies on.
//
// Written straight into the canvas's buffer where it can be (rotation 0, as
// every board draws): the row clipped once, a word without ink skipped
// whole, each pixel read and written in place. Through drawPixel/getPixel -
// two virtual calls, a clip test and a bounds test for every edge pixel -
// a screen of text in the 4-bit faces redrew 32 % slower on the 4.3B than
// in the 1-bit ones (hil/redraw-time.js, 2026-11-02). The pixels are the
// same either way: full coverage is `color`, an edge blendCoverage565.
void BdfFont::drawCoverageRow(ClippedCanvas16& canvas, const uint32_t* words, uint16_t wordsPerRow,
                              uint16_t digits, int16_t ox, int16_t y, uint16_t color) const {
  uint16_t* buffer = canvas.getRotation() == 0 ? canvas.getBuffer() : nullptr;
  if (buffer != nullptr) {
    const int width = canvas.width();
    if (y < 0 || y >= canvas.height()) return;
    int x0 = 0, x1 = width;
    if (canvas.clipActive()) {
      if (y < canvas.clipY() || y >= canvas.clipY() + canvas.clipH()) return;
      if (canvas.clipX() > x0) x0 = canvas.clipX();
      if (canvas.clipX() + canvas.clipW() < x1) x1 = canvas.clipX() + canvas.clipW();
    }
    uint16_t* line = buffer + (int32_t)y * width;
    const int left = ox + 1;
    int i = 0;
    while (i < (int)digits) {
      const int j = (int)digits - 1 - i;  // from the right end, as stored
      const int nibble = j & 7;
      const uint32_t word = (j >> 3) < (int)wordsPerRow ? words[j >> 3] : 0;
      if (word == 0) {
        i += nibble + 1;  // this word's digits down to its lowest
        continue;
      }
      const uint8_t a = (uint8_t)((word >> (nibble * 4)) & 0x0F);
      const int x = left + i;
      i++;
      if (a == 0 || x < x0 || x >= x1) continue;
      line[x] = a == 15 ? color : glyphpixels::blendCoverage565(line[x], color, a);
    }
    return;
  }
  int runStart = -1;
  for (int i = 0; i <= (int)digits; i++) {
    const uint8_t a = rowCoverage(words, wordsPerRow, digits, i);
    if (a == 15) {
      if (runStart < 0) runStart = i;
      continue;
    }
    if (runStart >= 0) {
      canvas.drawFastHLine((int16_t)(ox + 1 + runStart), y, (int16_t)(i - runStart), color);
      runStart = -1;
    }
    if (a == 0) continue;
    const int16_t x = (int16_t)(ox + 1 + i);
    canvas.drawPixel(x, y, glyphpixels::blendCoverage565(canvas.getPixel(x, y), color, a));
  }
}

void BdfFont::drawText(ClippedCanvas16& canvas, const String& utf8, int16_t bx, int16_t by,
                       uint16_t color) const {
  int16_t x = bx;
  size_t i = 0;
  while (i < utf8.length()) x += drawGlyph(canvas, nextCodepoint(utf8, i), x, by, color);
}
