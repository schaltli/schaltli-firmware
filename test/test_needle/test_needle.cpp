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

#include <cmath>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

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
  TEST_ASSERT_GREATER_OR_EQUAL_MESSAGE(9, cases.size(), "fewer vectors than the designer has");
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
    Target t;
    t.pixels = pixels.data();
    t.stride = w;
    t.width = w;
    t.height = h;
    draw(t, desc, pose);
    int32_t x0, y0, x1, y1;
    TEST_ASSERT_TRUE(bounds(desc, pose, &x0, &y0, &x1, &y1));
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

static void isqrt_is_the_floor_of_the_root() {
  const int64_t ns[] = {0, 1, 2, 3, 4, 15, 16, 17, 99, 100, 101, 1 << 20, 123456789};
  for (int64_t n : ns) TEST_ASSERT_EQUAL_INT64((int64_t)std::floor(std::sqrt((double)n)), isqrt(n));
}

static void angles_round_as_the_designer_does() {
  TEST_ASSERT_EQUAL_INT32(2880, angle64Of(45));
  TEST_ASSERT_EQUAL_INT32(-8147, angle64Of(-127.3));
  TEST_ASSERT_EQUAL_INT32(-3520, angle64Of(-55));
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
  RUN_TEST(isqrt_is_the_floor_of_the_root);
  RUN_TEST(angles_round_as_the_designer_does);
  return UNITY_END();
}
