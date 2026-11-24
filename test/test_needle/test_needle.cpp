// The shared needle raster vectors (schaltli-designer
// lib/needle-raster/vectors.json) against the firmware's NeedleRaster, on
// the PC:
//
//   pio test -e native -f test_needle
//
// needle-raster-vectors.json beside this file is a byte-for-byte copy of
// the designer's. With the designer checked out beside this repo, a copy
// that differs fails here - copy it again, never edit it.

#include <ArduinoJson.h>
#include <unity.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

#include "project/ArcSinTable.h"
#include "project/NeedleRaster.h"
#include "project/NeedleRasterJson.h"

using namespace needleraster;

static const char* COPY = "test/test_needle/needle-raster-vectors.json";
static const char* DESIGNER = "../schaltli-designer/lib/needle-raster/vectors.json";

static bool readFile(const char* path, std::string& out) {
  std::ifstream in(path, std::ios::binary);
  if (!in) return false;
  std::ostringstream buffer;
  buffer << in.rdbuf();
  out = buffer.str();
  return true;
}

static JsonDocument doc;

void setUp() {}
void tearDown() {}

static void the_copy_is_the_designers() {
  std::string copy, designer;
  TEST_ASSERT_TRUE_MESSAGE(readFile(COPY, copy), "the vectors' copy is missing");
  if (!readFile(DESIGNER, designer)) {
    TEST_IGNORE_MESSAGE("schaltli-designer not checked out beside this repo: the copy is not compared");
    return;
  }
  // Line endings are git's business, not the vectors'.
  auto strip = [](std::string s) {
    std::string o;
    for (char c : s)
      if (c != '\r') o += c;
    return o;
  };
  TEST_ASSERT_TRUE_MESSAGE(strip(copy) == strip(designer), "needle-raster-vectors.json differs from the designer's - copy it again");
}

static void every_vector_draws_the_designers_picture() {
  JsonArrayConst cases = doc["cases"].as<JsonArrayConst>();
  TEST_ASSERT_GREATER_OR_EQUAL_MESSAGE(26, cases.size(), "fewer vectors than the designer has");
  int failures = 0;
  for (JsonObjectConst c : cases) {
    const int w = c["width"] | 0, h = c["height"] | 0;
    std::vector<uint16_t> pixels((size_t)w * h, (uint16_t)(c["ground"] | 0));
    Description desc;
    parseDescription(c["desc"].as<JsonObjectConst>(), desc);
    Pose pose;
    pose.angle64 = c["pose"]["angle64"] | 0;
    pose.shiftX = c["pose"]["shiftX"] | 0;
    pose.shiftY = c["pose"]["shiftY"] | 0;
    pose.level = c["pose"]["level"] | 0;
    Target t;
    t.pixels = pixels.data();
    t.stride = w;
    t.width = w;
    t.height = h;
    draw(t, desc, pose);
    const std::string expected = c["expected"].as<std::string>();
    int diff = 0, firstX = -1, firstY = -1;
    for (int i = 0; i < w * h; i++) {
      char hex[5];
      std::snprintf(hex, sizeof hex, "%04x", pixels[i]);
      if (expected.compare(i * 4, 4, hex) != 0) {
        if (diff == 0) {
          firstX = i % w;
          firstY = i / w;
        }
        diff++;
      }
    }
    if (diff) {
      failures++;
      std::printf("FAIL %s: %d pixels differ, first at (%d, %d)\n", c["name"].as<const char*>(), diff, firstX, firstY);
    }
  }
  TEST_ASSERT_EQUAL_INT_MESSAGE(0, failures, "needle vectors failed, see above");
}

// What a board redraws while a needle swings (InstrumentMotion): every pixel
// the needle changes lies inside bounds().
static void bounds_hold_every_pixel_drawn() {
  int failures = 0;
  for (JsonObjectConst c : doc["cases"].as<JsonArrayConst>()) {
    const int w = c["width"] | 0, h = c["height"] | 0;
    const uint16_t ground = (uint16_t)(c["ground"] | 0);
    std::vector<uint16_t> pixels((size_t)w * h, ground);
    Description desc;
    parseDescription(c["desc"].as<JsonObjectConst>(), desc);
    Pose pose;
    pose.angle64 = c["pose"]["angle64"] | 0;
    pose.shiftX = c["pose"]["shiftX"] | 0;
    pose.shiftY = c["pose"]["shiftY"] | 0;
    pose.level = c["pose"]["level"] | 0;
    Target t;
    t.pixels = pixels.data();
    t.stride = w;
    t.width = w;
    t.height = h;
    draw(t, desc, pose);
    int32_t x0, y0, x1, y1;
    if (!bounds(desc, pose, &x0, &y0, &x1, &y1)) {
      // No box (only empty fills): nothing may be drawn at all.
      x0 = y0 = 0;
      x1 = y1 = -1;
    }
    for (int y = 0; y < h; y++)
      for (int x = 0; x < w; x++)
        if (pixels[(size_t)y * w + x] != ground && (x < x0 || x > x1 || y < y0 || y > y1)) {
          failures++;
          std::printf("FAIL %s: (%d, %d) drawn outside bounds %d,%d..%d,%d\n", c["name"].as<const char*>(), x, y, x0, y0, x1, y1);
          y = h;
          break;
        }
  }
  TEST_ASSERT_EQUAL_INT_MESSAGE(0, failures, "a needle drew outside its bounds, see above");
}

// A fill layer's box follows the pose: a rect its grown part, an arc the
// circle box round the pivot; an empty fill has none.
static void a_fills_bounds_follow_the_pose() {
  Description desc;
  Layer layer;
  layer.motion = Motion::Fill;
  Shape rect;
  rect.kind = Kind::Rect;
  rect.x0 = 16;
  rect.y0 = 16;
  rect.x1 = 96;
  rect.y1 = 48;
  layer.shapes.push_back(rect);
  desc.layers.push_back(layer);
  Pose pose;
  pose.shiftX = 40;  // grown to x = 56
  int32_t x0, y0, x1, y1;
  TEST_ASSERT_TRUE(bounds(desc, pose, &x0, &y0, &x1, &y1));
  // (16 - 8) / 8 - 1, (56 + 8) / 8 + 1: a body reaches a pixel, plus one.
  TEST_ASSERT_EQUAL_INT32(0, x0);
  TEST_ASSERT_EQUAL_INT32(0, y0);
  TEST_ASSERT_EQUAL_INT32(9, x1);
  TEST_ASSERT_EQUAL_INT32(8, y1);
  pose.shiftX = 0;
  TEST_ASSERT_FALSE_MESSAGE(bounds(desc, pose, &x0, &y0, &x1, &y1), "an empty fill rect has a box");

  Description ring;
  ring.pivotX = 100;
  ring.pivotY = 100;
  Layer arcLayer;
  arcLayer.motion = Motion::Fill;
  arcLayer.soft = {130, 87, 43};
  Shape arc;
  arc.kind = Kind::Arc;
  arc.r = 70;
  arc.hw = 10;
  arc.from64 = -3840;
  arc.to64 = 7680;
  arcLayer.shapes.push_back(arc);
  ring.layers.push_back(arcLayer);
  Pose angle;
  angle.angle64 = 0;
  TEST_ASSERT_TRUE(bounds(ring, angle, &x0, &y0, &x1, &y1));
  // r + hw = 80 round the pivot, the soft reach 3 * 8 + 8, plus one.
  TEST_ASSERT_EQUAL_INT32(floor((100 - 80 - 32) / 8.0) - 1, x0);
  TEST_ASSERT_EQUAL_INT32(floor((100 - 80 - 32) / 8.0) - 1, y0);
  TEST_ASSERT_EQUAL_INT32((100 + 80 + 32) / 8 + 1, x1);
  TEST_ASSERT_EQUAL_INT32((100 + 80 + 32) / 8 + 1, y1);
  angle.angle64 = -3840;
  TEST_ASSERT_FALSE_MESSAGE(bounds(ring, angle, &x0, &y0, &x1, &y1), "an arc of no span has a box");
}

// The designer's arcDirection(): (sin, -cos) x 16384 from the shared table.
static void directionOf(int32_t angle64, int32_t* x, int32_t* y) {
  int32_t a = angle64 % (360 * 64);
  if (a < 0) a += 360 * 64;
  const int32_t deg = a / 64, frac = a - deg * 64;
  const int32_t s = (ARC_SIN_TABLE[deg] * (64 - frac) + ARC_SIN_TABLE[(deg + 1) % 360] * frac + 32) >> 6;
  const int32_t c = (ARC_SIN_TABLE[(deg + 90) % 360] * (64 - frac) + ARC_SIN_TABLE[(deg + 91) % 360] * frac + 32) >> 6;
  *x = s;
  *y = -c;
}

static uint16_t blendRef(uint16_t d, uint16_t c, int32_t a) {
  if (a >= 256) return c;
  const int32_t r = (((d >> 11) & 31) * (256 - a) + ((c >> 11) & 31) * a + 128) >> 8;
  const int32_t g = (((d >> 5) & 63) * (256 - a) + ((c >> 5) & 63) * a + 128) >> 8;
  const int32_t b = ((d & 31) * (256 - a) + (c & 31) * a + 128) >> 8;
  return (uint16_t)((r << 11) | (g << 5) | b);
}

// The arc as the designer writes it, literally: every pixel of the picture,
// every sub-sample, the soft field rooted with isqrt(). The raster walks
// only the band and finds the soft level without a root; on arcs the vectors
// do not reach (a band wider than its radius, spans past 180 degrees, whole
// rings, end caps reaching into the hole, a clip) both must still agree
// pixel for pixel.
static void arcs_off_the_vectors_draw_as_the_designer_writes_them() {
  uint32_t seed = 12345;
  auto rnd = [&](int32_t n) {
    seed = seed * 1103515245u + 12345u;
    return (int32_t)((seed >> 8) % (uint32_t)n);
  };
  int failures = 0;
  for (int round = 0; round < 400; round++) {
    const int w = 20 + rnd(30), h = 20 + rnd(30);
    const uint16_t ground = (uint16_t)rnd(65536);
    Description desc;
    desc.pivotX = rnd(w * 8);
    desc.pivotY = rnd(h * 8);
    Layer layer;
    layer.motion = rnd(2) ? Motion::Fill : Motion::Static;
    layer.color = (uint16_t)rnd(65536);
    layer.alpha = rnd(3) == 0 ? 256 : 1 + rnd(255);
    layer.dx = rnd(17) - 8;
    layer.dy = rnd(17) - 8;
    if (rnd(2)) {
      const int levels = rnd(6);
      for (int i = 0; i <= levels; i++) layer.soft.push_back(1 + rnd(256));
    }
    if (rnd(2)) {
      layer.gradient.on = true;
      layer.gradient.to = (uint16_t)rnd(65536);
      layer.gradient.alongY = rnd(2);
      layer.gradient.start = rnd(10);
      layer.gradient.length = 1 + rnd(30);
    }
    Shape arc;
    arc.kind = Kind::Arc;
    arc.r = rnd(150);
    arc.hw = rnd(4) == 0 ? arc.r + rnd(30) : rnd(30);
    arc.from64 = rnd(360 * 64 * 2) - 360 * 64;
    arc.to64 = arc.from64 + (rnd(5) == 0 ? 360 * 64 + rnd(100) : rnd(360 * 64));
    layer.shapes.push_back(arc);
    desc.layers.push_back(layer);
    Pose pose;
    pose.angle64 = arc.from64 - 200 + rnd(arc.to64 - arc.from64 + 400);

    std::vector<uint16_t> got((size_t)w * h, ground);
    Target t;
    t.pixels = got.data();
    t.stride = w;
    t.width = w;
    t.height = h;
    const bool clipped = rnd(3) == 0;
    if (clipped) {
      t.clipX0 = rnd(w / 2);
      t.clipY0 = rnd(h / 2);
      t.clipX1 = w / 2 + rnd(w / 2);
      t.clipY1 = h / 2 + rnd(h / 2);
    }
    draw(t, desc, pose);

    std::vector<uint16_t> want((size_t)w * h, ground);
    const int32_t end = layer.motion == Motion::Fill ? std::max(arc.from64, std::min(arc.to64, pose.angle64)) : arc.to64;
    const int32_t span = end - arc.from64;
    if (span > 0) {
      int32_t u0x, u0y, u1x, u1y;
      directionOf(arc.from64, &u0x, &u0y);
      directionOf(end, &u1x, &u1y);
      const int32_t cx = desc.pivotX + layer.dx, cy = desc.pivotY + layer.dy;
      const int64_t e0x = (arc.r * u0x + 8192) >> 14, e0y = (arc.r * u0y + 8192) >> 14;
      const int64_t e1x = (arc.r * u1x + 8192) >> 14, e1y = (arc.r * u1y + 8192) >> 14;
      auto cross = [](int64_t ax, int64_t ay, int64_t bx, int64_t by) { return ax * by - ay * bx; };
      auto inSector = [&](int64_t x, int64_t y) {
        if (span >= 360 * 64) return true;
        if (span <= 180 * 64) return cross(u0x, u0y, x, y) >= 0 && cross(x, y, u1x, u1y) >= 0;
        return !(cross(u1x, u1y, x, y) > 0 && cross(x, y, u0x, u0y) > 0);
      };
      auto endD2 = [&](int64_t x, int64_t y) {
        const int64_t d0 = (x - e0x) * (x - e0x) + (y - e0y) * (y - e0y);
        const int64_t d1 = (x - e1x) * (x - e1x) + (y - e1y) * (y - e1y);
        return d0 < d1 ? d0 : d1;
      };
      auto colourAt = [&](int32_t x, int32_t y) -> uint16_t {
        if (!layer.gradient.on) return layer.color;
        const Gradient& g = layer.gradient;
        const int32_t steps = g.length, step = (g.alongY ? y : x) - g.start;
        const int32_t st = step < 0 ? 0 : (step > steps - 1 ? steps - 1 : step);
        const int32_t ww = 2 * st + 1, n = 2 * steps;
        auto at = [&](int32_t a, int32_t b) { return (a * (n - ww) + b * ww + steps) / n; };
        const uint16_t c = layer.color, d = g.to;
        return (uint16_t)((at((c >> 11) & 31, (d >> 11) & 31) << 11) | (at((c >> 5) & 63, (d >> 5) & 63) << 5) |
                          at(c & 31, d & 31));
      };
      const int32_t cX0 = clipped ? t.clipX0 : 0, cY0 = clipped ? t.clipY0 : 0;
      const int32_t cX1 = clipped ? t.clipX1 : w, cY1 = clipped ? t.clipY1 : h;
      auto put = [&](int32_t x, int32_t y, int32_t a) {
        if (a <= 0 || x < cX0 || y < cY0 || x >= cX1 || y >= cY1) return;
        uint16_t& px = want[(size_t)y * w + x];
        px = blendRef(px, colourAt(x, y), a);
      };
      for (int32_t y = 0; y < h; y++)
        for (int32_t x = 0; x < w; x++) {
          if (!layer.soft.empty()) {
            const int32_t levels = (int32_t)layer.soft.size() - 1;
            const int64_t px = x * 8 + 4 - cx, py = y * 8 + 4 - cy;
            int64_t centre;
            if (inSector(px, py)) {
              centre = isqrt(px * px + py * py) - arc.r;
              if (centre < 0) centre = -centre;
            } else {
              centre = isqrt(endD2(px, py));
            }
            const int64_t e = centre - arc.hw;
            const int64_t d2 = e > 0 ? e * e : 0;
            int32_t k = 0;
            if (d2 > 0) {
              k = levels + 1;
              for (int32_t j = 1; j <= levels; j++)
                if (d2 < (int64_t)(j * 8) * (j * 8)) {
                  k = j;
                  break;
                }
            }
            if (k <= levels) put(x, y, layer.soft[k]);
          } else {
            const int64_t lo = arc.r - arc.hw, hi = arc.r + arc.hw;
            int32_t cov = 0;
            for (int sy = 0; sy < 4; sy++)
              for (int sx = 0; sx < 4; sx++) {
                const int64_t px = x * 8 + 1 + sx * 2 - cx, py = y * 8 + 1 + sy * 2 - cy;
                bool in;
                if (inSector(px, py)) {
                  const int64_t d2 = px * px + py * py;
                  in = d2 >= lo * lo && d2 <= hi * hi;
                } else {
                  in = endD2(px, py) <= (int64_t)arc.hw * arc.hw;
                }
                if (in) cov++;
              }
            const int32_t a = std::max(0, std::min(256, (int)layer.alpha));
            if (cov > 0) put(x, y, cov == 16 ? a : (a * cov + 8) >> 4);
          }
        }
    }
    if (got != want) {
      failures++;
      if (failures <= 5)
        std::printf("FAIL round %d: r %d hw %d from %d to %d angle %d soft %d clip %d\n", round, (int)arc.r, (int)arc.hw,
                    (int)arc.from64, (int)arc.to64, (int)pose.angle64, (int)layer.soft.size(), (int)clipped);
    }
  }
  TEST_ASSERT_EQUAL_INT_MESSAGE(0, failures, "arcs differ from the literal reading, see above");
}

static void isqrt_is_the_floor_of_the_root() {
  const int64_t ns[] = {0, 1, 2, 3, 4, 15, 16, 17, 99, 100, 101, 1 << 20, 123456789};
  for (int64_t n : ns) TEST_ASSERT_EQUAL_INT64((int64_t)std::floor(std::sqrt((double)n)), isqrt(n));
}

static void angles_round_as_the_designer_does() {
  TEST_ASSERT_EQUAL_INT32(2880, angle64Of(45));
  TEST_ASSERT_EQUAL_INT32(-8147, angle64Of(-127.3));
  TEST_ASSERT_EQUAL_INT32(-3520, angle64Of(-55));
}

static void levels_round_as_the_designer_does() {
  TEST_ASSERT_EQUAL_INT32(0, levelOf(-0.3));
  TEST_ASSERT_EQUAL_INT32(0, levelOf(0));
  TEST_ASSERT_EQUAL_INT32(2048, levelOf(0.5));
  TEST_ASSERT_EQUAL_INT32(1516, levelOf(0.37));
  TEST_ASSERT_EQUAL_INT32(4096, levelOf(1));
  TEST_ASSERT_EQUAL_INT32(4096, levelOf(1.025));
}

int main(int, char**) {
  std::string json;
  if (!readFile(COPY, json)) {
    std::printf("missing %s\n", COPY);
    return 1;
  }
  deserializeJson(doc, json);
  UNITY_BEGIN();
  RUN_TEST(the_copy_is_the_designers);
  RUN_TEST(every_vector_draws_the_designers_picture);
  RUN_TEST(bounds_hold_every_pixel_drawn);
  RUN_TEST(a_fills_bounds_follow_the_pose);
  RUN_TEST(arcs_off_the_vectors_draw_as_the_designer_writes_them);
  RUN_TEST(isqrt_is_the_floor_of_the_root);
  RUN_TEST(angles_round_as_the_designer_does);
  RUN_TEST(levels_round_as_the_designer_does);
  return UNITY_END();
}
