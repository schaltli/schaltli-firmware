#include "NeedleRaster.h"

#include <math.h>

#include "ArcSinTable.h"
#include "Gradient565.h"

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
inline int32_t ceilDiv8(int32_t v) { return -floorDiv8(-v); }

// The colour of a layer at pixel (x, y): its own, or its gradient's step
// there (designer colourAt()).
inline uint16_t colourAt(const Layer& layer, int32_t x, int32_t y) {
  const Gradient& g = layer.gradient;
  if (!g.on) return layer.color;
  struct Channels {
    int32_t r, g, b;
  };
  const Channels from = {(layer.color >> 11) & 31, (layer.color >> 5) & 63, layer.color & 31};
  const Channels to = {(g.to >> 11) & 31, (g.to >> 5) & 63, g.to & 31};
  const Channels c = levelglow::gradient565(from, to, (g.alongY ? y : x) - g.start, g.length < 1 ? 1 : g.length);
  return (uint16_t)((c.r << 11) | (c.g << 5) | c.b);
}

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

// An arc placed: centre, radius and half width (eighths), span (1/64
// degree), the ends' directions (x 16384) and points, relative to the centre.
struct PlacedArc {
  int32_t cx = 0, cy = 0, r = 0, hw = 0, span = 0;
  int32_t u0x = 0, u0y = 0, u1x = 0, u1y = 0;
  int32_t e0x = 0, e0y = 0, e1x = 0, e1y = 0;
};

struct Placed {
  bool empty = false;  // draws nothing (an empty fill, a polygon of fewer than 3 corners)
  bool circle = false;
  bool arc = false;
  std::vector<PolygonRaster::Point> poly;
  int32_t cx = 0, cy = 0, r = 0, rIn = 0;
  PlacedArc a;
};

inline int32_t clampTo(int32_t v, int32_t lo, int32_t hi) { return v > hi ? (hi > lo ? hi : lo) : (v > lo ? v : lo); }

// ArcRaster's direction: (sin, -cos) x 16384, 0 = up, clockwise.
inline void direction(int32_t angle64, int32_t* x, int32_t* y) {
  int32_t s, c;
  sinCos(angle64, &s, &c);
  *x = s;
  *y = -c;
}

void placeArc(Placed& p, const Shape& shape, const Layer& layer, const Description& desc, const Pose& pose) {
  const int32_t end = layer.motion == Motion::Fill ? clampTo(pose.angle64, shape.from64, shape.to64) : shape.to64;
  const int32_t span = end - shape.from64;
  if (span <= 0) {
    p.empty = true;
    return;
  }
  p.arc = true;
  PlacedArc& a = p.a;
  a.cx = desc.pivotX + layer.dx;
  a.cy = desc.pivotY + layer.dy;
  a.r = shape.r;
  a.hw = shape.hw;
  a.span = span;
  direction(shape.from64, &a.u0x, &a.u0y);
  direction(end, &a.u1x, &a.u1y);
  a.e0x = (a.r * a.u0x + 8192) >> 14;
  a.e0y = (a.r * a.u0y + 8192) >> 14;
  a.e1x = (a.r * a.u1x + 8192) >> 14;
  a.e1y = (a.r * a.u1y + 8192) >> 14;
}

void placeRect(Placed& p, const Shape& shape, const Layer& layer, const Pose& pose) {
  int32_t x0 = shape.x0, y0 = shape.y0, x1 = shape.x1, y1 = shape.y1;
  if (layer.motion == Motion::Fill) {
    switch (layer.grow) {
      case Grow::Right:
        x1 = clampTo(x0 + pose.shiftX, x0, x1);
        break;
      case Grow::Left:
        x0 = clampTo(x1 + pose.shiftX, x0, x1);
        break;
      case Grow::Down:
        y1 = clampTo(y0 + pose.shiftY, y0, y1);
        break;
      case Grow::Up:
        y0 = clampTo(y1 + pose.shiftY, y0, y1);
        break;
    }
  } else if (layer.motion == Motion::Translate) {
    x0 += pose.shiftX;
    x1 += pose.shiftX;
    y0 += pose.shiftY;
    y1 += pose.shiftY;
  }
  if (x1 <= x0 || y1 <= y0) {
    p.empty = true;
    return;
  }
  x0 += layer.dx;
  x1 += layer.dx;
  y0 += layer.dy;
  y1 += layer.dy;
  p.poly.resize(4);
  p.poly[0].x = x0;
  p.poly[0].y = y0;
  p.poly[1].x = x1;
  p.poly[1].y = y0;
  p.poly[2].x = x1;
  p.poly[2].y = y1;
  p.poly[3].x = x0;
  p.poly[3].y = y1;
}

Placed place(const Shape& shape, const Layer& layer, const Description& desc, const Pose& pose) {
  Placed p;
  if (shape.kind == Kind::Arc) {
    placeArc(p, shape, layer, desc, pose);
    return p;
  }
  if (shape.kind == Kind::Rect) {
    placeRect(p, shape, layer, pose);
    return p;
  }
  if (shape.kind == Kind::Poly && shape.poly.size() < 3) {
    p.empty = true;
    return p;
  }
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
  p.circle = shape.kind == Kind::Circle;
  if (p.circle) {
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

// The arc's tests, relative to its centre. In 32 bits: a direction is at
// most 16384 and a point a few thousand eighths from the centre, so a cross
// product stays below 2^31 (a 64-bit multiply is a call on the ESP32).
inline bool inSector(const PlacedArc& a, int32_t x, int32_t y) {
  if (a.span >= FULL_TURN) return true;
  if (a.span <= 180 * ANGLE_SCALE) return a.u0x * y - a.u0y * x >= 0 && x * a.u1y - y * a.u1x >= 0;
  return !(a.u1x * y - a.u1y * x > 0 && x * a.u0y - y * a.u0x > 0);
}

inline int32_t endDistance2(const PlacedArc& a, int32_t x, int32_t y) {
  const int32_t d0 = (x - a.e0x) * (x - a.e0x) + (y - a.e0y) * (y - a.e0y);
  const int32_t d1 = (x - a.e1x) * (x - a.e1x) + (y - a.e1y) * (y - a.e1y);
  return d0 < d1 ? d0 : d1;
}

// Whether the point lies in the band: (r - hw)^2 <= d^2 <= (r + hw)^2 inside
// the sector, within hw of an end point outside it.
inline bool inArc(const PlacedArc& a, int32_t x, int32_t y, int32_t lo2, int32_t hi2, int32_t hw2) {
  if (inSector(a, x, y)) {
    const int32_t d2 = x * x + y * y;
    return d2 >= lo2 && d2 <= hi2;
  }
  return endDistance2(a, x, y) <= hw2;
}

// The distance from the band (eighths, 0 or less inside), for the soft field.
inline int32_t arcDistance(const PlacedArc& a, int32_t x, int32_t y) {
  int32_t centre;
  if (inSector(a, x, y)) {
    centre = (int32_t)isqrt((int64_t)x * x + (int64_t)y * y) - a.r;
    if (centre < 0) centre = -centre;
  } else {
    centre = (int32_t)isqrt(endDistance2(a, x, y));
  }
  return centre - a.hw;
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
  if (p.arc) {
    const int32_t R = p.a.r + p.a.hw;
    minX = p.a.cx - R;
    maxX = p.a.cx + R;
    minY = p.a.cy - R;
    maxY = p.a.cy + R;
  } else if (p.circle) {
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

// The rows and columns of a box the clip lets through.
inline void clipBox(const Target& t, int32_t* bx0, int32_t* by0, int32_t* bx1, int32_t* by1) {
  if (*by0 < t.clipY0) *by0 = t.clipY0;
  if (*by1 > t.clipY1 - 1) *by1 = t.clipY1 - 1;
  if (*bx0 < t.clipX0) *bx0 = t.clipX0;
  if (*bx1 > t.clipX1 - 1) *bx1 = t.clipX1 - 1;
}

// An arc's soft field. Only the pixels whose centres lie in the band
// r +- (hw + reach) are walked: farther out, inside the sector the distance
// from the band is at least reach, and outside it the nearer end point
// (r from the centre, give or take a rounding) is still reach - 2 away -
// beyond the table either way, so leaving them out cannot change a pixel.
// The level is found without a root: isqrt(n) < m exactly when n < m^2, so
// per shape the table's distances become squared thresholds - inside the
// sector beyond r on d^2 < (r + hw + 8k)^2, short of r on
// d^2 >= (r - hw - 8k + 1)^2, outside it on end^2 < (hw + 8k)^2 (k = 0:
// r + hw + 1, r - hw, hw + 1), the first that holds being the level.
void drawSoftArc(Target& t, const PlacedArc& a, const Layer& layer) {
  const int32_t levels = (int32_t)layer.soft.size() - 1;
  const int32_t reach8 = levels * 8 + 8;
  const int32_t W = a.hw + reach8;
  int32_t bx0 = floorDiv8(a.cx - a.r - W), by0 = floorDiv8(a.cy - a.r - W);
  int32_t bx1 = floorDiv8(a.cx + a.r + W), by1 = floorDiv8(a.cy + a.r + W);
  clipBox(t, &bx0, &by0, &bx1, &by1);
  const int32_t outer = a.r + W, inner = a.r - W;
  const int32_t outer2 = outer * outer, inner2 = inner > 0 ? inner * inner : 0;
  const int32_t r2 = a.r * a.r;
  const int32_t MAXL = 16;
  const bool table = levels <= MAXL;
  int32_t thrOut[MAXL + 1], thrIn[MAXL + 1], thrEnd[MAXL + 1];
  if (table) {
    for (int32_t j = 0; j <= levels; j++) {
      const int32_t step = j == 0 ? 1 : 8 * j;
      const int32_t o = a.r + a.hw + step;
      const int32_t i = a.r - a.hw - step + 1;
      const int32_t e = a.hw + step;
      thrOut[j] = o * o;
      thrIn[j] = i > 0 ? i * i : 0;
      thrEnd[j] = e * e;
    }
  }
  for (int32_t y = by0; y <= by1; y++) {
    const int32_t ry = y * 8 + 4 - a.cy;
    const int32_t ry2 = ry * ry;
    if (ry2 > outer2) continue;
    // Pixel centres x * 8 + 4 within the outer circle, less those inside the
    // inner one (|rx| <= h exactly when rx^2 < inner^2 - ry^2).
    const int32_t m = (int32_t)isqrt(outer2 - ry2);
    int32_t L = ceilDiv8(a.cx - m - 4), H = floorDiv8(a.cx + m - 4);
    if (L < bx0) L = bx0;
    if (H > bx1) H = bx1;
    int32_t holeL = 1, holeH = 0;
    if (inner2 > ry2) {
      const int32_t h = (int32_t)isqrt(inner2 - ry2 - 1);
      holeL = ceilDiv8(a.cx - h - 4);
      holeH = floorDiv8(a.cx + h - 4);
    }
    for (int32_t x = L; x <= H; x++) {
      if (x >= holeL && x <= holeH) {
        x = holeH;
        continue;
      }
      const int32_t rx = x * 8 + 4 - a.cx;
      int32_t k = levels + 1;
      if (table) {
        if (inSector(a, rx, ry)) {
          const int32_t d2 = rx * rx + ry2;
          if (d2 >= r2) {
            for (int32_t j = 0; j <= levels; j++)
              if (d2 < thrOut[j]) {
                k = j;
                break;
              }
          } else {
            for (int32_t j = 0; j <= levels; j++)
              if (d2 >= thrIn[j]) {
                k = j;
                break;
              }
          }
        } else {
          const int32_t e2 = endDistance2(a, rx, ry);
          for (int32_t j = 0; j <= levels; j++)
            if (e2 < thrEnd[j]) {
              k = j;
              break;
            }
        }
      } else {
        // A table this long (none of the styles has one): the designer's
        // way, word for word.
        const int32_t e = arcDistance(a, rx, ry);
        const int64_t d2 = e > 0 ? (int64_t)e * e : 0;
        k = 0;
        if (d2 > 0) {
          k = levels + 1;
          for (int32_t j = 1; j <= levels; j++)
            if (d2 < (int64_t)(j * 8) * (j * 8)) {
              k = j;
              break;
            }
        }
      }
      if (k <= levels) blendAt(t, x, y, colourAt(layer, x, y), layer.soft[k]);
    }
  }
}

void drawSoft(Target& t, const Placed& p, const Layer& layer) {
  if (p.arc) {
    drawSoftArc(t, p.a, layer);
    return;
  }
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
        if (k <= levels) blendAt(t, x, y, colourAt(layer, x, y), layer.soft[k]);
      }
      continue;
    }
    for (int32_t x = lo; x <= hi; x++) {
      const int32_t cx = x * 8 + 4;
      if (!p.circle) {
        const int32_t k = polyLevel(cx, cy, p.poly, winding, levels);
        if (k <= levels) blendAt(t, x, y, colourAt(layer, x, y), layer.soft[k]);
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
      if (k <= levels) blendAt(t, x, y, colourAt(layer, x, y), layer.soft[k]);
    }
  }
}

// The columns [lo, hi] of a row a disc of squared radius `rad2` round
// (cx, cy) can reach with a sub-sample (eighths 1..7 of a row starting at
// ry0 = y * 8 + 1 - cy): false when none. Sub-samples at x * 8 + 1..7 lie in
// [-m, m] about cx only for x within floorDiv8(cx - m)..floorDiv8(cx + m).
inline bool discRow(int32_t ry0, int32_t cx, int32_t rad2, int32_t* lo, int32_t* hi) {
  const int32_t ry3 = ry0 + 6;
  const int32_t near2 = (ry0 <= 0 && ry3 >= 0) ? 0 : (ry0 * ry0 < ry3 * ry3 ? ry0 * ry0 : ry3 * ry3);
  if (near2 > rad2) return false;
  const int32_t m = (int32_t)isqrt(rad2 - near2);
  *lo = floorDiv8(cx - m);
  *hi = floorDiv8(cx + m);
  return true;
}

// An arc's body, 4 x 4 sub-samples a pixel as the designer has it, but per
// row only over the columns the band can reach: the outer circle's span
// less the pixels whose every sub-sample lies in the hole (d^2 < (r - hw)^2,
// which no sub-sample of the band inside the sector can), plus the two round
// end caps - the only other place a sub-sample can be in the band. Merged so
// that no pixel is mixed in twice; every pixel left out has no coverage.
void drawBodyArc(Target& t, const PlacedArc& a, const Layer& layer, int32_t alpha) {
  const int32_t R = a.r + a.hw;
  int32_t bx0 = floorDiv8(a.cx - R), by0 = floorDiv8(a.cy - R);
  int32_t bx1 = floorDiv8(a.cx + R), by1 = floorDiv8(a.cy + R);
  clipBox(t, &bx0, &by0, &bx1, &by1);
  const int32_t lo = a.r - a.hw, hi = a.r + a.hw;
  const int32_t lo2 = lo * lo, hi2 = hi * hi, hw2 = a.hw * a.hw;
  const bool whole = a.span >= FULL_TURN;
  for (int32_t y = by0; y <= by1; y++) {
    const int32_t ry0 = y * 8 + 1 - a.cy, ry3 = ry0 + 6;
    int32_t ivL[4], ivH[4];
    int32_t n = 0;
    int32_t L, H;
    if (discRow(ry0, a.cx, hi2, &L, &H)) {
      // The pixels wholly in the hole: every sub-sample row, the farthest
      // first, has rx^2 < lo^2 - ry^2, i.e. |rx| <= isqrt(lo^2 - ry^2 - 1).
      const int32_t far2 = ry0 * ry0 > ry3 * ry3 ? ry0 * ry0 : ry3 * ry3;
      int32_t holeL = 1, holeH = 0;
      if (lo2 > far2) {
        const int32_t h = (int32_t)isqrt(lo2 - far2 - 1);
        holeL = ceilDiv8(a.cx - h - 1);   // x * 8 + 1 - cx >= -h
        holeH = floorDiv8(a.cx + h - 7);  // x * 8 + 7 - cx <= h
      }
      if (holeL > holeH || holeH < L || holeL > H) {
        ivL[n] = L;
        ivH[n++] = H;
      } else {
        if (holeL > L) {
          ivL[n] = L;
          ivH[n++] = holeL - 1;
        }
        if (holeH < H) {
          ivL[n] = holeH + 1;
          ivH[n++] = H;
        }
      }
    }
    if (!whole) {
      if (discRow(ry0 - a.e0y, a.cx + a.e0x, hw2, &L, &H)) {
        ivL[n] = L;
        ivH[n++] = H;
      }
      if (discRow(ry0 - a.e1y, a.cx + a.e1x, hw2, &L, &H)) {
        ivL[n] = L;
        ivH[n++] = H;
      }
    }
    // Sort by start (at most four) and walk the union once.
    for (int32_t i = 1; i < n; i++)
      for (int32_t j = i; j > 0 && ivL[j] < ivL[j - 1]; j--) {
        const int32_t tl = ivL[j], th = ivH[j];
        ivL[j] = ivL[j - 1];
        ivH[j] = ivH[j - 1];
        ivL[j - 1] = tl;
        ivH[j - 1] = th;
      }
    int32_t next = bx0;  // the first column not yet walked
    for (int32_t i = 0; i < n; i++) {
      int32_t x = ivL[i] > next ? ivL[i] : next;
      const int32_t xe = ivH[i] < bx1 ? ivH[i] : bx1;
      for (; x <= xe; x++) {
        int32_t cov = 0;
        const int32_t rx0 = x * 8 + 1 - a.cx;
        for (int32_t sy = 0; sy < 4; sy++) {
          const int32_t ry = ry0 + sy * 2;
          for (int32_t sx = 0; sx < 4; sx++)
            if (inArc(a, rx0 + sx * 2, ry, lo2, hi2, hw2)) cov++;
        }
        if (cov > 0) blendAt(t, x, y, colourAt(layer, x, y), cov == 16 ? alpha : (alpha * cov + 8) >> 4);
      }
      if (xe + 1 > next) next = xe + 1;
    }
  }
}

void drawBody(Target& t, const Placed& p, const Layer& layer) {
  const int32_t a = layer.alpha < 0 ? 0 : (layer.alpha > 256 ? 256 : layer.alpha);
  if (p.arc) {
    drawBodyArc(t, p.a, layer, a);
    return;
  }
  if (!p.circle) {
    PolygonRaster::Sink sink;
    sink.fullRun = [&](int16_t y, int16_t x0, int16_t x1) {
      for (int32_t x = x0; x <= x1; x++) blendAt(t, x, y, colourAt(layer, x, y), a);
    };
    sink.partial = [&](int16_t y, int16_t x, uint8_t cov) { blendAt(t, x, y, colourAt(layer, x, y), (a * cov + 8) >> 4); };
    PolygonRaster::fill(p.poly, (int16_t)t.clipY0, (int16_t)(t.clipY1 - 1), sink);
    return;
  }
  int32_t bx0, by0, bx1, by1;
  placedBounds(p, 0, &bx0, &by0, &bx1, &by1);
  for (int32_t y = by0 < t.clipY0 ? t.clipY0 : by0; y <= (by1 > t.clipY1 - 1 ? t.clipY1 - 1 : by1); y++) {
    for (int32_t x = bx0 < t.clipX0 ? t.clipX0 : bx0; x <= (bx1 > t.clipX1 - 1 ? t.clipX1 - 1 : bx1); x++) {
      const int32_t cov = circleCoverage(x, y, p.cx, p.cy, p.r, p.rIn);
      if (cov > 0) blendAt(t, x, y, colourAt(layer, x, y), cov == 16 ? a : (a * cov + 8) >> 4);
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
      const Placed p = place(shape, layer, desc, pose);
      if (p.empty) continue;
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
      const Placed p = place(shape, layer, desc, pose);
      if (p.empty) continue;
      if (!layer.soft.empty()) {
        drawSoft(t, p, layer);
      } else {
        drawBody(t, p, layer);
      }
    }
  }
}

}  // namespace needleraster
