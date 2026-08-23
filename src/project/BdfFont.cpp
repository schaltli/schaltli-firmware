#include "BdfFont.h"

#include <esp_heap_caps.h>
#include <string.h>

namespace {

// Walks the source line by line without copying it - these files are
// 100-200KB and there are four of them.
struct LineReader {
  const char* p;
  const char* end;

  bool next(const char*& lineStart, size_t& lineLen) {
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

uint32_t parseHexRow(const char* s, size_t len) {
  uint32_t v = 0;
  for (size_t i = 0; i < len; i++) {
    const char c = s[i];
    uint32_t d;
    if (c >= '0' && c <= '9') d = (uint32_t)(c - '0');
    else if (c >= 'a' && c <= 'f') d = (uint32_t)(c - 'a' + 10);
    else if (c >= 'A' && c <= 'F') d = (uint32_t)(c - 'A' + 10);
    else break;
    v = (v << 4) | d;
  }
  return v;
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
  release();
  fontAscent_ = 0;
  fontDescent_ = 0;

  // Pass 1: how many glyphs, how many bitmap rows. Counting first means one
  // allocation of the right size for each, instead of growth that would
  // fragment PSRAM for everything after it.
  uint32_t glyphs = 0;
  uint32_t rows = 0;
  {
    LineReader reader{text, text + length};
    const char* line;
    size_t len;
    bool inChar = false;
    bool inBitmap = false;
    while (reader.next(line, len)) {
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
        if (isHexLine(line, len)) rows++;
      } else if (startsWith(line, len, "BITMAP")) {
        inBitmap = true;
      }
    }
  }
  if (glyphs == 0) return false;

  glyphs_ = (Glyph*)heap_caps_malloc((size_t)glyphs * sizeof(Glyph), MALLOC_CAP_SPIRAM);
  rows_ = (uint32_t*)heap_caps_malloc((size_t)(rows == 0 ? 1 : rows) * sizeof(uint32_t), MALLOC_CAP_SPIRAM);
  if (glyphs_ == nullptr || rows_ == nullptr) {
    Serial.printf("[bdf] no PSRAM for %u glyphs / %u rows\n", (unsigned)glyphs, (unsigned)rows);
    release();
    return false;
  }

  // Pass 2: fill.
  LineReader reader{text, text + length};
  const char* line;
  size_t len;
  bool inChar = false;
  bool inBitmap = false;
  Glyph current;
  long encoding = -1;
  uint32_t glyphIndex = 0;
  uint32_t rowIndex = 0;

  while (reader.next(line, len)) {
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
      if (isHexLine(line, len) && rowIndex < rows) {
        // BDF pads each row to whole hex digits, and the designer derives
        // its shift width from that padded length rather than from BBw -
        // drawGlyph depends on matching it exactly.
        current.bitsPerRow = (uint16_t)(len * 4);
        rows_[rowIndex++] = parseHexRow(line, len);
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

int16_t BdfFont::drawGlyph(ClippedCanvas16& canvas, uint16_t codepoint, int16_t bx, int16_t by,
                           uint16_t color) const {
  const Glyph* g = find(codepoint);
  if (g == nullptr) return 0;

  const int16_t ox = (int16_t)(bx + g->bbox);
  const int16_t oy = (int16_t)(by - g->bboy - g->bbh);

  for (uint16_t row = 0; row < g->rowCount; row++) {
    const uint32_t bits = rows_[g->rowOffset + row];
    // Blank rows are most of a glyph's box - the space above an 'a', below
    // everything without a descender - and skipping them costs one test.
    if (bits == 0) continue;

    const int16_t y = (int16_t)(oy + (int16_t)row);
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
      const bool on = ((bits >> bit) & 0x01) != 0;
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

void BdfFont::drawText(ClippedCanvas16& canvas, const String& utf8, int16_t bx, int16_t by,
                       uint16_t color) const {
  int16_t x = bx;
  size_t i = 0;
  while (i < utf8.length()) x += drawGlyph(canvas, nextCodepoint(utf8, i), x, by, color);
}
