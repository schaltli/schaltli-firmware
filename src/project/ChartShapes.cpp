#include "ChartShapes.h"

#include <algorithm>
#include <cmath>

namespace chartshapes {

using chartcore::Box;
using chartcore::Point;
using PPoint = PolygonRaster::Point;
using Outline = std::vector<PPoint>;

static const std::vector<double> DASHES[4] = {{}, {6, 4}, {2, 3}, {8, 3, 2, 3}};
static const double HATCH_GAP = 5;
static const double HALF_TURN = 3.14159265358979323846;  // not PI: Arduino.h defines that

// In double, as the designer's toEighths: half away from zero.
static int32_t eighths(double px) {
  const double e = px * 8;
  return static_cast<int32_t>(e < 0 ? -std::floor(-e + 0.5) : std::floor(e + 0.5));
}

static PPoint pt(double x, double y) { return {eighths(x), eighths(y)}; }

static Outline rect(double x, double y, double w, double h) { return {pt(x, y), pt(x + w, y), pt(x + w, y + h), pt(x, y + h)}; }

static bool quad(double x1, double y1, double x2, double y2, double width, Outline& out) {
  const double dx = x2 - x1, dy = y2 - y1;
  const double len = std::hypot(dx, dy);
  if (len == 0) return false;
  const double nx = (-dy / len) * (width / 2), ny = (dx / len) * (width / 2);
  out = {pt(x1 + nx, y1 + ny), pt(x2 + nx, y2 + ny), pt(x2 - nx, y2 - ny), pt(x1 - nx, y1 - ny)};
  return true;
}

static Outline disc(double cx, double cy, double r) {
  Outline out;
  for (int i = 0; i < 16; i++) {
    const double a = i * HALF_TURN / 8;
    out.push_back(pt(cx + r * std::cos(a), cy + r * std::sin(a)));
  }
  return out;
}

static std::vector<Outline> polyline(const std::vector<Point>& points, const std::vector<double>& pattern, double width = 2) {
  std::vector<Outline> out;
  Outline q;
  if (pattern.empty()) {
    for (size_t i = 1; i < points.size(); i++)
      if (quad(points[i - 1].first, points[i - 1].second, points[i].first, points[i].second, width, q)) out.push_back(q);
    return out;
  }
  size_t k = 0;
  double left = pattern[0];
  for (size_t i = 1; i < points.size(); i++) {
    double x1 = points[i - 1].first, y1 = points[i - 1].second;
    const double x2 = points[i].first, y2 = points[i].second;
    double len = std::hypot(x2 - x1, y2 - y1);
    const double ux = len == 0 ? 0 : (x2 - x1) / len, uy = len == 0 ? 0 : (y2 - y1) / len;
    while (len > 0) {
      const double take = std::min(left, len);
      const double xe = x1 + ux * take, ye = y1 + uy * take;
      if (k % 2 == 0 && quad(x1, y1, xe, ye, 2, q)) out.push_back(q);
      x1 = xe;
      y1 = ye;
      len -= take;
      left -= take;
      if (left <= 1e-9) {
        k = (k + 1) % pattern.size();
        left = pattern[k];
      }
    }
  }
  return out;
}

// A segment clipped to a box (Liang-Barsky).
static bool clipped(double x1, double y1, double x2, double y2, const Box& b, double c[4]) {
  double t0 = 0, t1 = 1;
  const double dx = x2 - x1, dy = y2 - y1;
  const double edges[4][2] = {{-dx, x1 - b.x}, {dx, b.x + b.w - x1}, {-dy, y1 - b.y}, {dy, b.y + b.h - y1}};
  for (const auto& e : edges) {
    const double p = e[0], q = e[1];
    if (p == 0) {
      if (q < 0) return false;
      continue;
    }
    const double r = q / p;
    if (p < 0) t0 = std::max(t0, r);
    else t1 = std::min(t1, r);
    if (t0 > t1) return false;
  }
  c[0] = x1 + t0 * dx;
  c[1] = y1 + t0 * dy;
  c[2] = x1 + t1 * dx;
  c[3] = y1 + t1 * dy;
  return true;
}

static std::vector<Outline> hatched(const Box& b, int pattern) {
  std::vector<Outline> out;
  const int kind = pattern % 4;
  if (kind == 2) {
    for (double y = b.y; y < b.y + b.h; y += HATCH_GAP) out.push_back(rect(b.x, y, b.w, std::min(1.0, b.y + b.h - y)));
    return out;
  }
  Outline q;
  for (double d = -b.h; d <= b.w + b.h; d += HATCH_GAP) {
    double lines[2][4];
    int n = 0;
    if (kind == 0 || kind == 3) {
      lines[n][0] = b.x + d;
      lines[n][1] = b.y + b.h;
      lines[n][2] = b.x + d + b.h;
      lines[n][3] = b.y;
      n++;
    }
    if (kind == 1 || kind == 3) {
      lines[n][0] = b.x + d;
      lines[n][1] = b.y;
      lines[n][2] = b.x + d + b.h;
      lines[n][3] = b.y + b.h;
      n++;
    }
    for (int i = 0; i < n; i++) {
      double c[4];
      if (clipped(lines[i][0], lines[i][1], lines[i][2], lines[i][3], b, c) && quad(c[0], c[1], c[2], c[3], 1, q)) out.push_back(q);
    }
  }
  return out;
}

static std::vector<Outline> frame(const Box& b) {
  return {rect(b.x, b.y, b.w, 1), rect(b.x, b.y + b.h - 1, b.w, 1), rect(b.x, b.y, 1, b.h), rect(b.x + b.w - 1, b.y, 1, b.h)};
}

// Math.round
static double jsRound(double x) { return std::floor(x + 0.5); }

int fadeAlpha(const chartcore::Fade& f, int y) {
  if (f.y1 == f.y0) return (int)f.a1;
  const double t = std::min(1.0, std::max(0.0, (y + 0.5 - f.y0) / (f.y1 - f.y0)));
  return (int)jsRound(f.a0 + (f.a1 - f.a0) * t);
}

std::vector<Fill> chartShapes(const chartcore::Layout& l, const Look& look) {
  std::vector<Fill> out;
  const Box& plot = l.plot;
  auto add = [&](const Outline& points, const std::string& colour, int alpha, const Box* clip, const chartcore::Fade* fade = nullptr) {
    Fill f;
    f.points = points;
    f.colour = colour;
    f.alpha = alpha;
    if (clip) {
      f.hasClip = true;
      f.clip = *clip;
    }
    if (fade) {
      f.hasFade = true;
      f.fade = *fade;
    }
    out.push_back(f);
  };
  auto outline = [](const std::vector<Point>& ps) {
    Outline o;
    for (const Point& p : ps) o.push_back(pt(p.first, p.second));
    return o;
  };

  for (const chartcore::ZoneBox& z : l.zones) {
    if (look.grey)
      for (const Outline& h : hatched(z.box, 2)) add(h, z.color, 256, nullptr);
    else add(rect(z.box.x, z.box.y, z.box.w, z.box.h), z.color, 46, nullptr);
  }
  for (const chartcore::AxisTick& t : l.left.ticks) add(rect(plot.x, jsRound(t.y), plot.w, 1), look.grid, 256, nullptr);
  for (const chartcore::Labelled& m : l.midnights) add(rect(jsRound(m.x) - 1, plot.y, 2, plot.h), look.grid, 256, nullptr);

  for (const chartcore::SeriesLayout& s : l.series) {
    for (const auto& r : s.ranges) add(outline(r), s.color, 38, &plot);
    for (size_t k = 0; k < s.areas.size(); k++)
      add(outline(s.areas[k]), s.color, s.draw == "stacked" ? 154 : 77, &plot, k < s.areaFades.size() ? &s.areaFades[k] : nullptr);
    std::vector<Box> boxes = s.bands;
    boxes.insert(boxes.end(), s.bars.begin(), s.bars.end());
    for (const Box& b : boxes) {
      if (look.grey) {
        for (const Outline& h : hatched(b, s.pattern)) add(h, s.color, 256, &plot);
        for (const Outline& f : frame(b)) add(f, s.color, 256, &plot);
      } else add(rect(b.x, b.y, b.w, b.h), s.color, 256, &plot);
    }
    for (double x : s.starts) {
      const double top = (s.bands.empty() ? plot.y + plot.h : s.bands.front().y) - 2;
      add({pt(x - 4, top - 6), pt(x + 4, top - 6), pt(x, top)}, s.color, 256, &plot);
    }
    const std::vector<double>& dash = look.grey ? DASHES[s.pattern % 4] : DASHES[0];
    for (const auto& line : s.lines)
      for (const Outline& q : polyline(line, dash)) add(q, s.color, 256, &plot);
  }
  // The line at now, over the curves and under the NowPoints.
  if (!look.now.empty() && l.nowOn) add(rect(jsRound(l.nowX), plot.y, 1, plot.h), look.now, NOW_LINE_ALPHA, &plot);
  for (const chartcore::SeriesLayout& s : l.series) {
    const std::string& colour = s.nowOnStack && !look.now.empty() ? look.now : s.color;
    for (const chartcore::Mark& m : s.marks) add(disc(m.x, m.y, 3), s.color, 256, nullptr);
    if (s.hasNowPoint && look.pulse >= 0) {
      // Stronger since 2026-10-30 (lib/chart-shapes.ts pulseRing): 5 to 22 px, from 220, 3 px wide.
      const double r = 5 + 17 * look.pulse;
      const int alpha = (int)jsRound(220 * (1 - look.pulse));
      std::vector<Point> ring;
      for (int i = 0; i <= 16; i++) ring.push_back({s.nowPoint.first + r * std::cos(i * HALF_TURN / 8), s.nowPoint.second + r * std::sin(i * HALF_TURN / 8)});
      if (alpha > 0)
        for (const Outline& q : polyline(ring, DASHES[0], 3)) add(q, colour, alpha, nullptr);
    }
    if (s.hasNowPoint) add(disc(s.nowPoint.first, s.nowPoint.second, 5), colour, 256, nullptr);
  }
  for (const chartcore::LegendRow& row : l.legend) add(rect(row.swatch.x, row.swatch.y, row.swatch.w, row.swatch.h), row.color, 256, nullptr);
  return out;
}

}  // namespace chartshapes
