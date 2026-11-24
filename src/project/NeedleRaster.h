#pragma once
// The moving part of a styled gauge or bar - designer lib/needle-raster.ts,
// integer for integer (device contract §2.15, docs/2026-11-23-instrument-
// styles.md). A needle description is drawn into an RGB565 picture:
//
// - Coordinates are eighths of a pixel, in the object's own box.
// - A rotating layer turns about the pivot by the shared sine table, in
//   1/64 degree, 0 = up, clockwise: x' = px + ((rx*C - ry*S + 8192) >> 14).
// - A body is filled with 4 x 4 sub-samples per pixel (PolygonRaster for
//   polygons, the same sampling for circles and rings), alpha x coverage / 16.
// - A soft layer (glow, shadow) is a field: soft[0] inside, soft[k] up to k
//   whole pixels outside, compared as squared distances; nothing beyond.
// - Each pixel is mixed into what is already there (levelglow::blend565's
//   formula, restated here so this builds on the PC without Arduino).
// - A fill layer grows with the value instead of moving: a rect from the
//   edge `grow` names, an arc (a band of half width hw about radius r round
//   the pivot, round ends) from its `from` angle to the pose's angle.
// - A layer with a gradient takes its colour per pixel (gradient565()).
// - A segments layer holds still; a shape with a threshold `t` (out of
//   4096) is drawn only while the pose's level has reached it (a VFD's bars).
//
// The designer's lib/needle-raster/vectors.json is the reference
// (test/test_needle).

#include <stdint.h>

#include <vector>

#include "PolygonRaster.h"

namespace needleraster {

enum class Motion : uint8_t { Rotate, Translate, Static, Fill, Segments };

enum class Kind : uint8_t { Poly, Circle, Rect, Arc };

// The way a fill layer's rects grow from their start edge.
enum class Grow : uint8_t { Right, Left, Down, Up };

struct Shape {
  Kind kind = Kind::Poly;
  std::vector<PolygonRaster::Point> poly;  // eighths
  int32_t cx = 0, cy = 0, r = 0, rIn = 0;  // eighths; an arc's radius is r
  int32_t x0 = 0, y0 = 0, x1 = 0, y1 = 0;  // a rect, eighths (a fill layer's at full scale)
  int32_t hw = 0, from64 = 0, to64 = 0;    // an arc: half width (eighths), angles (1/64 degree)
  int32_t t = -1;                          // a segments layer: the level (out of 4096) it lights from; -1 none
};

// Colour per pixel: gradient565(color, to, (x or y) - start, length).
struct Gradient {
  bool on = false;
  uint16_t to = 0;  // RGB565
  bool alongY = false;
  int32_t start = 0;   // pixels
  int32_t length = 1;  // pixels, at least 1
};

struct Layer {
  Motion motion = Motion::Static;
  std::vector<Shape> shapes;
  uint16_t color = 0;  // RGB565
  int32_t alpha = 256;
  std::vector<int32_t> soft;  // empty: a body
  int32_t dx = 0, dy = 0;     // eighths, after turning
  Grow grow = Grow::Right;    // a fill layer's rects
  Gradient gradient;
};

struct Description {
  int32_t pivotX = 0, pivotY = 0;  // eighths
  std::vector<Layer> layers;
};

struct Pose {
  int32_t angle64 = 0;  // 1/64 degree
  int32_t shiftX = 0;   // eighths
  int32_t shiftY = 0;
  int32_t level = 0;    // the position out of 4096 (segments layers)
};

// An RGB565 picture: `width` x `height` pixels at `pixels`, `stride` pixels
// to a row. Pixels outside are never touched, nor those outside the clip
// (x0, y0 inclusive, x1, y1 exclusive, in the same coordinates) - a region
// redrawn on its own. A clip left at -1 is the whole picture.
struct Target {
  uint16_t* pixels = nullptr;
  int32_t stride = 0;
  int32_t width = 0;
  int32_t height = 0;
  int32_t clipX0 = -1, clipY0 = -1, clipX1 = -1, clipY1 = -1;
};

// Draws every layer of `desc` in `pose` into `t`, back to front.
void draw(Target& t, const Description& desc, const Pose& pose);

// The pixels `desc` in `pose` can touch, glow and shadow included:
// x0..x1, y0..y1 inclusive, in the object's own coordinates. False when it
// has no shapes (or only empty fills). What a board redraws while a needle
// swings.
bool bounds(const Description& desc, const Pose& pose, int32_t* x0, int32_t* y0, int32_t* x1, int32_t* y1);

// A position 0..1 as a pose's level: floor(clamp(at, 0, 1) * 4096 + 0.5).
int32_t levelOf(double at);

// `color` over `dst` at alpha/256, rounded per 5/6/5 channel - levelglow::
// blend565 on packed pixels (256 or more: `color` itself, 0 or less: `dst`).
inline uint16_t blend565(uint16_t dst, uint16_t color, int32_t alpha) {
  if (alpha <= 0) return dst;
  if (alpha >= 256) return color;
  const int32_t dr = (dst >> 11) & 31, dg = (dst >> 5) & 63, db = dst & 31;
  const int32_t cr = (color >> 11) & 31, cg = (color >> 5) & 63, cb = color & 31;
  const int32_t r = (dr * (256 - alpha) + cr * alpha + 128) >> 8;
  const int32_t g = (dg * (256 - alpha) + cg * alpha + 128) >> 8;
  const int32_t b = (db * (256 - alpha) + cb * alpha + 128) >> 8;
  return (uint16_t)((r << 11) | (g << 5) | b);
}

// The angle a needle stands at for `degrees`: floor(degrees * 64 + 0.5).
int32_t angle64Of(double degrees);

// floor(sqrt(n)) for n >= 0, without floating point.
int64_t isqrt(int64_t n);

}  // namespace needleraster
