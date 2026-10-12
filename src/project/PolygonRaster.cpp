#include "PolygonRaster.h"

#include <algorithm>
#include <cmath>

namespace PolygonRaster {

int32_t toEighths(float px) {
  const float e = px * 8.0f;
  return (int32_t)(e < 0 ? -std::floor(-e + 0.5f) : std::floor(e + 0.5f));
}

// Floor and ceiling of a / b for b > 0, in whole numbers. They were
// std::floor(a / 4.0) and the like, in double - which this chip does in
// software: once for every sub-sample, 16 times a pixel, they cost an energy
// diagram 790 ms (2026-10-12). Same results, a hundredth of the time.
static inline int32_t floorDiv(int32_t a, int32_t b) { return a >= 0 ? a / b : -((-a + b - 1) / b); }
static inline int32_t ceilDiv(int32_t a, int32_t b) { return a >= 0 ? (a + b - 1) / b : -((-a) / b); }

// An edge of the outline, from its upper end (inclusive) to its lower end
// (exclusive), so a shared corner counts once. Horizontal edges cross no row.
struct Edge {
  int32_t topY, bottomY, topX, bottomX;
};

// Where an edge crosses the row at height y (eighths), as x in eighths:
// top.x + (y - top.y) * dx / dy, rounded to the nearest eighth.
static inline int32_t crossing(const Edge& e, int32_t y) {
  const int64_t num = (int64_t)(y - e.topY) * (e.bottomX - e.topX);
  const int64_t den = e.bottomY - e.topY;
  const int64_t q = num >= 0 ? (2 * num + den) / (2 * den) : -((-2 * num + den) / (2 * den));
  return e.topX + (int32_t)q;
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
  const int32_t px0 = floorDiv(minX, 8);
  const int32_t px1 = floorDiv(maxX, 8);
  int32_t py0 = floorDiv(minY, 8);
  int32_t py1 = floorDiv(maxY, 8);
  py0 = std::max<int32_t>(py0, clipY0);
  py1 = std::min<int32_t>(py1, clipY1);
  if (px1 < px0 || py1 < py0) return;

  const int32_t width = px1 - px0 + 1;
  std::vector<uint8_t> cover(width);
  std::vector<int32_t> xs;
  // The edges by their upper end, and those a row crosses: rows come in
  // order, so each edge joins once and leaves once - every edge tried on
  // every row cost a ring of 300 corners 100 000 tries (2026-10-12).
  std::vector<Edge> edges;
  edges.reserve(points.size());
  for (size_t i = 0; i < points.size(); i++) {
    const Point& a = points[i];
    const Point& b = points[(i + 1) % points.size()];
    if (a.y == b.y) continue;
    const Point& top = a.y < b.y ? a : b;
    const Point& bottom = a.y < b.y ? b : a;
    edges.push_back({top.y, bottom.y, top.x, bottom.x});
  }
  std::sort(edges.begin(), edges.end(), [](const Edge& p, const Edge& q) { return p.topY < q.topY; });
  std::vector<const Edge*> active;
  size_t next = 0;
  for (int32_t py = py0; py <= py1; py++) {
    std::fill(cover.begin(), cover.end(), 0);
    bool any = false;
    for (int sub = 0; sub < 4; sub++) {
      const int32_t sy = py * 8 + 1 + sub * 2;
      while (next < edges.size() && edges[next].topY <= sy) active.push_back(&edges[next++]);
      xs.clear();
      for (size_t k = 0; k < active.size();) {
        if (active[k]->bottomY <= sy) {
          active[k] = active.back();
          active.pop_back();
          continue;
        }
        xs.push_back(crossing(*active[k], sy));
        k++;
      }
      std::sort(xs.begin(), xs.end());
      for (size_t k = 0; k + 1 < xs.size(); k += 2) {
        // Sub-sample columns at px*8 + 1, 3, 5, 7 inside [xs[k], xs[k+1]).
        const int32_t from = xs[k], to = xs[k + 1];
        if (to <= from) continue;
        // The first sub-sample column >= from, and the last < to.
        int32_t c0 = ceilDiv(from - 1, 2);  // index of column 2c+1 >= from
        int32_t c1 = ceilDiv(to - 1, 2) - 1;
        if (c1 < c0) continue;
        // Counted a pixel at a time, not a sub-sample at a time: the first
        // and last pixel get what of their four columns lies inside, the
        // ones between all four (2026-10-12: 550 000 single counts a
        // diagram).
        const int32_t p0 = floorDiv(c0, 4), p1 = floorDiv(c1, 4);
        auto add = [&](int32_t pixel, int32_t n) {
          if (pixel < px0 || pixel > px1 || n <= 0) return;
          cover[pixel - px0] += (uint8_t)n;
          any = true;
        };
        if (p0 == p1) {
          add(p0, c1 - c0 + 1);
        } else {
          add(p0, 4 - (c0 - 4 * p0));
          for (int32_t pixel = std::max(p0 + 1, px0); pixel <= std::min(p1 - 1, px1); pixel++) {
            cover[pixel - px0] += 4;
            any = true;
          }
          add(p1, c1 - 4 * p1 + 1);
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
