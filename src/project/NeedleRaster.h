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
//
// The designer's lib/needle-raster/vectors.json is the reference
// (test/test_needle).

#include <stdint.h>

#include <vector>

#include "PolygonRaster.h"

namespace needleraster {

enum class Motion : uint8_t { Rotate, Translate, Static };

struct Shape {
  bool circle = false;
  std::vector<PolygonRaster::Point> poly;  // eighths
  int32_t cx = 0, cy = 0, r = 0, rIn = 0;  // eighths
};

struct Layer {
  Motion motion = Motion::Static;
  std::vector<Shape> shapes;
  uint16_t color = 0;  // RGB565
  int32_t alpha = 256;
  std::vector<int32_t> soft;  // empty: a body
  int32_t dx = 0, dy = 0;     // eighths, after turning
};

struct Description {
  int32_t pivotX = 0, pivotY = 0;  // eighths
  std::vector<Layer> layers;
};

struct Pose {
  int32_t angle64 = 0;  // 1/64 degree
  int32_t shiftX = 0;   // eighths
  int32_t shiftY = 0;
};

// An RGB565 picture: `width` x `height` pixels at `pixels`, `stride` pixels
// to a row. Pixels outside are never touched.
struct Target {
  uint16_t* pixels = nullptr;
  int32_t stride = 0;
  int32_t width = 0;
  int32_t height = 0;
};

// Draws every layer of `desc` in `pose` into `t`, back to front.
void draw(Target& t, const Description& desc, const Pose& pose);

// The angle a needle stands at for `degrees`: floor(degrees * 64 + 0.5).
int32_t angle64Of(double degrees);

// floor(sqrt(n)) for n >= 0, without floating point.
int64_t isqrt(int64_t n);

}  // namespace needleraster
