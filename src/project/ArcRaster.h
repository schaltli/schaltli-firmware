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
// The shape changed on 2026-09-22 (the designer's docs/2026-09-22-arc-look.md),
// and this file changed with it: the band gained rounded pill ends, the ring
// moved inwards to make room for a handle, the white setpoint wedge became
// the slider's own handle lying ACROSS the band, and the unfilled track
// stopped being a body of its own.
//
// The `framed` branch - the track drawn as the band's own one-pixel OUTLINE
// in the fill colour, left open where the band was cut - belongs to a panel
// that cannot show the halfway tint the track would otherwise be. None of
// this repo's three boards is 1-bit, so the two 24-bit Waveshare panels never
// take it; the 4-bit PaperS3 takes it exactly when its sixteen greys quantise
// that tint back onto the screen's own colour. LevelShape.h's trackLook()
// answers the question once, for the ring and for the bar alike. The one
// device that takes the branch on every screen is schaltli-eink - which
// is why this repo's copy is held to the same recorded pixels rather than
// only to a panel it would never show up on (tools/arc-raster/run.js).
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

// A rounded end of the band: a disc on the centreline at the end angle.
//
// Which is the whole definition of a pill, read literally - the band is every
// point within half a thickness of its centreline arc, and past the last
// angle that is a disc. The cost is that the ends reach half a thickness past
// minAngle and maxAngle, exactly as a bar's pill reaches past its own run; a
// scale whose gap is narrower than a whole thickness therefore closes up, and
// a ring that goes all the way round has no caps at all.
//
// All lengths are in 1/8 pixel from the object's centre, like every other
// coordinate here; the tangent is a direction vector from the sine table,
// scaled by ARC_SIN_SCALE - kept because it says which way the band was
// heading when it stopped.
struct Cap {
  int32_t cx = 0;
  int32_t cy = 0;
  int32_t tx = 0;  // outward along the band at this end
  int32_t ty = 0;
  int32_t r = 0;  // half the band's thickness
};

// The two rounded ends of a scale - absent where it goes all the way round.
struct Caps {
  bool has = false;
  Cap startCap;
  Cap endCap;
};

// The setpoint handle: the bar's own, bent onto a ring.
//
// Same proportions as the slider's (LevelShape.h's handleLength/handleWidth/
// handleGap): as long as eleven quarters of the thickness, an eleventh of
// that wide, with a gap of three twenty-seconds each side. It lies across the
// band on a straight line rather than following the curve, which is what a
// handle 60 units long on a ring 22 thick looks like anyway - and what the
// bar does.
struct Handle {
  int32_t cx = 0;  // centre, on the centreline at the setpoint's angle
  int32_t cy = 0;
  int32_t tx = 0;  // along the band (the handle's width runs this way)
  int32_t ty = 0;
  int32_t rx = 0;  // outwards from the centre (the length runs this way)
  int32_t ry = 0;
  int32_t halfWidth = 0;
  int32_t halfLength = 0;
  int32_t gap = 0;  // cut out of the band each side, on top of halfWidth
};

// How many of a pixel's 16 sub-samples fall in each band.
struct PixelBands {
  uint8_t fill = 0;
  uint8_t track = 0;
  // Called `marker` until 2026-09-22, when it stopped being a wedge of the
  // ring and became the slider's own handle: a pill lying across the band,
  // standing out of it on both sides, with a gap cut either side of it.
  uint8_t handle = 0;
  // The setpoint pointer of a gauge, which has no handle (Pointer).
  uint8_t pointer = 0;
};

// A gauge's setpoint pointer: a triangle outside the ring, its tip towards the
// centre. A gauge cannot be moved, so it has no handle (2026-09-28). The bar's
// pointer bent onto a ring, as the handle is the bar's handle bent onto one.
// Tested in the ray's own frame, both products still scaled by ARC_SIN_SCALE
// and never divided - they outgrow 32 bits on a large ring, so int64.
// ArcPointer and inArcPointer in the designer's lib/arc-raster.ts.
struct Pointer {
  int32_t rx = 0;  // outwards from the centre at the setpoint's angle
  int32_t ry = 0;
  int32_t tx = 0;  // along the band
  int32_t ty = 0;
  int32_t tip = 0;     // from the centre to the tip, in 1/8 pixel
  int32_t length = 0;  // from the tip to the base, in 1/8 pixel
  int32_t half = 0;    // half the base, in 1/8 pixel
};

struct RingGeometry {
  int32_t size = 0;       // side of the (square) object, in pixels
  int32_t thickness = 0;  // ring thickness, inwards from the band's outer edge
  // How far the ring sits inside the object's own edge, in pixels.
  //
  // Room for the handle, which lies across the band and stands out of it on
  // both sides: with the ring touching the object's edge the outer half of
  // the handle would fall outside the object - and an object that draws past
  // its own rectangle is clipped by the designer's buffer, by the app's box
  // and by this renderer's object rect alike.
  //
  // Reserved whenever the object CAN have a handle rather than while one is
  // being drawn, so the ring does not jump inwards the moment a setpoint
  // arrives. The bar reserves its handle's room the same way: its slot is as
  // tall as the handle, whatever the track's own thickness.
  int32_t inset = 0;
  Sector track;
  Sector fill;
  bool hasStartCap = false;
  bool hasEndCap = false;
  Cap startCap;
  Cap endCap;
  // Whether each cap belongs to the fill rather than to the track.
  bool startCapFilled = false;
  bool endCapFilled = false;
  bool hasHandle = false;
  Handle handle;
  bool hasPointer = false;
  Pointer pointer;
  // The track is drawn as its own outline, one pixel wide, instead of as a
  // body - for a panel that cannot show the mixed colour the track would
  // otherwise be, which on this device is every panel. LevelShape.h answers
  // the same question the same way for the bar.
  bool framed = false;
};

// Which pixels of one row can be covered at all, so a caller can skip the
// rest instead of asking about them one at a time. On a 360px ring, 105,236
// of the 129,600 pixels in the object's square lie outside the ring or in
// the hole, and visiting them cost a third of the arc's render time to be
// told nothing is there.
//
// Exact, and exactly the same two tests pixelBands() makes before it does
// anything else - the box of a pixel's sub-samples against the outer radius,
// and against the inner one, both widened by the handle's reach - only solved
// for x instead of evaluated. So a pixel this excludes is a pixel
// pixelBands() would have returned empty for, which is what ArcSelfTest checks
// on the glass and tools/arc-raster/selftest.cpp checks on a laptop, over
// every pixel of every geometry.
//
// This one has no TypeScript counterpart, deliberately, and it is the only
// thing in this file that does not. It decides which pixels the loop walks,
// never what a pixel becomes, so it cannot move a single sub-pixel of the
// output; and the designer draws one screen on a laptop, where the walk is
// free. Anything that touches coverage still belongs in both files.
struct RowSpan {
  int32_t x0 = 0;       // first pixel that can be covered, inclusive
  int32_t x1 = -1;      // last, inclusive - x1 < x0 means the row is empty
  int32_t holeX0 = 0;   // first pixel of the empty middle, inclusive
  int32_t holeX1 = -1;  // last - holeX1 < holeX0 means there is no hole
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
RowSpan rowSpan(const RingGeometry& geom, int32_t py);

// The ring's centreline radius, in 1/8 pixel - where caps and handle sit.
int32_t midRadius(int32_t size, int32_t thickness, int32_t inset);
// A point on the centreline, in 1/8 pixel from the object's centre.
void pointAt(int32_t size, int32_t thickness, int32_t inset, int32_t angle64, int32_t* outX,
             int32_t* outY);
Caps caps(int32_t size, int32_t thickness, int32_t inset, int32_t start64, int32_t sweep64);
Handle handleBand(int32_t size, int32_t thickness, int32_t inset, int32_t angle64, int32_t sweep64);
bool inPointer(const Pointer& p, int32_t x, int32_t y);
// A gauge's pointer at angle64: outside the ring, its tip two pixels off the
// band's outer edge, in the room the inset keeps. Sized by the bar's rule -
// two thirds of the thickness, a base as wide, shrunk into the room there is
// (levelshape::pointerSize, which this repeats because this file must build
// without Arduino.h for tools/arc-raster). False where there is no room.
// arcPointerBand in the designer's render-arc-level.ts.
bool pointerBand(int32_t size, int32_t thickness, int32_t inset, int32_t angle64, Pointer* out);

Rgb565 toRgb565(uint16_t packed);
uint16_t packRgb565(const Rgb565& c);

// Mixes up to three bands plus the background by coverage, in 5/6/5 space.
// The counts must total COVERAGE_MAX.
uint16_t blendBands(const Rgb565& fill, uint8_t fillCount,
                    const Rgb565& track, uint8_t trackCount,
                    const Rgb565& handle, uint8_t handleCount,
                    const Rgb565& background, uint8_t backgroundCount);
// The same, with a gauge's pointer as a fourth band.
uint16_t blendBands(const Rgb565& fill, uint8_t fillCount,
                    const Rgb565& track, uint8_t trackCount,
                    const Rgb565& handle, uint8_t handleCount,
                    const Rgb565& pointer, uint8_t pointerCount,
                    const Rgb565& background, uint8_t backgroundCount);

// Where along a sector a percentage sits, truncated - mirroring the
// designer's Math.trunc, which in turn mirrors C's own assignment to an int.
int32_t sweepForPercent(int32_t sweep64, float percent);

}  // namespace ArcRaster
