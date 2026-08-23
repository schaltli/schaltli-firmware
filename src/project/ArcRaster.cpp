#include "ArcRaster.h"
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

bool inSector(const Sector& s, int32_t x, int32_t y) {
  if (s.full) return true;
  if (s.empty) return false;
  // cross(u, p) > 0 means p is clockwise of u, with y downwards. Up to a
  // half turn that is an intersection of two half-planes; beyond it, the
  // same two have to be unioned instead, which is what `wide` selects.
  const int32_t crossU = s.ux * y - s.uy * x;
  const int32_t crossV = s.vx * y - s.vy * x;
  return s.wide ? (crossU >= 0 || crossV < 0) : (crossU >= 0 && crossV < 0);
}

PixelBands pixelBands(const RingGeometry& geom, int32_t px, int32_t py) {
  PixelBands bands;

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
  if (minAbsX * minAbsX + minAbsY * minAbsY >= rOuter2) return bands;
  if (maxAbsX * maxAbsX + maxAbsY * maxAbsY < rInner2) return bands;

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
