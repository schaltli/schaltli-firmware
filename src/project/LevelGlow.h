#pragma once
// A level's fill as a gradient, and the weak glow around it - the designer's
// lib/level-glow.ts, integer for integer. A theme gives them to a bar, slider,
// gauge or dial whose fill is its accent (designer lib/themes.ts, 2026-09-28);
// the export writes fillEndColor and glow, and a project without them draws
// exactly as before.
//
// The glow is mixed into whatever already stands on the canvas where it falls
// - the screen's colour, an icon behind the control, another control - which
// the renderer reads back with getPixel(): objects are drawn back to front, so
// what lies under the level is already there. It stays inside the object's own
// box, so a region redrawn under it redraws the level too.

#include <stdint.h>

#include "ArcRaster.h"
#include "Gradient565.h"

namespace levelglow {

// How far the glow reaches beyond the band, in whole pixels (the "weak" glow).
static const int32_t LEVELS = 8;

// How strongly the glow is mixed in, out of 256, by how many pixels from the
// band a pixel lies: 1 touches it, 8 is the last. 0 is no glow.
static const int32_t ALPHA[LEVELS + 1] = {0, 128, 100, 76, 56, 40, 26, 15, 7};

// `col` over `dst` at alpha/256, rounded, per 5/6/5 channel.
inline ArcRaster::Rgb565 blend565(const ArcRaster::Rgb565& dst, const ArcRaster::Rgb565& col, int32_t alpha) {
  ArcRaster::Rgb565 out;
  out.r = (uint8_t)((dst.r * (256 - alpha) + col.r * alpha + 128) >> 8);
  out.g = (uint8_t)((dst.g * (256 - alpha) + col.g * alpha + 128) >> 8);
  out.b = (uint8_t)((dst.b * (256 - alpha) + col.b * alpha + 128) >> 8);
  return out;
}

// gradient565(from, to, step, steps): Gradient565.h, shared with the needle
// raster.

// 1 inside or touching the band, up to LEVELS, 0 beyond - from a squared
// distance to the band's centre line and half its width, in 1/8 pixel.
inline int32_t levelFromDistance2(int64_t d2, int32_t half) {
  if (d2 < (int64_t)half * half) return 1;
  for (int32_t k = 1; k <= LEVELS; k++) {
    const int64_t r = half + k * 8;
    if (d2 < r * r) return k;
  }
  return 0;
}

// Squared distance from (x, y) to an axis-aligned segment (a bar's centre
// line), all integers.
inline int64_t segmentDistance2(int32_t x, int32_t y, int32_t ax, int32_t ay, int32_t bx, int32_t by) {
  const int64_t vx = bx - ax, vy = by - ay, wx = x - ax, wy = y - ay;
  const int64_t len2 = vx * vx + vy * vy;
  const int64_t dot = wx * vx + wy * vy;
  if (len2 == 0 || dot <= 0) return wx * wx + wy * wy;
  if (dot >= len2) {
    const int64_t ex = x - bx, ey = y - by;
    return ex * ex + ey * ey;
  }
  if (vx == 0) return wx * wx;
  return wy * wy;
}

// How far an object's glow reaches, in pixels, from its `glow` property.
inline int32_t glowPx(int32_t glowProperty) {
  if (glowProperty <= 0) return 0;
  return glowProperty > LEVELS ? LEVELS : glowProperty;
}

}  // namespace levelglow
