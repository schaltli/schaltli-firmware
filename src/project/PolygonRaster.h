#pragma once
#include <stdint.h>

#include <functional>
#include <vector>

// A filled polygon, rasterized to the same rules as ArcRaster and PillRaster
// - for the energy diagram's Sankey bands (designer
// docs/2026-10-11-energy-diagram.md), which the designer's core hands over
// as polygons so the canvas, this firmware and the Android app fill the same
// shape (tasks/energy-diagram-plan.md, S2).
//
//   - Corners live on a 1/8-pixel grid: integers, 8 to a pixel.
//   - Coverage is a count out of 16, from a 4x4 lattice of sub-samples at
//     1/8, 3/8, 5/8 and 7/8 of a pixel.
//   - Even-odd: a sub-sample is inside when a ray to its left crosses the
//     outline an odd number of times. A band never crosses itself, so this
//     is also nonzero for it.
//
// Rows come out as spans: a run of whole pixels as one call, a pixel on an
// edge with its coverage. Whole runs are what a band mostly is, and drawing
// them as horizontal lines is what keeps a band cheap.
namespace PolygonRaster {

struct Point {
  int32_t x;  // eighths of a pixel
  int32_t y;
};

// One row of the polygon: fullRun(y, x0, x1) for pixels x0..x1 inclusive
// fully inside, partial(y, x, coverage) for a pixel 1..15 sixteenths in.
struct Sink {
  std::function<void(int16_t y, int16_t x0, int16_t x1)> fullRun;
  std::function<void(int16_t y, int16_t x, uint8_t coverage)> partial;
};

// Fills `points` (a closed outline; the last joins the first) between pixel
// rows clipY0 and clipY1 inclusive.
void fill(const std::vector<Point>& points, int16_t clipY0, int16_t clipY1, const Sink& sink);

// Pixels to eighths, rounded half away from zero, as the designer does.
int32_t toEighths(float px);

}  // namespace PolygonRaster
