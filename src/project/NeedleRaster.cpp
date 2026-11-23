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
  const int64_t r2 = (int64_t)r * r;
  const int64_t i2 = (int64_t)rIn * rIn;
  int32_t n = 0;
  for (int32_t sy = 0; sy < 4; sy++) {
    const int64_t dy = py * 8 + 1 + sy * 2 - cy;
    for (int32_t sx = 0; sx < 4; sx++) {
      const int64_t dx = px * 8 + 1 + sx * 2 - cx;
      const int64_t d2 = dx * dx + dy * dy;
      if (d2 <= r2 && (rIn == 0 || d2 >= i2)) n++;
    }
  }
  return n;
}

// Squared distance, eighths squared, from (x, y) to a convex polygon; 0 inside.
int64_t polyDistance2(int32_t x, int32_t y, const std::vector<PolygonRaster::Point>& pts) {
  const size_t n = pts.size();
  int64_t area = 0;
  for (size_t i = 0; i < n; i++) {
    const PolygonRaster::Point& a = pts[i];
    const PolygonRaster::Point& b = pts[(i + 1) % n];
    area += (int64_t)a.x * b.y - (int64_t)b.x * a.y;
  }
  const int64_t sign = area >= 0 ? 1 : -1;
  bool inside = true;
  int64_t best = INT64_MAX;
  for (size_t i = 0; i < n; i++) {
    const PolygonRaster::Point& a = pts[i];
    const PolygonRaster::Point& b = pts[(i + 1) % n];
    const int64_t vx = b.x - a.x, vy = b.y - a.y;
    const int64_t wx = x - a.x, wy = y - a.y;
    const int64_t cross = vx * wy - vy * wx;
    if (cross * sign < 0) inside = false;
    const int64_t len2 = vx * vx + vy * vy;
    const int64_t dot = wx * vx + wy * vy;
    int64_t d2;
    if (len2 == 0 || dot <= 0) {
      d2 = wx * wx + wy * wy;
    } else if (dot >= len2) {
      const int64_t ex = x - b.x, ey = y - b.y;
      d2 = ex * ex + ey * ey;
    } else {
      d2 = (cross * cross) / len2;  // both non-negative: the floor
    }
    if (d2 < best) best = d2;
  }
  return inside ? 0 : best;
}

void bounds(const Placed& p, int32_t reach8, int32_t* x0, int32_t* y0, int32_t* x1, int32_t* y1) {
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
  bounds(p, reach8, &bx0, &by0, &bx1, &by1);
  if (by0 < 0) by0 = 0;
  if (by1 > t.height - 1) by1 = t.height - 1;
  if (bx0 < 0) bx0 = 0;
  if (bx1 > t.width - 1) bx1 = t.width - 1;
  const int64_t r2 = (int64_t)p.r * p.r;
  const int64_t i2 = (int64_t)p.rIn * p.rIn;
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
      int64_t area = 0;
      const size_t n = p.poly.size();
      for (size_t i = 0; i < n; i++) {
        area += (int64_t)p.poly[i].x * p.poly[(i + 1) % n].y - (int64_t)p.poly[(i + 1) % n].x * p.poly[i].y;
      }
      const double sign = area >= 0 ? 1.0 : -1.0;
      double dlo = -1e18, dhi = 1e18;
      bool empty = false;
      for (size_t i = 0; i < n && !empty; i++) {
        const PolygonRaster::Point& a = p.poly[i];
        const PolygonRaster::Point& b = p.poly[(i + 1) % n];
        const double vx = b.x - a.x, vy = b.y - a.y;
        const double len = sqrt(vx * vx + vy * vy);
        if (len == 0) continue;
        // Outward normal: inside is cross(v, w) * sign >= 0, and
        // cross(v, w) * sign = -(n . w) * len for n = sign * (vy, -vx) / len.
        const double nx = vy * sign / len, ny = -vx * sign / len;
        // Inside the grown edge: nx*(X - a.x) + ny*(cy - a.y) <= reach8 (+ slack).
        const double rhs = reach8 + 16 - ny * (cy - a.y);
        if (fabs(nx) < 1e-9) {
          if (rhs < 0) empty = true;
          continue;
        }
        const double xb = a.x + rhs / nx;
        if (nx > 0) {
          if (xb < dhi) dhi = xb;
        } else {
          if (xb > dlo) dlo = xb;
        }
      }
      if (empty || dlo > dhi) continue;
      if (dlo > -1e17) {
        const int32_t l = (int32_t)floor(dlo / 8.0) - 1;
        if (l > lo) lo = l;
      }
      if (dhi < 1e17) {
        const int32_t h = (int32_t)floor(dhi / 8.0) + 1;
        if (h < hi) hi = h;
      }
    }
    for (int32_t x = lo; x <= hi; x++) {
      const int32_t cx = x * 8 + 4;
      int64_t d2;
      if (!p.circle) {
        d2 = polyDistance2(cx, cy, p.poly);
      } else {
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
    PolygonRaster::fill(p.poly, 0, (int16_t)(t.height - 1), sink);
    return;
  }
  int32_t bx0, by0, bx1, by1;
  bounds(p, 0, &bx0, &by0, &bx1, &by1);
  for (int32_t y = by0 < 0 ? 0 : by0; y <= (by1 > t.height - 1 ? t.height - 1 : by1); y++) {
    for (int32_t x = bx0 < 0 ? 0 : bx0; x <= (bx1 > t.width - 1 ? t.width - 1 : bx1); x++) {
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
