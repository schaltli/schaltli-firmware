#pragma once
#include <stddef.h>
#include <stdint.h>

// How a BDF glyph's hex rows become pixels - for 1-bit fonts and, since
// 2026-11-01, for 4-bit ones (designer docs/2026-11-01-4bit-fonts.md and
// docs/device-contract.md, «Font depth»). Pure arithmetic, no Arduino, so
// `pio test -e native` checks it (test/test_glyph_pixels) against the same
// worked values the designer's lib/bdffont.ts is checked against.
//
// A row is stored the way BdfFont has stored it since 2026-09-30: one hex
// digit is 4 bits, packed into 32-bit words, word 0 holding the rightmost
// eight digits. A 1-bit font reads that as bits; a 4-bit font reads each
// digit as one pixel's coverage, 0 (none) to 15 (full) - the same storage,
// two pixels per byte.
namespace glyphpixels {

// The 32-bit words a row of `len` hex digits takes.
inline uint16_t wordsFor(size_t len) {
  return (uint16_t)((len + 7) / 8);
}

// Parses a hex row into `words`, word 0 holding its rightmost 32 bits.
// Until 2026-09-30 a row was one uint32_t, and a glyph wider than 32 px -
// FreeUniversal 42's "D", every digit of DSEG7 - lost its left part: the
// designer's lib/bdffont.ts had the same limit and the same fix.
inline void parseHexRow(const char* s, size_t len, uint32_t* words) {
  const uint16_t n = wordsFor(len);
  for (uint16_t w = 0; w < n; w++) words[w] = 0;
  for (size_t j = 0; j < len; j++) {
    // j counts hex digits from the right end of the row.
    const char c = s[len - 1 - j];
    uint32_t d;
    if (c >= '0' && c <= '9') d = (uint32_t)(c - '0');
    else if (c >= 'a' && c <= 'f') d = (uint32_t)(c - 'a' + 10);
    else if (c >= 'A' && c <= 'F') d = (uint32_t)(c - 'A' + 10);
    else d = 0;
    words[j / 8] |= d << ((j % 8) * 4);
  }
}

// 1 bit: bit `bit` of a row counted from its right end; 0 beyond the row.
inline bool rowBit(const uint32_t* words, uint16_t wordsPerRow, int bit) {
  if (bit < 0 || (bit >> 5) >= (int)wordsPerRow) return false;
  return ((words[bit >> 5] >> (bit & 31)) & 0x01) != 0;
}

// 4 bits: the coverage of pixel `i` of a row of `digits` pixels, counted
// from the left; 0 outside the row.
inline uint8_t rowCoverage(const uint32_t* words, uint16_t wordsPerRow, uint16_t digits, int i) {
  if (i < 0 || i >= (int)digits) return 0;
  const int j = (int)digits - 1 - i;  // from the right end, as stored
  if ((j >> 3) >= (int)wordsPerRow) return 0;
  return (uint8_t)((words[j >> 3] >> ((j & 7) * 4)) & 0x0F);
}

// Coverage 0-15 of `ink` over `under`, both packed RGB565. Coverage becomes
// an alpha out of 256, floor((c * 256 + 7) / 15), and the channels mix as
// levelglow::blend565 mixes them: (u * (256 - a) + i * a + 128) >> 8 per
// 5/6/5 channel. 0 leaves `under`, 15 is `ink` exactly.
inline uint16_t blendCoverage565(uint16_t under, uint16_t ink, uint8_t coverage) {
  if (coverage == 0) return under;
  if (coverage >= 15) return ink;
  const uint32_t a = ((uint32_t)coverage * 256 + 7) / 15;
  const uint32_t ur = (under >> 11) & 31, ug = (under >> 5) & 63, ub = under & 31;
  const uint32_t ir = (ink >> 11) & 31, ig = (ink >> 5) & 63, ib = ink & 31;
  const uint32_t r = (ur * (256 - a) + ir * a + 128) >> 8;
  const uint32_t g = (ug * (256 - a) + ig * a + 128) >> 8;
  const uint32_t b = (ub * (256 - a) + ib * a + 128) >> 8;
  return (uint16_t)((r << 11) | (g << 5) | b);
}

}  // namespace glyphpixels
