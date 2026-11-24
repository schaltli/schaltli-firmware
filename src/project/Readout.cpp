#include "Readout.h"

#include <ctype.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

namespace readout {

size_t format(const char* raw, int32_t decimals, char* out) {
  out[0] = '\0';
  if (!raw) return 0;
  const char* s = raw;
  while (*s && isspace((unsigned char)*s)) s++;
  if (!*s) return 0;
  char* end = nullptr;
  const double v = strtod(s, &end);
  if (end == s) return 0;
  while (*end && isspace((unsigned char)*end)) end++;
  if (*end) return 0;  // anything else after the number: no number
  if (!isfinite(v)) return 0;
  // A product of 10s, as the designer builds it.
  double p = 1;
  for (int32_t i = 0; i < decimals; i++) p *= 10;
  // volatile: each product rounded on its own, never fused into the add.
  volatile double scaled = fabs(v) * p;
  const double m = floor(scaled + 0.5);
  const double whole = floor(m / p);
  volatile double wholeScaled = whole * p;
  const double rest = m - wholeScaled;
  char buf[TEXT_MAX];
  const char* minus = (v < 0 && m != 0) ? "-" : "";
  const int n = decimals > 0 ? snprintf(buf, sizeof buf, "%s%.0f.%0*.0f", minus, whole, (int)decimals, rest)
                             : snprintf(buf, sizeof buf, "%s%.0f", minus, whole);
  // Longer than any readout can hold: nothing rather than half a number.
  if (n <= 0 || (size_t)n >= TEXT_MAX) return 0;
  memcpy(out, buf, (size_t)n + 1);
  return (size_t)n;
}

int32_t start(const char* text, const Description& d) {
  int32_t total = 0;
  for (const char* c = text; *c; c++) {
    const size_t i = d.chars.find(*c);
    if (i != std::string::npos && i < d.cells.size()) total += d.cells[i].advance;
  }
  return d.center ? d.x - (total >> 1) : d.x - total;
}

void draw(needleraster::Target& target, const Description& d, const uint8_t* alpha, int32_t stripWidth, int32_t stripHeight,
          const char* text, int32_t opacity) {
  if (!target.pixels || !alpha || !text || target.width <= 0 || target.height <= 0) return;
  // The clip as needleraster::draw() takes it: -1 is the whole picture.
  const int32_t cx0 = target.clipX0 < 0 ? 0 : target.clipX0;
  const int32_t cy0 = target.clipY0 < 0 ? 0 : target.clipY0;
  const int32_t cx1 = (target.clipX1 < 0 || target.clipX1 > target.width) ? target.width : target.clipX1;
  const int32_t cy1 = (target.clipY1 < 0 || target.clipY1 > target.height) ? target.height : target.clipY1;
  if (cx0 >= cx1 || cy0 >= cy1) return;
  // Rows of the cell inside the clip (and inside the strip).
  int32_t yFrom = cy0 - d.y, yTo = cy1 - d.y;
  if (yFrom < 0) yFrom = 0;
  if (yTo > d.height) yTo = d.height;
  if (yTo > stripHeight) yTo = stripHeight;
  int32_t pen = start(text, d);
  for (const char* c = text; *c; c++) {
    const size_t i = d.chars.find(*c);
    if (i == std::string::npos || i >= d.cells.size()) continue;
    const Cell& cell = d.cells[i];
    const int32_t ox = pen - d.pad;
    pen += cell.advance;
    if (cell.sx < 0 || cell.sx + cell.w > stripWidth) continue;  // a strip that does not match its cells
    int32_t xFrom = cx0 - ox, xTo = cx1 - ox;
    if (xFrom < 0) xFrom = 0;
    if (xTo > cell.w) xTo = cell.w;
    for (int32_t y = yFrom; y < yTo; y++) {
      const uint8_t* src = alpha + (size_t)y * stripWidth + cell.sx;
      uint16_t* row = target.pixels + (ptrdiff_t)(d.y + y) * target.stride + ox;
      for (int32_t x = xFrom; x < xTo; x++) {
        const int32_t a8 = src[x];
        if (a8 == 0) continue;
        // An alpha byte as a blend565 weight out of 256: a + (a >> 7).
        const int32_t a = ((a8 + (a8 >> 7)) * opacity) >> 8;
        if (a <= 0) continue;
        row[x] = needleraster::blend565(row[x], d.color, a);
      }
    }
  }
}

}  // namespace readout
