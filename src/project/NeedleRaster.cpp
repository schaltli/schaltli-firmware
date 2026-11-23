#include "NeedleRaster.h"

#include <math.h>

#include "ArcSinTable.h"

namespace needleraster {

namespace {

const int32_t ANGLE_SCALE = 64;
const int32_t FULL_TURN = 360 * ANGLE_SCALE;

// ArcRaster::direction(), restated: (sin, cos) x 16384 for an angle in 1/64
// degree, interpolated between whole degrees.
void sinCos(int32_t angle64, int32_t* s, int32_t* c) {
  int32_t a = angle64 % FULL_TURN;
  if (a < 0) a += FULL_TURN;
  const int32_t deg = a / ANGLE_SCALE;
  const int32_t frac = a - deg * ANGLE_SCALE;
  const int32_t sin0 = ARC_SIN_TABLE[deg];
  const int32_t sin1 = ARC_SIN_TABLE[(deg + 1) % 360];
  const int32_t cos0 = ARC_SIN_TABLE[(deg + 90) % 360];
  const int32_t cos1 = ARC_SIN_TABLE[(deg + 91) % 360];
  *s = (sin0 * (ANGLE_SCALE - frac) + sin1 * frac + 32) >> 6;
  *c = (cos0 * (ANGLE_SCALE - frac) + cos1 * frac + 32) >> 6;
}

inline int32_t floorDiv8(int32_t v) { return v >= 0 ? v / 8 : -((-v + 7) / 8); }

inline void blendAt(Target& t, int32_t x, int32_t y, uint16_t color, int32_t alpha) {
  if (alpha <= 0 || x < t.clipX0 || y < t.clipY0 || x >= t.clipX1 || y >= t.clipY1) return;
  uint16_t& px = t.pixels[y * t.stride + x];
  if (alpha >= 256) {
    px = color;
    return;
  }
  const int32_t dr = (px >> 11) & 31, dg = (px >> 5) & 63, db = px & 31;
  const int32_t cr = (color >> 11) & 31, cg = (color >> 5) & 63, cb = color & 31;
  const int32_t r = (dr * (256 - alpha) + cr * alpha + 128) >> 8;
  const int32_t g = (dg * (256 - alpha) + cg * alpha + 128) >> 8;
  const int32_t b = (db * (256 - alpha) + cb * alpha + 128) >> 8;
  px = (uint16_t)((r << 11) | (g << 5) | b);
}

struct Placed {
  bool circle = false;
  std::vector<PolygonRaster::Point> poly;
  int32_t cx = 0, cy = 0, r = 0, rIn = 0;
};

Placed place(const Shape& shape, const Layer& layer, const Description& desc, const Pose& pose) {
  const int32_t dx = layer.dx + (layer.motion == Motion::Translate ? pose.shiftX : 0);
  const int32_t dy = layer.dy + (layer.motion == Motion::Translate ? pose.shiftY : 0);
  int32_t s = 0, c = 16384;
  if (layer.motion == Motion::Rotate) sinCos(pose.angle64, &s, &c);
  auto at = [&](int32_t x, int32_t y, int32_t* ox, int32_t* oy) {
    if (layer.motion == Motion::Rotate) {
      const int32_t rx = x - desc.pivotX;
      const int32_t ry = y - desc.pivotY;
      x = desc.pivotX + ((rx * c - ry * s + 8192) >> 14);
      y = desc.pivotY + ((rx * s + ry * c + 8192) >> 14);
    }
    *ox = x + dx;
    *oy = y + dy;
  };
  Placed p;
  p.circle = shape.circle;
  if (shape.circle) {
    at(shape.cx, shape.cy, &p.cx, &p.cy);
    p.r = shape.r;
    p.rIn = shape.rIn;
  } else {
    p.poly.reserve(shape.poly.size());
    for (const PolygonRaster::Point& q : shape.poly) {
      PolygonRaster::Point o;
      at(q.x, q.y, &o.x, &o.y);
      p.poly.push_back(o);
    }
  }
  return p;
}

int32_t circleCoverage(int32_t px, int32_t py, int32_t cx, int32_t cy, int32_t r, int32_t rIn) {
  // In 32 bits (a gauge is a few thousand eighths across), and a pixel
  // whose nearest and farthest sub-samples agree is answered without
  // sampling: the same count, a sixteenth of the work.
  const int32_t r2 = r * r;
  const int32_t i2 = rIn * rIn;
  const int32_t dx0 = px * 8 + 1 - cx, dx1 = px * 8 + 7 - cx;
  const int32_t dy0 = py * 8 + 1 - cy, dy1 = py * 8 + 7 - cy;
  const int32_t fx = dx0 * dx0 > dx1 * dx1 ? dx0 * dx0 : dx1 * dx1;
  const int32_t fy = dy0 * dy0 > dy1 * dy1 ? dy0 * dy0 : dy1 * dy1;
  const int32_t nx = (dx0 <= 0 && dx1 >= 0) ? 0 : (dx0 * dx0 < dx1 * dx1 ? dx0 * dx0 : dx1 * dx1);
  const int32_t ny = (dy0 <= 0 && dy1 >= 0) ? 0 : (dy0 * dy0 < dy1 * dy1 ? dy0 * dy0 : dy1 * dy1);
  const int32_t farthest = fx + fy, nearest = nx + ny;
  if (nearest > r2) return 0;                                       // every sample outside
  if (rIn > 0 && farthest < i2) return 0;                           // every sample in the hole
  if (farthest <= r2 && (rIn == 0 || nearest >= i2)) return 16;     // every sample in the band
  int32_t n = 0;
  for (int32_t sy = 0; sy < 4; sy++) {
    const int32_t dy = py * 8 + 1 + sy * 2 - cy;
    for (int32_t sx = 0; sx < 4; sx++) {
      const int32_t dx = px * 8 + 1 + sx * 2 - cx;
      const int32_t d2 = dx * dx + dy * dy;
      if (d2 <= r2 && (rIn == 0 || d2 >= i2)) n++;
    }
  }
  return n;
}

// The winding of a polygon, +1 or -1: which side of its edges is inside.
int32_t windingOf(const std::vector<PolygonRaster::Point>& pts) {
  const size_t n = pts.size();
  int64_t area = 0;
  for (size_t i = 0; i < n; i++) {
    const PolygonRaster::Point& a = pts[i];
    const PolygonRaster::Point& b = pts[(i + 1) % n];
    area += (int64_t)a.x * b.y - (int64_t)b.x * a.y;
  }
  return area >= 0 ? 1 : -1;
}

// Squared distance, eighths squared, from (x, y) to a convex polygon of
// winding `sign`; 0 inside. Every product but the last fits 32 bits
// (corners within a few thousand eighths): a 64-bit multiply is a call on
// the ESP32, and this runs for every pixel of a glow or a shadow.
int64_t polyDistance2(int32_t x, int32_t y, const std::vector<PolygonRaster::Point>& pts, int32_t sign) {
  const size_t n = pts.size();
  bool inside = true;
  int64_t best = INT64_MAX;
  for (size_t i = 0; i < n; i++) {
    const PolygonRaster::Point& a = pts[i];
    const PolygonRaster::Point& b = pts[(i + 1) % n];
    const int32_t vx = b.x - a.x, vy = b.y - a.y;
    const int32_t wx = x - a.x, wy = y - a.y;
    const int32_t cross = vx * wy - vy * wx;
    if (cross * sign < 0) inside = false;
    const int32_t len2 = vx * vx + vy * vy;
    const int32_t dot = wx * vx + wy * vy;
    int64_t d2;
    if (len2 == 0 || dot <= 0) {
      d2 = wx * wx + wy * wy;
    } else if (dot >= len2) {
      const int32_t ex = x - b.x, ey = y - b.y;
      d2 = ex * ex + ey * ey;
    } else {
      d2 = ((int64_t)cross * cross) / len2;  // both non-negative: the floor
    }
    if (d2 < best) best = d2;
  }
  return inside ? 0 : best;
}

// The soft field's level for a point - 0 inside, k up to `levels` when its
// squared distance is below (8k)^2, levels + 1 beyond - without dividing:
// floor(c^2 / l^2) < T exactly when c^2 < T * l^2. The level of the nearest
// edge is the smallest edge level, as the level of the smallest distance is
// (it only grows with the distance), so this is polyDistance2() followed by
// the table lookup, pixel for pixel - a 64-bit division per edge and pixel
// was most of a 400 px needle's shadow on the 4.3B (I9).
int32_t polyLevel(int32_t x, int32_t y, const std::vector<PolygonRaster::Point>& pts, int32_t sign, int32_t levels) {
  const size_t n = pts.size();
  bool inside = true;
  int32_t best = levels + 1;
  for (size_t i = 0; i < n; i++) {
    const PolygonRaster::Point& a = pts[i];
    const PolygonRaster::Point& b = pts[(i + 1) % n];
    const int32_t vx = b.x - a.x, vy = b.y - a.y;
    const int32_t wx = x - a.x, wy = y - a.y;
    const int32_t cross = vx * wy - vy * wx;
    if (cross * sign < 0) inside = false;
    if (best == 0) continue;  // nothing is nearer than 0
    const int32_t len2 = vx * vx + vy * vy;
    const int32_t dot = wx * vx + wy * vy;
    int32_t k;
    if (len2 == 0 || dot <= 0 || dot >= len2) {
      int32_t d2;
      if (len2 == 0 || dot <= 0) {
        d2 = wx * wx + wy * wy;
      } else {
        const int32_t ex = x - b.x, ey = y - b.y;
        d2 = ex * ex + ey * ey;
      }
      // A distance that rounds to 0 is the inside's level, as the designer
      // has it (needle-raster.ts: k stays 0 unless d2 > 0).
      k = d2 == 0 ? 0 : levels + 1;
      for (int32_t j = 1; k != 0 && j < best && j <= levels; j++) {
        if (d2 < (j * 8) * (j * 8)) {
          k = j;
          break;
        }
      }
    } else {
      const int64_t c2 = (int64_t)cross * cross;
      // floor(c2 / len2) == 0 exactly when c2 < len2.
      k = c2 < len2 ? 0 : levels + 1;
      for (int32_t j = 1; k != 0 && j < best && j <= levels; j++) {
        if (c2 < (int64_t)((j * 8) * (j * 8)) * len2) {
          k = j;
          break;
        }
      }
    }
    if (k < best) best = k;
  }
  return inside ? 0 : best;
}

void placedBounds(const Placed& p, int32_t reach8, int32_t* x0, int32_t* y0, int32_t* x1, int32_t* y1) {
  int32_t minX, minY, maxX, maxY;
  if (p.circle) {
    minX = p.cx - p.r;
    maxX = p.cx + p.r;
    minY = p.cy - p.r;
    maxY = p.cy + p.r;
  } else {
    minX = maxX = p.poly[0].x;
    minY = maxY = p.poly[0].y;
    for (const PolygonRaster::Point& q : p.poly) {
      if (q.x < minX) minX = q.x;
      if (q.x > maxX) maxX = q.x;
      if (q.y < minY) minY = q.y;
      if (q.y > maxY) maxY = q.y;
    }
  }
  *x0 = floorDiv8(minX - reach8);
  *y0 = floorDiv8(minY - reach8);
  *x1 = floorDiv8(maxX + reach8);
  *y1 = floorDiv8(maxY + reach8);
}

void drawSoft(Target& t, const Placed& p, const Layer& layer) {
  const int32_t levels = (int32_t)layer.soft.size() - 1;
  const int32_t reach8 = levels * 8 + 8;
  int32_t bx0, by0, bx1, by1;
  placedBounds(p, reach8, &bx0, &by0, &bx1, &by1);
  // Only rows and columns the clip lets through: a region redrawn on its
  // own walks only what it shows.
  if (by0 < t.clipY0) by0 = t.clipY0;
  if (by1 > t.clipY1 - 1) by1 = t.clipY1 - 1;
  if (bx0 < t.clipX0) bx0 = t.clipX0;
  if (bx1 > t.clipX1 - 1) bx1 = t.clipX1 - 1;
  const int64_t r2 = (int64_t)p.r * p.r;
  const int64_t i2 = (int64_t)p.rIn * p.rIn;
  const int32_t winding = p.circle ? 1 : windingOf(p.poly);
  struct SpanEdge {
    int32_t ax, ay;
    float nx, ny, reach;
    bool vertical;
  };
  SpanEdge spanEdges[8];
  size_t spanN = 0;
  if (!p.circle) {
    const size_t n = p.poly.size() < 8 ? p.poly.size() : 8;
    for (size_t i = 0; i < n; i++) {
      const PolygonRaster::Point& a = p.poly[i];
      const PolygonRaster::Point& b = p.poly[(i + 1) % p.poly.size()];
      const float vx = (float)(b.x - a.x), vy = (float)(b.y - a.y);
      const float len = sqrtf(vx * vx + vy * vy);
      if (len == 0) continue;
      SpanEdge& e = spanEdges[spanN++];
      e.ax = a.x;
      e.ay = a.y;
      // Outward normal: inside is cross(v, w) * winding >= 0.
      e.nx = vy * winding / len;
      e.ny = -vx * winding / len;
      e.reach = (float)(reach8 + 16);
      e.vertical = fabsf(e.nx) < 1e-6f;
    }
    // A polygon of more than eight corners walks its whole box (none of the
    // styles has one).
    if (p.poly.size() > 8) spanN = 0;
  }
  for (int32_t y = by0; y <= by1; y++) {
    const int32_t cy = y * 8 + 4;
    // Only the columns a field this wide can reach - a speed-up that cannot
    // change a pixel: whatever lies outside is farther than the table goes
    // (the spike, designer docs/2026-11-23-instrument-styles.md: 5x).
    int32_t lo = bx0, hi = bx1;
    if (p.circle) {
      const int64_t rr = p.r + reach8;
      const int64_t dyc = cy - p.cy;
      if (dyc * dyc > rr * rr) continue;
      const int64_t half = isqrt(rr * rr - dyc * dyc) + 8;
      const int32_t l = floorDiv8((int32_t)(p.cx - half)), h = floorDiv8((int32_t)(p.cx + half));
      if (l > lo) lo = l;
      if (h < hi) hi = h;
    } else {
      // A convex polygon grown by reach8 on every side lies within the
      // intersection of its edges' half-planes, each moved out by reach8.
      // The edges' normals are worked out once per shape, in float: this
      // core has no double unit, and per row and edge a double sqrt and
      // divide made a 400 px shadow cost 29 ms (I9). The bound only ever
      // widens by its slack, so no pixel can change.
      bool empty = false;
      float flo = -1e30f, fhi = 1e30f;
      for (size_t i = 0; i < spanN && !empty; i++) {
        const SpanEdge& e = spanEdges[i];
        const float rhs = e.reach - e.ny * (float)(cy - e.ay);
        if (e.vertical) {
          if (rhs < 0) empty = true;
          continue;
        }
        const float xb = (float)e.ax + rhs / e.nx;
        if (e.nx > 0) {
          if (xb < fhi) fhi = xb;
        } else {
          if (xb > flo) flo = xb;
        }
      }
      if (empty || flo > fhi) continue;
      if (flo > -1e29f) {
        const int32_t l = (int32_t)floorf(flo / 8.0f) - 1;
        if (l > lo) lo = l;
      }
      if (fhi < 1e29f) {
        const int32_t h = (int32_t)floorf(fhi / 8.0f) + 1;
        if (h < hi) hi = h;
      }
    }
    if (!p.circle && p.poly.size() <= 8 && levels <= 8 && lo <= hi) {
      // polyLevel() along the row by increments: per edge, cross and dot
      // are linear in x and the end distances quadratic, so a pixel costs
      // additions, not multiplications - the same numbers exactly (a 400 px
      // shadow was most of a frame on the 4.3B, I9).
      const size_t n = p.poly.size();
      int32_t cross[8], dCross[8], dot[8], dDot[8], len2[8], wa2[8], dWa2[8], wb2[8], dWb2[8];
      // Per edge, the squared-distance thresholds as cross^2 bounds:
      // floor(c^2 / l^2) < (8j)^2 exactly when c^2 < 64 j^2 l^2. Worked out
      // once a row, not once a pixel: a 64 by 64 bit multiply is a library
      // call on this core.
      int64_t thr[8][9];
      const int32_t cx0 = lo * 8 + 4;
      for (size_t i = 0; i < n; i++) {
        const PolygonRaster::Point& a = p.poly[i];
        const PolygonRaster::Point& b = p.poly[(i + 1) % n];
        const int32_t vx = b.x - a.x, vy = b.y - a.y;
        const int32_t wx = cx0 - a.x, wy = cy - a.y;
        cross[i] = vx * wy - vy * wx;
        dCross[i] = -vy * 8;
        dot[i] = wx * vx + wy * vy;
        dDot[i] = vx * 8;
        len2[i] = vx * vx + vy * vy;
        for (int32_t j = 0; j <= levels && j < 9; j++) thr[i][j] = j == 0 ? (int64_t)len2[i] : (int64_t)(64 * j * j) * len2[i];
        wa2[i] = wx * wx + wy * wy;
        dWa2[i] = 16 * wx + 64;  // (wx + 8)^2 - wx^2, wx growing by 8 a pixel
        const int32_t ex = cx0 - b.x, ey = cy - b.y;
        wb2[i] = ex * ex + ey * ey;
        dWb2[i] = 16 * ex + 64;
      }
      for (int32_t x = lo; x <= hi; x++) {
        bool inside = true;
        int32_t best = levels + 1;
        for (size_t i = 0; i < n; i++) {
          if (cross[i] * winding < 0) inside = false;
          if (best != 0) {
            int32_t k;
            if (len2[i] == 0 || dot[i] <= 0 || dot[i] >= len2[i]) {
              const int32_t d2 = (len2[i] == 0 || dot[i] <= 0) ? wa2[i] : wb2[i];
              k = d2 == 0 ? 0 : levels + 1;
              for (int32_t j = 1; k != 0 && j < best && j <= levels; j++)
                if (d2 < (j * 8) * (j * 8)) {
                  k = j;
                  break;
                }
            } else {
              const int64_t c2 = (int64_t)cross[i] * cross[i];  // 32 x 32 -> 64: in hardware
              k = c2 < thr[i][0] ? 0 : levels + 1;
              for (int32_t j = 1; k != 0 && j < best && j <= levels; j++)
                if (c2 < thr[i][j]) {
                  k = j;
                  break;
                }
            }
            if (k < best) best = k;
          }
          cross[i] += dCross[i];
          dot[i] += dDot[i];
          wa2[i] += dWa2[i];
          dWa2[i] += 128;
          wb2[i] += dWb2[i];
          dWb2[i] += 128;
        }
        const int32_t k = inside ? 0 : best;
        if (k <= levels) blendAt(t, x, y, layer.color, layer.soft[k]);
      }
      continue;
    }
    for (int32_t x = lo; x <= hi; x++) {
      const int32_t cx = x * 8 + 4;
      if (!p.circle) {
        const int32_t k = polyLevel(cx, cy, p.poly, winding, levels);
        if (k <= levels) blendAt(t, x, y, layer.color, layer.soft[k]);
        continue;
      }
      int64_t d2;
      {
        const int64_t ddx = cx - p.cx, ddy = cy - p.cy;
        const int64_t dc2 = ddx * ddx + ddy * ddy;
        if (dc2 <= r2 && (p.rIn == 0 || dc2 >= i2)) {
          d2 = 0;
        } else {
          const int64_t dc = isqrt(dc2);
          const int64_t out = dc - p.r;
          const int64_t inn = p.rIn - dc;
          const int64_t e = (p.rIn > 0 && dc2 < i2) ? inn : out;
          d2 = e > 0 ? e * e : 0;
        }
      }
      int32_t k = 0;
      if (d2 > 0) {
        k = levels + 1;
        for (int32_t j = 1; j <= levels; j++) {
          if (d2 < (int64_t)(j * 8) * (j * 8)) {
            k = j;
            break;
          }
        }
      }
      if (k <= levels) blendAt(t, x, y, layer.color, layer.soft[k]);
    }
  }
}

void drawBody(Target& t, const Placed& p, const Layer& layer) {
  const int32_t a = layer.alpha < 0 ? 0 : (layer.alpha > 256 ? 256 : layer.alpha);
  if (!p.circle) {
    PolygonRaster::Sink sink;
    sink.fullRun = [&](int16_t y, int16_t x0, int16_t x1) {
      for (int32_t x = x0; x <= x1; x++) blendAt(t, x, y, layer.color, a);
    };
    sink.partial = [&](int16_t y, int16_t x, uint8_t cov) { blendAt(t, x, y, layer.color, (a * cov + 8) >> 4); };
    PolygonRaster::fill(p.poly, (int16_t)t.clipY0, (int16_t)(t.clipY1 - 1), sink);
    return;
  }
  int32_t bx0, by0, bx1, by1;
  placedBounds(p, 0, &bx0, &by0, &bx1, &by1);
  for (int32_t y = by0 < t.clipY0 ? t.clipY0 : by0; y <= (by1 > t.clipY1 - 1 ? t.clipY1 - 1 : by1); y++) {
    for (int32_t x = bx0 < t.clipX0 ? t.clipX0 : bx0; x <= (bx1 > t.clipX1 - 1 ? t.clipX1 - 1 : bx1); x++) {
      const int32_t cov = circleCoverage(x, y, p.cx, p.cy, p.r, p.rIn);
      if (cov > 0) blendAt(t, x, y, layer.color, cov == 16 ? a : (a * cov + 8) >> 4);
    }
  }
}

}  // namespace

int64_t isqrt(int64_t n) {
  if (n < 2) return n < 0 ? 0 : n;
  int64_t x = n;
  int64_t y = (x + 1) / 2;
  while (y < x) {
    x = y;
    y = (x + n / x) / 2;
  }
  return x;
}

int32_t angle64Of(double degrees) { return (int32_t)floor(degrees * 64.0 + 0.5); }

bool bounds(const Description& desc, const Pose& pose, int32_t* x0, int32_t* y0, int32_t* x1, int32_t* y1) {
  bool any = false;
  for (const Layer& layer : desc.layers) {
    const int32_t reach8 = layer.soft.empty() ? 8 : (int32_t)layer.soft.size() * 8 + 8;
    for (const Shape& shape : layer.shapes) {
      if (!shape.circle && shape.poly.size() < 3) continue;
      const Placed p = place(shape, layer, desc, pose);
      int32_t a, b, c, d;
      // One pixel more each way than the raster itself walks.
      placedBounds(p, reach8, &a, &b, &c, &d);
      a--;
      b--;
      c++;
      d++;
      if (!any) {
        *x0 = a;
        *y0 = b;
        *x1 = c;
        *y1 = d;
        any = true;
      } else {
        if (a < *x0) *x0 = a;
        if (b < *y0) *y0 = b;
        if (c > *x1) *x1 = c;
        if (d > *y1) *y1 = d;
      }
    }
  }
  return any;
}

void draw(Target& target, const Description& desc, const Pose& pose) {
  if (!target.pixels || target.width <= 0 || target.height <= 0) return;
  Target t = target;
  t.clipX0 = t.clipX0 < 0 ? 0 : t.clipX0;
  t.clipY0 = t.clipY0 < 0 ? 0 : t.clipY0;
  t.clipX1 = (t.clipX1 < 0 || t.clipX1 > t.width) ? t.width : t.clipX1;
  t.clipY1 = (t.clipY1 < 0 || t.clipY1 > t.height) ? t.height : t.clipY1;
  if (t.clipX0 >= t.clipX1 || t.clipY0 >= t.clipY1) return;
  for (const Layer& layer : desc.layers) {
    for (const Shape& shape : layer.shapes) {
      if (!shape.circle && shape.poly.size() < 3) continue;
      const Placed p = place(shape, layer, desc, pose);
      if (!layer.soft.empty()) {
        drawSoft(t, p, layer);
      } else {
        drawBody(t, p, layer);
      }
    }
  }
}

}  // namespace needleraster
