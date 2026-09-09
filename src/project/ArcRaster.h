#pragma once
#include <Arduino.h>
#include <stdint.h>

// The arc-level rasterizer, ported from the designer's lib/arc-raster.ts.
//
// This file and that one are the same algorithm written twice, deliberately,
// and they have to produce identical pixels: the HIL comparison is exact,
// with no tolerance, and that is what has found every real rendering bug in
// this project so far. Read the TypeScript's header comment for why an
// anti-aliased ring forced the question at all - the short version is that a
// hard-edged arc at radius 168 reads as cheap, anti-aliasing means
// intermediate colours, and intermediate colours are what a zero-tolerance
// comparison cannot afford to have drift.
//
// So nothing here is left to a maths library:
//
//   - Angles are whole 1/64 degrees, zero at twelve o'clock, clockwise
//     positive. Direction vectors come from ArcSinTable.h - generated into
//     both repos from one script - never from sinf()/cosf(), whose last
//     digit would differ between newlib here and V8 there, and whose last
//     digit is exactly what decides a sub-pixel along a shallow edge.
//   - Sub-pixels sit on a 1/8-pixel lattice, so every coordinate is an
//     integer and every radius comparison is an integer square.
//   - Sector membership is the sign of a cross product. No atan2.
//   - The division by 64 is written as `>> 6` because C++ integer division
//     truncates toward zero for negatives where JS floors, and an arithmetic
//     shift is the operation both languages agree on.
//
// Every function below has a counterpart of the same name in the TypeScript.
// Keep them that way: a change to one is a change to both, and the pixel
// diff is what will tell you if you forgot.
namespace ArcRaster {

// Angles are carried in 1/64 degree units.
static const int32_t ANGLE_SCALE = 64;
static const int32_t FULL_TURN = 360 * ANGLE_SCALE;

// Sub-pixel grid: 4x4 samples per pixel, positions on a 1/8-pixel lattice.
static const int32_t SUBSAMPLES = 4;
static const int32_t COVERAGE_MAX = SUBSAMPLES * SUBSAMPLES;
static const int32_t SUBPIXEL_SCALE = 8;

// A sector's two boundary rays, prepared once per band rather than per
// sub-pixel. `wide` selects the union test: a sector wider than a half turn
// is not the intersection of two half-planes but the union of them.
struct Sector {
  int32_t ux = 0;
  int32_t uy = 0;
  int32_t vx = 0;
  int32_t vy = 0;
  bool wide = false;
  bool full = false;
  bool empty = true;
};

// How many of a pixel's 16 sub-samples fall in each band.
struct PixelBands {
  uint8_t fill = 0;
  uint8_t track = 0;
  uint8_t marker = 0;
};

struct RingGeometry {
  int32_t size = 0;       // side of the (square) object, in pixels
  int32_t thickness = 0;  // ring thickness, inwards from the object's edge
  Sector track;
  Sector fill;
  Sector marker;
};

// A colour reduced to the framebuffer's own depth, as separate channels.
struct Rgb565 {
  uint8_t r = 0;
  uint8_t g = 0;
  uint8_t b = 0;
};

// Cost probe. Diagnostic only: two counters and two cycle totals, so the
// two paths through pixelBands() can be told apart from outside. Left in
// because the question "which path is slow" was asked three times in one
// afternoon and answered wrongly twice.
struct Probe {
  uint32_t earlyCalls = 0;
  uint32_t earlyCycles = 0;
  uint32_t sampledCalls = 0;
  uint32_t sampledCycles = 0;
  uint32_t uniformCalls = 0;
  uint32_t uniformCycles = 0;
};
Probe& probe();

// Off in normal operation: the probe costs two cycle-counter reads on
// every call, and pixelBands() is called 129,600 times for one ring.
bool& probeEnabled();

// Lets a test force the exhaustive sampling path, so the exact short cut can
// be compared against the thing it replaces rather than trusted.
bool& fastPathEnabled();
void probeReset();

void direction(int32_t angle64, int32_t* outX, int32_t* outY);
Sector makeSector(int32_t start64, int32_t sweep64);
bool inSector(const Sector& s, int32_t x, int32_t y);
PixelBands pixelBands(const RingGeometry& geom, int32_t px, int32_t py);

Rgb565 toRgb565(uint16_t packed);
uint16_t packRgb565(const Rgb565& c);

// Mixes up to three bands plus the background by coverage, in 5/6/5 space.
// The counts must total COVERAGE_MAX.
uint16_t blendBands(const Rgb565& fill, uint8_t fillCount,
                    const Rgb565& track, uint8_t trackCount,
                    const Rgb565& marker, uint8_t markerCount,
                    const Rgb565& background, uint8_t backgroundCount);

// Where along a sector a percentage sits, truncated - mirroring the
// designer's Math.trunc, which in turn mirrors C's own assignment to an int.
int32_t sweepForPercent(int32_t sweep64, float percent);

}  // namespace ArcRaster
