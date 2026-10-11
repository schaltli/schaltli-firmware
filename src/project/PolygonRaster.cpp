#include "PolygonRaster.h"

#include <algorithm>
#include <cmath>

namespace PolygonRaster {

int32_t toEighths(float px) {
  const float e = px * 8.0f;
  return (int32_t)(e < 0 ? -std::floor(-e + 0.5f) : std::floor(e + 0.5f));
}

// Where the outline crosses the horizontal line at height y (eighths), as x
// in eighths, for every edge that spans it - an edge counts from its upper
// end inclusive to its lower end exclusive, so a shared corner counts once.
static void crossings(const std::vector<Point>& pts, int32_t y, std::vector<int32_t>& out) {
  out.clear();
  const size_t n = pts.size();
  for (size_t i = 0; i < n; i++) {
    const Point& a = pts[i];
    const Point& b = pts[(i + 1) % n];
    if (a.y == b.y) continue;
    const Point& top = a.y < b.y ? a : b;
    const Point& bottom = a.y < b.y ? b : a;
    if (y < top.y || y >= bottom.y) continue;
    // x = top.x + (y - top.y) * dx / dy, rounded to the nearest eighth.
    const int64_t num = (int64_t)(y - top.y) * (bottom.x - top.x);
    const int64_t den = bottom.y - top.y;
    const int64_t q = num >= 0 ? (2 * num + den) / (2 * den) : -((-2 * num + den) / (2 * den));
    out.push_back(top.x + (int32_t)q);
  }
  std::sort(out.begin(), out.end());
}

void fill(const std::vector<Point>& points, int16_t clipY0, int16_t clipY1, const Sink& sink) {
  if (points.size() < 3) return;
  int32_t minX = points[0].x, maxX = points[0].x, minY = points[0].y, maxY = points[0].y;
  for (const Point& p : points) {
    minX = std::min(minX, p.x);
    maxX = std::max(maxX, p.x);
    minY = std::min(minY, p.y);
    maxY = std::max(maxY, p.y);
  }
  // Pixels whose sub-samples can be inside.
  const int32_t px0 = (int32_t)std::floor(minX / 8.0);
  const int32_t px1 = (int32_t)std::floor(maxX / 8.0);
  int32_t py0 = (int32_t)std::floor(minY / 8.0);
  int32_t py1 = (int32_t)std::floor(maxY / 8.0);
  py0 = std::max<int32_t>(py0, clipY0);
  py1 = std::min<int32_t>(py1, clipY1);
  if (px1 < px0 || py1 < py0) return;

  const int32_t width = px1 - px0 + 1;
  std::vector<uint8_t> cover(width);
  std::vector<int32_t> xs;
  for (int32_t py = py0; py <= py1; py++) {
    std::fill(cover.begin(), cover.end(), 0);
    bool any = false;
    for (int sub = 0; sub < 4; sub++) {
      const int32_t sy = py * 8 + 1 + sub * 2;
      crossings(points, sy, xs);
      for (size_t k = 0; k + 1 < xs.size(); k += 2) {
        // Sub-sample columns at px*8 + 1, 3, 5, 7 inside [xs[k], xs[k+1]).
        const int32_t from = xs[k], to = xs[k + 1];
        if (to <= from) continue;
        // The first sub-sample column >= from, and the last < to.
        int32_t c0 = (int32_t)std::ceil((from - 1) / 2.0);  // index of column 2c+1 >= from
        int32_t c1 = (int32_t)std::ceil((to - 1) / 2.0) - 1;
        for (int32_t c = c0; c <= c1; c++) {
          const int32_t pixel = (int32_t)std::floor(c / 4.0);
          if (pixel < px0 || pixel > px1) continue;
          cover[pixel - px0]++;
          any = true;
        }
      }
    }
    if (!any) continue;
    int32_t runStart = -1;
    for (int32_t i = 0; i <= width; i++) {
      const uint8_t c = i < width ? cover[i] : 0;
      if (c == 16) {
        if (runStart < 0) runStart = i;
        continue;
      }
      if (runStart >= 0) {
        if (sink.fullRun) sink.fullRun((int16_t)py, (int16_t)(px0 + runStart), (int16_t)(px0 + i - 1));
        runStart = -1;
      }
      if (i < width && c > 0 && sink.partial) sink.partial((int16_t)py, (int16_t)(px0 + i), c);
    }
  }
}

}  // namespace PolygonRaster
