// PolygonRaster on the PC (pio test -e native): the energy diagram's band
// fill (designer docs/2026-10-11-energy-diagram.md; plan S2).

#include <unity.h>

#include <map>
#include <utility>
#include <vector>

#include "project/PolygonRaster.h"

using namespace PolygonRaster;
using Grid = std::map<std::pair<int, int>, int>;  // (x, y) -> coverage out of 16

static Grid raster(const std::vector<Point>& pts, int y0 = -100, int y1 = 1000) {
  Grid g;
  Sink sink;
  sink.fullRun = [&](int16_t y, int16_t x0, int16_t x1) {
    for (int x = x0; x <= x1; x++) g[{x, y}] = 16;
  };
  sink.partial = [&](int16_t y, int16_t x, uint8_t c) { g[{x, y}] = c; };
  fill(pts, (int16_t)y0, (int16_t)y1, sink);
  return g;
}

static int at(Grid& g, int x, int y) {
  auto it = g.find({x, y});
  return it == g.end() ? 0 : it->second;
}

static std::vector<Point> rect(float x0, float y0, float x1, float y1) {
  return {{toEighths(x0), toEighths(y0)}, {toEighths(x1), toEighths(y0)}, {toEighths(x1), toEighths(y1)}, {toEighths(x0), toEighths(y1)}};
}

void test_whole_pixels() {
  // A square on pixel edges covers exactly its four pixels, all the way.
  Grid g = raster(rect(1, 1, 3, 3));
  TEST_ASSERT_EQUAL(4, (int)g.size());
  for (auto& kv : g) TEST_ASSERT_EQUAL(16, kv.second);
  TEST_ASSERT_EQUAL(16, at(g, 1, 1));
  TEST_ASSERT_EQUAL(16, at(g, 2, 2));
}

void test_half_pixels() {
  // Shifted by half a pixel: corners a quarter, edges a half, the middle whole.
  Grid g = raster(rect(0.5f, 0.5f, 2.5f, 2.5f));
  TEST_ASSERT_EQUAL(4, at(g, 0, 0));
  TEST_ASSERT_EQUAL(8, at(g, 1, 0));
  TEST_ASSERT_EQUAL(8, at(g, 0, 1));
  TEST_ASSERT_EQUAL(16, at(g, 1, 1));
  TEST_ASSERT_EQUAL(4, at(g, 2, 2));
  TEST_ASSERT_EQUAL(9, (int)g.size());
}

void test_area() {
  // A triangle's coverage adds up to its area, give or take its edge.
  std::vector<Point> tri = {{toEighths(10), toEighths(10)}, {toEighths(90), toEighths(30)}, {toEighths(20), toEighths(70)}};
  Grid g = raster(tri);
  double sum = 0;
  for (auto& kv : g) sum += kv.second / 16.0;
  const double area = 0.5 * ((90 - 10) * (70 - 10) - (20 - 10) * (30 - 10));
  TEST_ASSERT_TRUE(sum > area * 0.99 && sum < area * 1.01);
}

void test_clip_rows() {
  Grid g = raster(rect(0, 0, 4, 10), 3, 4);
  for (auto& kv : g) {
    TEST_ASSERT_TRUE(kv.first.second >= 3 && kv.first.second <= 4);
  }
  TEST_ASSERT_EQUAL(8, (int)g.size());
}

void test_band_shape() {
  // A Sankey band's outline: a curved top and bottom, joined - its rows are
  // runs of whole pixels with soft ends, and nothing outside it.
  std::vector<Point> band;
  for (int i = 0; i <= 24; i++) {
    float t = i / 24.0f;
    band.push_back({toEighths(100 + 200 * t), toEighths(50 + 100 * t * t * (3 - 2 * t))});
  }
  for (int i = 24; i >= 0; i--) {
    float t = i / 24.0f;
    band.push_back({toEighths(100 + 200 * t), toEighths(90 + 100 * t * t * (3 - 2 * t))});
  }
  Grid g = raster(band);
  int full = 0, soft = 0;
  for (auto& kv : g) (kv.second == 16 ? full : soft)++;
  TEST_ASSERT_TRUE(full > 6000);
  TEST_ASSERT_TRUE(soft < full / 5);
  TEST_ASSERT_EQUAL(0, at(g, 99, 60));
}

int main(int, char**) {
  UNITY_BEGIN();
  RUN_TEST(test_whole_pixels);
  RUN_TEST(test_half_pixels);
  RUN_TEST(test_area);
  RUN_TEST(test_clip_rows);
  RUN_TEST(test_band_shape);
  return UNITY_END();
}
