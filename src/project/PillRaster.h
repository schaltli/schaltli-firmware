#pragma once
#include <Arduino.h>
#include <stdint.h>

#include <vector>

#include "MaterialColors.h"  // DEVICE_COLOR_DEPTH_* - what this panel can show

// The shared rasterizer for pill-shaped runs - the bar's track, fill and
// handle, and the switch's track, knob, buttons and outlines.
//
// A sibling of ArcRaster.h, written to the same rules and for the same reason:
// these shapes are drawn live on every target, so the designer
// (lib/pill-raster.ts), this firmware and the Android app (PillRaster.kt) have
// to arrive at the same pixels, including the soft ones.
// `Adafruit_GFX::fillRoundRect` gave all three the same HARD pixels, which is
// why it served until now; a stair-stepped pill next to an anti-aliased ring
// is what asked the question (the designer's docs/2026-09-22-pill-raster.md).
//
// The rules are the arc's, unchanged:
//
//   - Sub-pixel positions live on a 1/8-pixel grid, so every coordinate is an
//     integer and every comparison is an integer square.
//   - Coverage is a count out of 16, from a 4x4 lattice of sub-samples.
//   - Colours are mixed in 5/6/5 with an explicit rounding rule, by
//     ArcRaster::blendBands' own arithmetic (ColorScreenRenderer::paintPills
//     does the mixing, the way paint-pills.ts does on the designer's side),
//     because the framebuffer is RGB565 and mixing in 8 bits first would land
//     a step or two away.
//
// And the arc's hardest-won rule most of all: **a sub-sample is counted into
// exactly one band.** Where the bar's fill meets its track, compositing one
// shape over the other leaves a seam of track colour along the value's edge -
// fill*c + track*c*(1-c) + bg*(1-c)^2 instead of fill*c + bg*(1-c). Counting
// once has no such seam, and the value's edge is the one place on a bar that
// anybody looks at. That is why pixelBands() takes all the runs at once rather
// than being called once per run.
//
// Every function below has a counterpart of the same name in the TypeScript,
// with the prefix the namespace already carries taken off - exactly as
// ArcRaster::pixelBands is arc-raster.ts's arcPixelBands:
//
//   PillBand        -> PillRaster::Band
//   insidePillBand  -> PillRaster::insideBand
//   pillPixelBands  -> PillRaster::pixelBands
//   pillBandsBounds -> PillRaster::bandsBounds
//
// Keep them that way: a change to one is a change to all three, and the HIL
// pixel diff is what will tell you if you forgot.
namespace PillRaster {

// Sub-pixel grid: 4x4 samples per pixel, positions on a 1/8-pixel lattice.
static const int32_t SUBSAMPLES = 4;
static const int32_t COVERAGE_MAX = SUBSAMPLES * SUBSAMPLES;
static const int32_t SUBPIXEL_SCALE = 8;

// One run with rounded ends, in whole pixels.
//
// The two radii are the run's own ends along its long axis - a segment of a
// connected button group is round on the outside and barely rounded where it
// faces its neighbour, and a bar's filled run is round at the bar's end and
// square where the value cuts it. Each end's radius rounds both of its
// corners, which is what makes the shape a pill rather than a rectangle with
// four independent corners.
struct Band {
  int32_t x = 0;
  int32_t y = 0;
  int32_t w = 0;
  int32_t h = 0;
  // The end at the lower coordinate: left on a horizontal run, top on a
  // vertical one.
  int32_t rLow = 0;
  // The end at the higher coordinate.
  int32_t rHigh = 0;
  // True when the run's long axis is vertical.
  bool vertical = false;
  // Not a run at all but a triangle filling the box: its tip at the middle of
  // the side this names, its base along the opposite side. The radii and
  // `vertical` mean nothing to it. TIP_NONE for every ordinary run.
  //
  // The pointer a bar or a gauge shows its setpoint with. A handle means "you
  // can move this", and neither of them can be moved - the user: "ein
  // anfasser bedeutet verschiebbarkeit" (2026-09-28). A point says the
  // opposite: it cuts into the finger, "das bedeutet ruehr mich nicht an".
  uint8_t tip = 0;
};

static const uint8_t TIP_NONE = 0;
static const uint8_t TIP_UP = 1;
static const uint8_t TIP_DOWN = 2;
static const uint8_t TIP_LEFT = 3;
static const uint8_t TIP_RIGHT = 4;

// Whether a sub-sample, in 1/8 pixel, lies inside a triangle band
// (insidePillTip in the designer's lib/pill-raster.ts).
//
// At depth d from the tip the triangle is w*d/h wide, so a point is inside
// when twice its distance from the centreline, times h, is no more than w*d -
// cross-multiplied, so there is nothing to divide and nothing to round. In 64
// bits, since the product is of two lengths on the 1/8 lattice.
inline bool insideTip(const Band& band, int32_t x, int32_t y) {
  const int32_t S = SUBPIXEL_SCALE;
  const int32_t x0 = band.x * S;
  const int32_t y0 = band.y * S;
  const int32_t x1 = x0 + band.w * S;
  const int32_t y1 = y0 + band.h * S;
  if (x < x0 || x >= x1 || y < y0 || y >= y1) return false;
  const bool across = band.tip == TIP_UP || band.tip == TIP_DOWN;
  const int32_t depth = band.tip == TIP_UP     ? y - y0
                        : band.tip == TIP_DOWN ? y1 - y
                        : band.tip == TIP_LEFT ? x - x0
                                               : x1 - x;
  int32_t off2 = across ? 2 * x - (x0 + x1) : 2 * y - (y0 + y1);
  if (off2 < 0) off2 = -off2;
  const int32_t length = across ? y1 - y0 : x1 - x0;
  const int32_t width = across ? x1 - x0 : y1 - y0;
  return (int64_t)off2 * (int64_t)length <= (int64_t)width * (int64_t)depth;
}

// Whether a triangle band holds a pixel's centre - how a panel without soft
// edges draws one: whole pixels, by the same test the soft path samples with
// (hardPill in the designer's paint-pills.ts).
inline bool tipHoldsPixel(const Band& band, int32_t px, int32_t py) {
  const int32_t S = SUBPIXEL_SCALE;
  return insideTip(band, px * S + S / 2, py * S + S / 2);
}

// C++ integer division truncates toward zero where JS's Math.floor floors, and
// the two disagree for a negative short side - so the rounding direction is
// said out loud rather than assumed, the same way ArcRaster.cpp says it.
inline int32_t floorDiv(int32_t a, int32_t b) {
  int32_t q = a / b;
  if ((a % b != 0) && ((a < 0) != (b < 0))) q--;
  return q;
}

// Math.max(0, Math.min(maxRadius, r)), written out in that order.
//
// NOT a clamp(lo, hi) helper: those assert lo <= hi, and a degenerate run
// (w or h negative) hands this a negative maxRadius, where JS answers 0 and a
// clamp either throws or answers the wrong end. The Kotlin port found this
// one; it is written the same way in all three copies.
inline int32_t clampRadius(int32_t r, int32_t maxRadius) {
  const int32_t capped = r < maxRadius ? r : maxRadius;
  return capped > 0 ? capped : 0;
}

// Whether a sub-sample, in 1/8 pixel, lies inside the band.
//
// The radius is clamped the way fillRoundRect clamps it - to half the short
// side - so an oversized radius self-corrects identically on both sides, and a
// run shorter than it is thick ends in a half-circle rather than a wedge.
//
// The designer's `Math.trunc(band.rLow)` has no counterpart here: its radii
// arrive from JSON and can be fractional, these are already whole.
inline bool insideBand(const Band& band, int32_t x, int32_t y) {
  if (band.tip != TIP_NONE) return insideTip(band, x, y);
  const int32_t S = SUBPIXEL_SCALE;
  const int32_t x0 = band.x * S;
  const int32_t y0 = band.y * S;
  const int32_t x1 = x0 + band.w * S;
  const int32_t y1 = y0 + band.h * S;
  if (x < x0 || x >= x1 || y < y0 || y >= y1) return false;

  const int32_t maxRadius = floorDiv(band.w < band.h ? band.w : band.h, 2);
  const int32_t rLow = clampRadius(band.rLow, maxRadius) * S;
  const int32_t rHigh = clampRadius(band.rHigh, maxRadius) * S;
  if (rLow == 0 && rHigh == 0) return true;

  // Along the run and across it, so one piece of arithmetic serves both
  // directions.
  const int32_t along = band.vertical ? y : x;
  const int32_t across = band.vertical ? x : y;
  const int32_t alongLow = band.vertical ? y0 : x0;
  const int32_t alongHigh = band.vertical ? y1 : x1;
  const int32_t acrossLow = band.vertical ? x0 : y0;
  const int32_t acrossHigh = band.vertical ? x1 : y1;

  auto insideCorner = [&](int32_t cAlong, int32_t cAcross, int32_t r) {
    const int32_t dAlong = along - cAlong;
    const int32_t dAcross = across - cAcross;
    return dAlong * dAlong + dAcross * dAcross <= r * r;
  };

  if (rLow > 0 && along < alongLow + rLow) {
    if (across < acrossLow + rLow) return insideCorner(alongLow + rLow, acrossLow + rLow, rLow);
    if (across >= acrossHigh - rLow) return insideCorner(alongLow + rLow, acrossHigh - rLow, rLow);
  }
  if (rHigh > 0 && along >= alongHigh - rHigh) {
    if (across < acrossLow + rHigh) return insideCorner(alongHigh - rHigh, acrossLow + rHigh, rHigh);
    if (across >= acrossHigh - rHigh) return insideCorner(alongHigh - rHigh, acrossHigh - rHigh, rHigh);
  }
  return true;
}

// How many of a pixel's 16 sub-samples fall in each band.
//
// Bands are tried in order and the first one that contains a sub-sample keeps
// it, so the caller states its own priority by the order it passes them in: a
// handle before the runs it lies on, a cut before what it cuts.
//
// `into` is filled in place rather than returned, so a caller walking a whole
// control hands the same buffer back every pixel. ArcRaster packs its three
// counts into one small struct for the same reason, but a pill has as many
// bands as the control has parts, so there is no fixed width to pack into.
// It must have room for bands.size() entries; no count can exceed 16.
inline void pixelBands(const std::vector<Band>& bands, int32_t px, int32_t py, uint8_t* into) {
  const int32_t S = SUBPIXEL_SCALE;
  const size_t n = bands.size();
  for (size_t b = 0; b < n; b++) into[b] = 0;

  for (int32_t j = 0; j < SUBSAMPLES; j++) {
    // Sub-sample centres sit at (2k+1)/8 of a pixel, i.e. 1/8, 3/8, 5/8, 7/8.
    const int32_t y = py * S + 2 * j + 1;
    for (int32_t i = 0; i < SUBSAMPLES; i++) {
      const int32_t x = px * S + 2 * i + 1;
      for (size_t b = 0; b < n; b++) {
        if (insideBand(bands[b], x, y)) {
          into[b]++;
          break;
        }
      }
    }
  }
}

// Whether a pixel is wholly inside a band (2), wholly outside it (0) or
// neither (1) - decided from whole pixels, without a single sub-sample.
//
// Exact, not an estimate: the band's box and its clamped radii are whole
// pixels, so a pixel is either in the box or out of it, and a pixel outside
// both of the run's rounded end zones - or across the middle, clear of every
// corner - has all sixteen sub-samples inside by insideBand()'s own rules.
// Everything nearer a rounded corner than that answers 1 and is sampled.
inline int pixelCover(const Band& band, int32_t px, int32_t py) {
  if (band.w <= 0 || band.h <= 0) return 0;
  if (px < band.x || px >= band.x + band.w || py < band.y || py >= band.y + band.h) return 0;
  // A triangle's edges run across its whole box, so every pixel of it is
  // sampled. It is a few dozen pixels.
  if (band.tip != TIP_NONE) return 1;
  const int32_t maxRadius = floorDiv(band.w < band.h ? band.w : band.h, 2);
  const int32_t rLow = clampRadius(band.rLow, maxRadius);
  const int32_t rHigh = clampRadius(band.rHigh, maxRadius);
  if (rLow == 0 && rHigh == 0) return 2;
  const int32_t along = band.vertical ? py : px;
  const int32_t alongLow = band.vertical ? band.y : band.x;
  const int32_t alongHigh = alongLow + (band.vertical ? band.h : band.w);
  // Sub-samples sit at 1/8..7/8 of the pixel: all of them are at or past the
  // low end zone when the pixel starts there, and all before the high end
  // zone when the pixel ends before it.
  if (along >= alongLow + rLow && along < alongHigh - rHigh) return 2;
  const int32_t across = band.vertical ? px : py;
  const int32_t acrossLow = band.vertical ? band.x : band.y;
  const int32_t acrossHigh = acrossLow + (band.vertical ? band.w : band.h);
  const int32_t r = rLow > rHigh ? rLow : rHigh;
  if (across >= acrossLow + r && across < acrossHigh - r) return 2;
  // Near a rounded end: decided from the pixel's four outermost sub-samples.
  // Every zone insideBand() tells apart begins and ends on a whole pixel, so
  // all sixteen sub-samples of a pixel fall in the same zone, and in a corner
  // the distance to its centre grows with both coordinates away from it - the
  // nearest and the farthest sub-sample are two of these four. All four in:
  // all sixteen in; all four out: none in. Only a pixel the edge runs through
  // is sampled whole. A group's buttons, round at one end and barely at the
  // other, sent nearly every pixel of both ends to pixelBands(): 36 ms for a
  // 130x43 group on the 4.3B (2026-10-30, ?set=render=1).
  const int32_t S = SUBPIXEL_SCALE;
  const int32_t xs[2] = {px * S + 1, px * S + 2 * SUBSAMPLES - 1};
  const int32_t ys[2] = {py * S + 1, py * S + 2 * SUBSAMPLES - 1};
  int in = 0;
  for (int j = 0; j < 2; j++)
    for (int i = 0; i < 2; i++) in += insideBand(band, xs[i], ys[j]) ? 1 : 0;
  return in == 4 ? 2 : in == 0 ? 0 : 1;
}

// pixelBands(), with the same answer and most of the work left out.
//
// Firmware-only, and a matter of speed, not of result: sampling every pixel of
// a control sixteen times against every run cost the 4.3B's full-width slider
// 290 ms a draw (2026-09-27, /api/debug?set=render=1), while nearly all of its
// pixels lie wholly inside one run or wholly outside all of them. Walked in
// the same priority order as pixelBands(): runs that miss the pixel are
// passed over, the first that holds it whole takes all sixteen, and a run
// that holds only part of it - or holds it after a partial one - sends the
// pixel to pixelBands() as before. tools/pill-raster/ checks the two agree on
// every pixel of thousands of random controls. The designer and the Android
// app have no counterpart and need none: their pixels are the same.
inline void pixelBandsQuick(const std::vector<Band>& bands, int32_t px, int32_t py, uint8_t* into) {
  const size_t n = bands.size();
  for (size_t b = 0; b < n; b++) into[b] = 0;
  for (size_t b = 0; b < n; b++) {
    const int cover = pixelCover(bands[b], px, py);
    if (cover == 0) continue;
    if (cover == 2) {
      into[b] = (uint8_t)COVERAGE_MAX;
      return;
    }
    pixelBands(bands, px, py, into);
    return;
  }
}

// Where along one row of pixels pixelCover() answers 1 or 2 ("touched") and
// where it answers 2 ("full"): two half-open intervals, empty when lo >= hi.
//
// They are intervals because pixelCover() rises and falls only once along a
// row: 0 outside the band's box; inside it, in a rounded end, the four
// outermost sub-samples come nearer the corner's centre with every pixel
// towards the middle, so the answer can only grow there and only shrink at
// the other end; 2 between. A triangle answers 1 across its whole box. So the
// edges are found walking in from both ends of the row on pixelCover()
// itself - as many pixels as the rounded ends are wide, not the whole row -
// and a pixel's answer is then two comparisons: the same answer, not an
// approximation of it. tools/pill-raster/ holds the two to each other.
struct RowSpan {
  int32_t touchLo = 0, touchHi = 0;
  int32_t fullLo = 0, fullHi = 0;
};

inline RowSpan rowSpan(const Band& band, int32_t py) {
  RowSpan s;
  if (band.w <= 0 || band.h <= 0 || py < band.y || py >= band.y + band.h) return s;
  const int32_t x0 = band.x, x1 = band.x + band.w;
  int32_t lo = x0;
  while (lo < x1 && pixelCover(band, lo, py) == 0) lo++;
  if (lo == x1) return s;
  s.touchLo = lo;
  while (lo < x1 && pixelCover(band, lo, py) == 1) lo++;
  int32_t hi = x1 - 1;
  while (hi >= s.touchLo && pixelCover(band, hi, py) == 0) hi--;
  s.touchHi = hi + 1;
  if (lo == x1) return s;  // never whole: no full span
  s.fullLo = lo;
  while (hi >= lo && pixelCover(band, hi, py) == 1) hi--;
  s.fullHi = hi + 1;
  return s;
}

// pixelBandsQuick(), with each band's answer taken from its row's spans.
inline void pixelBandsSpans(const std::vector<Band>& bands, const RowSpan* spans, int32_t px, int32_t py,
                            uint8_t* into) {
  const size_t n = bands.size();
  for (size_t b = 0; b < n; b++) into[b] = 0;
  for (size_t b = 0; b < n; b++) {
    const RowSpan& s = spans[b];
    if (px < s.touchLo || px >= s.touchHi) continue;
    if (px >= s.fullLo && px < s.fullHi) {
      into[b] = (uint8_t)COVERAGE_MAX;
      return;
    }
    pixelBands(bands, px, py, into);
    return;
  }
}

// A box in whole pixels: what bandsBounds() answers.
struct Box {
  int32_t x = 0;
  int32_t y = 0;
  int32_t w = 0;
  int32_t h = 0;
};

// The pixels a set of bands can possibly touch, as a box in whole pixels.
inline Box bandsBounds(const std::vector<Band>& bands) {
  Box out;
  // The designer starts at +/-Infinity and asks Number.isFinite afterwards;
  // int32 has no infinity, so the same question is asked with a flag rather
  // than with a sentinel a real coordinate could reach.
  bool any = false;
  int32_t x0 = 0, y0 = 0, x1 = 0, y1 = 0;
  for (size_t i = 0; i < bands.size(); i++) {
    const Band& band = bands[i];
    if (band.w <= 0 || band.h <= 0) continue;
    if (!any) {
      x0 = band.x;
      y0 = band.y;
      x1 = band.x + band.w;
      y1 = band.y + band.h;
      any = true;
      continue;
    }
    if (band.x < x0) x0 = band.x;
    if (band.y < y0) y0 = band.y;
    if (band.x + band.w > x1) x1 = band.x + band.w;
    if (band.y + band.h > y1) y1 = band.y + band.h;
  }
  if (!any) return out;
  out.x = x0;
  out.y = y0;
  out.w = x1 - x0;
  out.h = y1 - y0;
  return out;
}

}  // namespace PillRaster

// **Nothing is softened below 24-bit colour depth.**
//
// On a 1-bit panel there is nothing to mix into: every soft pixel snaps to
// black or white on the way to the glass, and the panel draws these shapes
// with whole pixels anyway. A 4-bit panel could take a soft edge in its
// sixteen greys, but only once the designer's own copy quantises the blend the
// same way - a decision that is still open, and one about both sides at once
// (docs/2026-09-22-pill-raster.md, "Wo nicht geglaettet wird").
//
// So this is a decision, not an omission. Where it is 0, the callers keep
// their existing whole-pixel path line for line: the same fillRoundRect, the
// same squared-off ends, the same picture down to the byte.
//
// The macros are the board's own, set in platformio.ini and defaulted to 0 by
// MaterialColors.h - the same pair that already decides which colours a
// derived one is allowed to be.
#if DEVICE_COLOR_DEPTH_1BIT || DEVICE_COLOR_DEPTH_4BIT
#define PILL_ANTIALIASED 0
#else
#define PILL_ANTIALIASED 1
#endif
