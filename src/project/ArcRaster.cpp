#include "ArcRaster.h"
#include <esp_cpu.h>
#include "ArcSinTable.h"

namespace ArcRaster {

void direction(int32_t angle64, int32_t* outX, int32_t* outY) {
  int32_t a = angle64 % FULL_TURN;
  if (a < 0) a += FULL_TURN;

  const int32_t deg = a / ANGLE_SCALE;
  const int32_t frac = a - deg * ANGLE_SCALE;

  const int32_t sin0 = ARC_SIN_TABLE[deg];
  const int32_t sin1 = ARC_SIN_TABLE[(deg + 1) % 360];
  // cos(d) is sin(d + 90), so the same table serves both.
  const int32_t cos0 = ARC_SIN_TABLE[(deg + 90) % 360];
  const int32_t cos1 = ARC_SIN_TABLE[(deg + 91) % 360];

  // Interpolated between whole degrees rather than looked up, because the
  // fill edge has to move smoothly: a whole degree at radius 168 is nearly
  // three pixels of arc, and a table-only lookup would make the filled arc
  // advance in visible jumps. The result is not exactly on the unit circle,
  // but only the direction is used - it feeds cross products, whose sign is
  // unaffected by length.
  const int32_t sinA = (sin0 * (ANGLE_SCALE - frac) + sin1 * frac + 32) >> 6;
  const int32_t cosA = (cos0 * (ANGLE_SCALE - frac) + cos1 * frac + 32) >> 6;

  // Twelve o'clock is straight up, which is -y on a screen.
  *outX = sinA;
  *outY = -cosA;
}

Sector makeSector(int32_t start64, int32_t sweep64) {
  Sector s;
  if (sweep64 <= 0) {
    s.empty = true;
    return s;
  }
  if (sweep64 >= FULL_TURN) {
    s.empty = false;
    s.full = true;
    return s;
  }
  direction(start64, &s.ux, &s.uy);
  direction(start64 + sweep64, &s.vx, &s.vy);
  s.wide = sweep64 >= FULL_TURN / 2;
  s.full = false;
  s.empty = false;
  return s;
}

bool IRAM_ATTR inSector(const Sector& s, int32_t x, int32_t y) {
  if (s.full) return true;
  if (s.empty) return false;
  // cross(u, p) > 0 means p is clockwise of u, with y downwards. Up to a
  // half turn that is an intersection of two half-planes; beyond it, the
  // same two have to be unioned instead, which is what `wide` selects.
  const int32_t crossU = s.ux * y - s.uy * x;
  const int32_t crossV = s.vx * y - s.vy * x;
  return s.wide ? (crossU >= 0 || crossV < 0) : (crossU >= 0 && crossV < 0);
}

// True when inSector() is guaranteed to answer the same for every point of a
// box of half-extent h about (cx, cy) - i.e. neither of the two cross
// products it tests can change sign, or reach zero, anywhere in the box.
//
// The bound is the exact worst case: cross(u, c+d) = cross(u, c) + ux*dy -
// uy*dx, and |ux*dy - uy*dx| <= h*(|ux|+|uy|) for |dx|,|dy| <= h. Strict
// comparison because inSector() treats zero as a side of its own.
static bool IRAM_ATTR sectorUniform(const Sector& s, int32_t cx, int32_t cy, int32_t h) {
  if (s.full || s.empty) return true;  // no boundary to cross
  const int32_t crossU = s.ux * cy - s.uy * cx;
  const int32_t boundU = h * (abs(s.ux) + abs(s.uy));
  if (crossU <= boundU && crossU >= -boundU) return false;
  const int32_t crossV = s.vx * cy - s.vy * cx;
  const int32_t boundV = h * (abs(s.vx) + abs(s.vy));
  if (crossV <= boundV && crossV >= -boundV) return false;
  return true;
}

bool& fastPathEnabled() {
  static bool on = true;
  return on;
}

Probe& probe() {
  static Probe p;
  return p;
}

bool& probeEnabled() {
  static bool on = false;
  return on;
}

void probeReset() { probe() = Probe(); }

// Largest integer whose square is at most v. A binary search rather than
// sqrtf(), because this decides a loop bound that has to agree with an
// integer comparison exactly, and a float one ulp low would drop a covered
// pixel. Sixteen iterations, run once per row against 360 pixels.
static int32_t isqrt(int32_t v) {
  if (v <= 0) return 0;
  int32_t lo = 0, hi = 46340;  // 46340^2 is the largest square in int32
  while (lo < hi) {
    const int32_t mid = lo + (hi - lo + 1) / 2;
    if ((int64_t)mid * mid <= (int64_t)v) lo = mid;
    else hi = mid - 1;
  }
  return lo;
}

// C++ truncates toward zero and these bounds go negative, so the rounding
// direction has to be said out loud.
static int32_t floorDiv(int32_t a, int32_t b) {
  int32_t q = a / b;
  if ((a % b != 0) && ((a < 0) != (b < 0))) q--;
  return q;
}

static int32_t ceilDiv(int32_t a, int32_t b) {
  int32_t q = a / b;
  if ((a % b != 0) && ((a < 0) == (b < 0))) q++;
  return q;
}

RowSpan rowSpan(const RingGeometry& geom, int32_t py) {
  RowSpan span;

  const int32_t S = SUBPIXEL_SCALE;
  const int32_t centre = (geom.size * S) / 2;
  const int32_t rOuter = centre;
  const int32_t rInner = rOuter - geom.thickness * S;
  const int32_t rOuter2 = rOuter * rOuter;
  const int32_t rInner2 = rInner > 0 ? rInner * rInner : 0;

  // The same box of sub-sample centres pixelBands() builds: offsets 1 and
  // 2*SUBSAMPLES-1 from the pixel's top-left corner, on the 1/8 lattice.
  const int32_t far = 2 * SUBSAMPLES - 1;
  const int32_t yLo = py * S + 1 - centre;
  const int32_t yHi = py * S + far - centre;
  const int32_t minAbsY = (yLo <= 0 && yHi >= 0) ? 0 : min(abs(yLo), abs(yHi));
  const int32_t maxAbsY = max(abs(yLo), abs(yHi));

  // Outside the ring: pixelBands() drops the pixel when
  // minAbsX^2 + minAbsY^2 >= rOuter2. Keeping it therefore means
  // minAbsX^2 < rOuter2 - minAbsY^2, and with the square taken off,
  // minAbsX <= isqrt(that - 1). minAbsX is the distance from the y axis to
  // the nearer edge of the box, so the condition is that [xLo, xHi] reaches
  // into [-t, t] at all.
  const int32_t outerRoom = rOuter2 - minAbsY * minAbsY;
  if (outerRoom <= 0) return span;  // x1 < x0: no pixel of this row is covered
  const int32_t t = isqrt(outerRoom - 1);
  span.x0 = ceilDiv(centre - far - t, S);
  span.x1 = floorDiv(centre + t - 1, S);

  // Inside the hole: dropped when maxAbsX^2 + maxAbsY^2 < rInner2, and
  // maxAbsX is the distance to the further edge, so this time the whole box
  // has to lie within [-u, u].
  const int32_t innerRoom = rInner2 - maxAbsY * maxAbsY;
  if (innerRoom > 0) {
    const int32_t u = isqrt(innerRoom - 1);
    span.holeX0 = ceilDiv(centre - u - 1, S);
    span.holeX1 = floorDiv(centre + u - far, S);
  }

  return span;
}

PixelBands IRAM_ATTR pixelBands(const RingGeometry& geom, int32_t px, int32_t py) {
  PixelBands bands;
  const uint32_t probeStart = probeEnabled() ? esp_cpu_get_cycle_count() : 0;

  const int32_t S = SUBPIXEL_SCALE;
  // The ring touches the object's edge, so the outer radius is half the side.
  const int32_t centre = (geom.size * S) / 2;
  const int32_t rOuter = (geom.size * S) / 2;
  const int32_t rInner = rOuter - geom.thickness * S;
  const int32_t rOuter2 = rOuter * rOuter;
  const int32_t rInner2 = rInner > 0 ? rInner * rInner : 0;

  // Two exact short cuts before sampling - not approximations, so they are
  // part of the shared algorithm rather than a judgement call either side
  // could make differently. Between them they dismiss the whole middle of
  // the dial and all four corners, which on a 360px ring is most of it.
  const int32_t xLo = px * S + 1 - centre;
  const int32_t xHi = px * S + (2 * SUBSAMPLES - 1) - centre;
  const int32_t yLo = py * S + 1 - centre;
  const int32_t yHi = py * S + (2 * SUBSAMPLES - 1) - centre;
  const int32_t minAbsX = (xLo <= 0 && xHi >= 0) ? 0 : min(abs(xLo), abs(xHi));
  const int32_t minAbsY = (yLo <= 0 && yHi >= 0) ? 0 : min(abs(yLo), abs(yHi));
  const int32_t maxAbsX = max(abs(xLo), abs(xHi));
  const int32_t maxAbsY = max(abs(yLo), abs(yHi));
  if (minAbsX * minAbsX + minAbsY * minAbsY >= rOuter2) {
    if (probeEnabled()) { probe().earlyCalls++;
    probe().earlyCycles += esp_cpu_get_cycle_count() - probeStart; }
    return bands;
  }
  if (maxAbsX * maxAbsX + maxAbsY * maxAbsY < rInner2) {
    if (probeEnabled()) { probe().earlyCalls++;
    probe().earlyCycles += esp_cpu_get_cycle_count() - probeStart; }
    return bands;
  }

  // Third exact short cut: a pixel no boundary crosses cannot disagree with
  // itself, so one evaluation stands for all sixteen. See this file's patch
  // note for the derivation; the two conditions are "the whole sub-sample
  // box is inside the annulus" and "no sector edge passes through it".
  //
  // The box of sub-sample centres spans offsets 1..2*SUBSAMPLES-1, so its
  // centre is at +SUBSAMPLES and its half-extent is SUBSAMPLES-1.
  {
    const int32_t boxCx = px * S + SUBSAMPLES - centre;
    const int32_t boxCy = py * S + SUBSAMPLES - centre;
    const int32_t boxH = SUBSAMPLES - 1;
    if (fastPathEnabled() &&
        maxAbsX * maxAbsX + maxAbsY * maxAbsY < rOuter2 &&
        minAbsX * minAbsX + minAbsY * minAbsY >= rInner2 &&
        sectorUniform(geom.marker, boxCx, boxCy, boxH) &&
        sectorUniform(geom.fill, boxCx, boxCy, boxH) &&
        sectorUniform(geom.track, boxCx, boxCy, boxH)) {
      // Same priority order as the sampling loop below, and it has to stay
      // that way: a sub-sample is counted into exactly one band.
      if (inSector(geom.marker, boxCx, boxCy)) bands.marker = COVERAGE_MAX;
      else if (inSector(geom.fill, boxCx, boxCy)) bands.fill = COVERAGE_MAX;
      else if (inSector(geom.track, boxCx, boxCy)) bands.track = COVERAGE_MAX;
      if (probeEnabled()) { probe().uniformCalls++;
      probe().uniformCycles += esp_cpu_get_cycle_count() - probeStart; }
      return bands;
    }
  }

  for (int32_t j = 0; j < SUBSAMPLES; j++) {
    // Sub-sample centres sit at (2k+1)/8 of a pixel: 1/8, 3/8, 5/8, 7/8.
    const int32_t y = py * S + 2 * j + 1 - centre;
    for (int32_t i = 0; i < SUBSAMPLES; i++) {
      const int32_t x = px * S + 2 * i + 1 - centre;
      const int32_t d2 = x * x + y * y;
      if (d2 >= rOuter2 || d2 < rInner2) continue;

      // Each sub-sample is counted into exactly one band. Rasterizing the
      // bands separately and compositing them looks equivalent and is not:
      // where the fill's radial edge meets the ring's outer edge both have
      // the same partial coverage, and one over the other leaves a seam of
      // track colour that should not be there.
      if (inSector(geom.marker, x, y)) bands.marker++;
      else if (inSector(geom.fill, x, y)) bands.fill++;
      else if (inSector(geom.track, x, y)) bands.track++;
      // Inside the annulus but outside the track - the gap at the bottom of
      // a 270 degree dial. Stays background.
    }
  }

  if (probeEnabled()) { probe().sampledCalls++;
  probe().sampledCycles += esp_cpu_get_cycle_count() - probeStart; }
  return bands;
}

Rgb565 toRgb565(uint16_t packed) {
  Rgb565 c;
  c.r = (packed >> 11) & 0x1F;
  c.g = (packed >> 5) & 0x3F;
  c.b = packed & 0x1F;
  return c;
}

uint16_t packRgb565(const Rgb565& c) {
  return (uint16_t)(((uint16_t)c.r << 11) | ((uint16_t)c.g << 5) | (uint16_t)c.b);
}

uint16_t blendBands(const Rgb565& fill, uint8_t fillCount,
                    const Rgb565& track, uint8_t trackCount,
                    const Rgb565& marker, uint8_t markerCount,
                    const Rgb565& background, uint8_t backgroundCount) {
  const int32_t r = fill.r * fillCount + track.r * trackCount + marker.r * markerCount + background.r * backgroundCount;
  const int32_t g = fill.g * fillCount + track.g * trackCount + marker.g * markerCount + background.g * backgroundCount;
  const int32_t b = fill.b * fillCount + track.b * trackCount + marker.b * markerCount + background.b * backgroundCount;

  // The rounding is written out rather than inherited: the grayscale mask
  // blend in ColorAssetLoader divides by 255 without rounding, and an
  // unrounded divide here would bias every anti-aliased edge half a step
  // darker on one side. All inputs are non-negative, so a plain divide and
  // the designer's Math.floor agree.
  const int32_t half = COVERAGE_MAX / 2;
  Rgb565 out;
  out.r = (uint8_t)((r + half) / COVERAGE_MAX);
  out.g = (uint8_t)((g + half) / COVERAGE_MAX);
  out.b = (uint8_t)((b + half) / COVERAGE_MAX);
  return packRgb565(out);
}

int32_t sweepForPercent(int32_t sweep64, float percent) {
  float clamped = percent;
  if (clamped < 0.0f) clamped = 0.0f;
  if (clamped > 100.0f) clamped = 100.0f;
  // Truncation, matching the designer's Math.trunc - which is itself there
  // to match C's assignment to an int, the same pairing the level indicator
  // bar already relies on.
  return (int32_t)((sweep64 * clamped) / 100.0f);
}

}  // namespace ArcRaster
