#pragma once
// levelglow::gradient565 - designer lib/level-glow.ts gradient565 - in a
// header of its own, free of Arduino, so LevelGlow.h (the boards) and the
// needle raster (which also builds on the PC, test/test_needle) share the one
// formula. Any colour type with r, g, b channels (ArcRaster::Rgb565 on the
// boards).

#include <stdint.h>

namespace levelglow {

// Step `step` of `steps` between two colours, taken at the step's middle.
// Every operand non-negative, so the division floors as the designer's does.
template <class Colour>
inline Colour gradient565(const Colour& from, const Colour& to, int32_t step, int32_t steps) {
  int32_t s = step < 0 ? 0 : (step > steps - 1 ? steps - 1 : step);
  const int32_t w = 2 * s + 1;
  const int32_t n = 2 * steps;
  Colour out;
  out.r = (decltype(out.r))((from.r * (n - w) + to.r * w + steps) / n);
  out.g = (decltype(out.g))((from.g * (n - w) + to.g * w + steps) / n);
  out.b = (decltype(out.b))((from.b * (n - w) + to.b * w + steps) / n);
  return out;
}

}  // namespace levelglow
