#include "ArcRaster.h"
#include <esp_cpu.h>
#include <math.h>
#include "ArcSinTable.h"

namespace ArcRaster {

// One pixel of the frame, in 1/8 units.
static const int32_t FRAME = SUBPIXEL_SCALE;

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

// Whether a point is inside a cap's disc.
static bool IRAM_ATTR inCap(const Cap& cap, int32_t x, int32_t y) {
  const int32_t dx = x - cap.cx;
  const int32_t dy = y - cap.cy;
  return dx * dx + dy * dy <= cap.r * cap.r;
}

// Whether a point inside a cap is within the frame's own pixel of its edge.
// One half of what draws the outline on a 1-bit panel; the other half is the
// outer/inner radius test in pixelBands().
static bool IRAM_ATTR capEdge(const Cap& cap, int32_t x, int32_t y) {
  const int32_t dx = x - cap.cx;
  const int32_t dy = y - cap.cy;
  const int32_t inner = cap.r - FRAME;
  return dx * dx + dy * dy >= inner * inner;
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

// The squared distance from the nearest point of a box of half-extent h about
// (cx, cy) to the point (tx, ty). Used only to prove that a box is entirely
// clear of something, never to decide what a sub-sample becomes.
static int32_t IRAM_ATTR boxMinDist2(int32_t cx, int32_t cy, int32_t h, int32_t tx, int32_t ty) {
  int32_t dx = abs(cx - tx) - h;
  int32_t dy = abs(cy - ty) - h;
  if (dx < 0) dx = 0;
  if (dy < 0) dy = 0;
  return dx * dx + dy * dy;
}

// Whether the handle can reach any sub-sample of this pixel at all.
//
// A firmware-only short cut of the same kind as rowSpan(), and exact for the
// same reason. The handle test below does something to a sub-sample only when
// |out| <= halfLength AND |along| <= halfWidth + gap; that region lies inside
// the disc of radius halfLength + halfWidth + gap about the handle's centre,
// so a pixel whose whole sub-sample box is outside that disc would fall
// straight through the test to the annulus. Skipping it there changes no
// pixel and keeps the only floating-point arithmetic in this file off the
// hundreds of pixels that are nowhere near the handle - which matters on a
// part with no double-precision unit.
static bool IRAM_ATTR handleNear(const RingGeometry& geom, int32_t boxCx, int32_t boxCy,
                                 int32_t boxH) {
  if (!geom.hasHandle) return false;
  const Handle& h = geom.handle;
  const int32_t reach = h.halfLength + h.halfWidth + h.gap;
  return boxMinDist2(boxCx, boxCy, boxH, h.cx, h.cy) <= reach * reach;
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

int32_t midRadius(int32_t size, int32_t thickness, int32_t inset) {
  return size * SUBPIXEL_SCALE / 2 - inset * SUBPIXEL_SCALE - thickness * SUBPIXEL_SCALE / 2;
}

void pointAt(int32_t size, int32_t thickness, int32_t inset, int32_t angle64, int32_t* outX,
             int32_t* outY) {
  int32_t dx = 0;
  int32_t dy = 0;
  direction(angle64, &dx, &dy);
  const int32_t rMid = midRadius(size, thickness, inset);
  // Rounded half toward positive infinity, which is what JS's Math.round
  // does. An integer divide here would truncate toward zero and put every cap
  // and every handle up to an eighth of a pixel out on the negative side of
  // the dial - and only there, so it would read as the ring being lopsided.
  //
  // Exact in double: the product is an integer well under 2^53, the divisor
  // is a power of two, so nothing is rounded before floor() sees it.
  *outX = (int32_t)floor((double)dx * (double)rMid / (double)ARC_SIN_SCALE + 0.5);
  *outY = (int32_t)floor((double)dy * (double)rMid / (double)ARC_SIN_SCALE + 0.5);
}

Caps caps(int32_t size, int32_t thickness, int32_t inset, int32_t start64, int32_t sweep64) {
  Caps out;
  if (sweep64 >= FULL_TURN) return out;  // all the way round: no ends to round
  out.has = true;
  const int32_t r = thickness * SUBPIXEL_SCALE / 2;
  direction(start64 - 90 * ANGLE_SCALE, &out.startCap.tx, &out.startCap.ty);
  direction(start64 + sweep64 + 90 * ANGLE_SCALE, &out.endCap.tx, &out.endCap.ty);
  pointAt(size, thickness, inset, start64, &out.startCap.cx, &out.startCap.cy);
  pointAt(size, thickness, inset, start64 + sweep64, &out.endCap.cx, &out.endCap.cy);
  out.startCap.r = r;
  out.endCap.r = r;
  return out;
}

Handle handleBand(int32_t size, int32_t thickness, int32_t inset, int32_t angle64,
                  int32_t sweep64) {
  const int32_t rMid = midRadius(size, thickness, inset);
  // The scale's own length along the centreline: 2*pi*r * sweep/turn, in
  // whole 1/8 pixels. 355/113 is pi to seven digits, in integers, so every
  // copy of this arrives at the same number.
  //
  // In 64-bit, not 32: a 360 px dial reaches 1.66e10 in the numerator, which
  // overflows int32 and would hand the handle a length taken from a
  // wrapped-round number.
  const int32_t runLength =
      (int32_t)((2LL * 355LL * (int64_t)rMid * (int64_t)sweep64) / (113LL * (int64_t)FULL_TURN));

  // Eleven quarters of the thickness long, an eleventh of that wide, with a
  // gap of three twenty-seconds each side: exactly LevelShape.h's
  // handleLength/handleWidth/handleGap, because "looks like the slider's" was
  // the whole point.
  //
  // Two clamps a straight bar never needs. A handle longer than twice the
  // centreline's radius would reach through the middle of the dial and out
  // the other side; one longer than a third of the scale's own run leaves the
  // fill nowhere to show. Width and gap follow the length the thickness ASKS
  // for - what a small dial is short of is room along the handle, not across
  // it.
  const int32_t wanted = thickness * SUBPIXEL_SCALE * 11 / 4;
  int32_t length = wanted;
  if (2 * rMid < length) length = 2 * rMid;
  if (runLength / 3 < length) length = runLength / 3;
  if (length < 2) length = 2;
  int32_t width = wanted / 11;
  if (width < 3) width = 3;
  int32_t gap = wanted * 3 / 22;
  if (gap < 2) gap = 2;

  Handle h;
  direction(angle64, &h.rx, &h.ry);
  direction(angle64 + 90 * ANGLE_SCALE, &h.tx, &h.ty);
  pointAt(size, thickness, inset, angle64, &h.cx, &h.cy);
  h.halfWidth = width / 2 < 1 ? 1 : width / 2;
  h.halfLength = length / 2 < 1 ? 1 : length / 2;
  h.gap = gap;
  return h;
}

RowSpan rowSpan(const RingGeometry& geom, int32_t py) {
  RowSpan span;

  const int32_t S = SUBPIXEL_SCALE;
  const int32_t centre = (geom.size * S) / 2;
  const int32_t rOuter = centre - geom.inset * S;
  const int32_t rInner = rOuter - geom.thickness * S;
  // The handle stands out of the ring on both sides, so the bounds have to
  // allow for it - the same widening pixelBands() makes before its own two
  // short cuts, or this would throw away the very pixels it overhangs into.
  const int32_t reach = geom.hasHandle ? geom.handle.halfLength : 0;
  const int32_t rReachOuter = rOuter + reach;
  const int32_t rReachInner = (rInner - reach) > 0 ? (rInner - reach) : 0;
  const int32_t rOuter2 = rReachOuter > 0 ? rReachOuter * rReachOuter : 0;
  const int32_t rInner2 = rReachInner * rReachInner;

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
  const int32_t centre = (geom.size * S) / 2;
  // The ring sits inside the object's edge by the room reserved for the
  // handle; with no handle possible that room is zero and the band touches
  // the edge, which is where a ring has always sat.
  const int32_t rOuter = (geom.size * S) / 2 - geom.inset * S;
  const int32_t rInner = rOuter - geom.thickness * S;
  const int32_t rOuter2 = rOuter * rOuter;
  const int32_t rInner2 = rInner > 0 ? rInner * rInner : 0;
  // Where the frame's own pixel ends, when the track is an outline.
  const int32_t rOuterInner2 = (rOuter - FRAME) * (rOuter - FRAME);
  const int32_t rInnerOuter2 = (rInner + FRAME) * (rInner + FRAME);

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
  const int32_t minD2 = minAbsX * minAbsX + minAbsY * minAbsY;
  const int32_t maxD2 = maxAbsX * maxAbsX + maxAbsY * maxAbsY;
  // The handle reaches out of the ring on both sides, so the short cuts have
  // to allow for it - otherwise the very pixels it overhangs into are thrown
  // away before it is ever tested, and the handle comes out as a sliver
  // trapped inside the band.
  const int32_t reach = geom.hasHandle ? geom.handle.halfLength : 0;
  const int32_t rReachOuter = rOuter + reach;
  const int32_t rReachInner = (rInner - reach) > 0 ? (rInner - reach) : 0;
  if (minD2 >= rReachOuter * rReachOuter) {
    if (probeEnabled()) { probe().earlyCalls++;
    probe().earlyCycles += esp_cpu_get_cycle_count() - probeStart; }
    return bands;
  }
  if (maxD2 < rReachInner * rReachInner) {
    if (probeEnabled()) { probe().earlyCalls++;
    probe().earlyCycles += esp_cpu_get_cycle_count() - probeStart; }
    return bands;
  }

  // Third exact short cut: a pixel that nothing's boundary crosses cannot
  // disagree with itself, so one evaluation stands for all sixteen. The
  // conditions are, one for each thing the loop below can test:
  //
  //   - the whole sub-sample box is inside the annulus;
  //   - where the track is an outline, the box also clears both of the
  //     frame's own pixels, so the radius test inside the annulus is uniform
  //     too - without this the band's hollow middle would come back solid
  //     track, which is the one way this short cut could lie on this device;
  //   - the box is entirely outside both end caps, so neither the cap
  //     membership nor the cap rim can vary across it;
  //   - the handle cannot reach the box at all;
  //   - and no sector edge passes through it.
  //
  // The box of sub-sample centres spans offsets 1..2*SUBSAMPLES-1, so its
  // centre is at +SUBSAMPLES and its half-extent is SUBSAMPLES-1.
  const int32_t boxCx = px * S + SUBSAMPLES - centre;
  const int32_t boxCy = py * S + SUBSAMPLES - centre;
  const int32_t boxH = SUBSAMPLES - 1;
  // Asked once per pixel rather than once per sub-sample, and reused below.
  const bool testHandle = handleNear(geom, boxCx, boxCy, boxH);
  {
    const bool capsClear =
        (!geom.hasStartCap ||
         boxMinDist2(boxCx, boxCy, boxH, geom.startCap.cx, geom.startCap.cy) >
             geom.startCap.r * geom.startCap.r) &&
        (!geom.hasEndCap ||
         boxMinDist2(boxCx, boxCy, boxH, geom.endCap.cx, geom.endCap.cy) >
             geom.endCap.r * geom.endCap.r);
    const bool frameClear = !geom.framed || (maxD2 < rOuterInner2 && minD2 > rInnerOuter2);
    if (fastPathEnabled() &&
        maxD2 < rOuter2 && minD2 >= rInner2 &&
        frameClear && capsClear && !testHandle &&
        sectorUniform(geom.fill, boxCx, boxCy, boxH) &&
        sectorUniform(geom.track, boxCx, boxCy, boxH)) {
      // Same priority order as the sampling loop below, and it has to stay
      // that way: a sub-sample is counted into exactly one band. Outside the
      // scale's own angles there is nothing here, the caps having been ruled
      // out already; inside it, the fill, or the track unless the track is an
      // outline and this is the part of the band an outline leaves empty.
      if (inSector(geom.track, boxCx, boxCy)) {
        if (inSector(geom.fill, boxCx, boxCy)) bands.fill = COVERAGE_MAX;
        else if (!geom.framed) bands.track = COVERAGE_MAX;
      }
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

      // The handle first, and before the ring's own radii: it lies ACROSS the
      // band and stands out of it on both sides, which is what says "a thing
      // lying on top" rather than "a slice of the ring". Testing the annulus
      // first clips the handle back into the band - a sliver instead of a
      // handle.
      if (testHandle) {
        const Handle& h = geom.handle;
        const int32_t dx = x - h.cx;
        const int32_t dy = y - h.cy;
        // Floating divides, not integer ones. The designer's are doubles and
        // both results are squared below, so truncating here would move both
        // of the handle's long edges. Exact in double: the dot products are
        // integers far under 2^53 and the divisor is a power of two.
        const double along = ((double)dx * (double)h.tx + (double)dy * (double)h.ty) /
                             (double)ARC_SIN_SCALE;
        const double out = ((double)dx * (double)h.rx + (double)dy * (double)h.ry) /
                           (double)ARC_SIN_SCALE;
        if (out >= -(double)h.halfLength && out <= (double)h.halfLength) {
          const double absAlong = along < 0 ? -along : along;
          const double absOut = out < 0 ? -out : out;
          // A pill: straight sides, and a half-circle at each radial end.
          const double straight = (double)(h.halfLength - h.halfWidth);
          bool inHandle = absAlong <= (double)h.halfWidth;
          if (inHandle && absOut > straight) {
            const double over = absOut - straight;
            inHandle = over * over + along * along <= (double)h.halfWidth * (double)h.halfWidth;
          }
          if (inHandle) {
            bands.handle++;
            continue;
          }
          // The gap: background either side of the handle, cut out of the
          // band rather than drawn over it. That is what separates the two -
          // the gap takes its pixels from the ring, never from the handle.
          if (absAlong <= (double)(h.halfWidth + h.gap)) continue;
        }
      }

      if (d2 >= rOuter2 || d2 < rInner2) continue;

      // The band is every point within half a thickness of its centreline:
      // inside the scale's angles, or inside one of the end discs.
      const bool inStartCap = geom.hasStartCap && inCap(geom.startCap, x, y);
      const bool inEndCap = geom.hasEndCap && inCap(geom.endCap, x, y);
      const bool inside = inSector(geom.track, x, y);
      if (!inside && !inStartCap && !inEndCap) continue;

      // A cap belongs to whichever band reaches that end of the scale. Where
      // a cap overlaps the band proper the sector decides - otherwise the
      // fill's own straight edge would be rounded off by the track's cap.
      const bool filled = inside ? inSector(geom.fill, x, y)
                                 : (inStartCap ? geom.startCapFilled : geom.endCapFilled);

      // Each sub-sample is counted into exactly one band. Rasterizing the
      // bands separately and compositing them looks equivalent and is not:
      // where the fill's radial edge meets the ring's outer edge both have
      // the same partial coverage, and one over the other leaves a seam of
      // track colour that should not be there.
      if (filled) {
        bands.fill++;
        continue;
      }
      if (!geom.framed) {
        bands.track++;
        continue;
      }
      // An outline is the band's own outer pixel, and a pill's outline is two
      // long edges that STOP where the rounded end begins, plus the half of
      // that end which sticks out past it.
      //
      // Drawing both without that division shows: the cap would contribute
      // its whole circle while the two radii ran on past it, so the dial
      // would end in a little ring with two lines sailing by. Inside the
      // scale's angles the band is an ordinary band and its radii are its
      // edges; past them there is only the cap, and only its rim.
      //
      // Where the band was cut - at the fill's edge, at the handle's gap -
      // the frame is left open, so it ends straight there rather than closing
      // itself around nothing. That is the same rule as the bar's
      // levelshape::frameInner, which leaves the run's cut end square.
      const bool onRadius = inside && (d2 >= rOuterInner2 || d2 <= rInnerOuter2);
      const bool onCapRim = !inside && ((inStartCap && capEdge(geom.startCap, x, y)) ||
                                        (inEndCap && capEdge(geom.endCap, x, y)));
      if (onRadius || onCapRim) bands.track++;
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
                    const Rgb565& handle, uint8_t handleCount,
                    const Rgb565& background, uint8_t backgroundCount) {
  const int32_t r = fill.r * fillCount + track.r * trackCount + handle.r * handleCount + background.r * backgroundCount;
  const int32_t g = fill.g * fillCount + track.g * trackCount + handle.g * handleCount + background.g * backgroundCount;
  const int32_t b = fill.b * fillCount + track.b * trackCount + handle.b * handleCount + background.b * backgroundCount;

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
