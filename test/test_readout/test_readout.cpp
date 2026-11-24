// The shared readout vectors (schaltli-designer lib/instrument-readout/
// vectors.json) against the firmware's Readout, on the PC:
//
//   pio test -e native -f test_readout
//
// readout-vectors.json beside this file is a byte-for-byte copy of the
// designer's. With the designer checked out beside this repo, a copy that
// differs fails here - copy it again, never edit it.

#include <ArduinoJson.h>
#include <unity.h>

#include <cstdio>
#include <cstring>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

#include "project/Readout.h"

static const char* COPY = "test/test_readout/readout-vectors.json";
static const char* DESIGNER = "../schaltli-designer/lib/instrument-readout/vectors.json";

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
  auto strip = [](std::string s) {
    std::string o;
    for (char c : s)
      if (c != '\r') o += c;
    return o;
  };
  TEST_ASSERT_TRUE_MESSAGE(strip(copy) == strip(designer), "readout-vectors.json differs from the designer's - copy it again");
}

static void every_value_formats_as_the_designer_writes_it() {
  JsonArrayConst formats = doc["formats"].as<JsonArrayConst>();
  TEST_ASSERT_GREATER_OR_EQUAL_MESSAGE(16, formats.size(), "fewer formats than the designer has");
  int failures = 0;
  for (JsonObjectConst f : formats) {
    char out[readout::TEXT_MAX];
    readout::format(f["raw"] | "", f["decimals"] | 0, out);
    const std::string expected = f["expected"] | "";
    if (expected != out) {
      failures++;
      std::printf("FAIL format(\"%s\", %d): \"%s\", the designer \"%s\"\n", f["raw"] | "", f["decimals"] | 0, out, expected.c_str());
    }
  }
  TEST_ASSERT_EQUAL_INT_MESSAGE(0, failures, "formats failed, see above");
}

static void read_strip(readout::Description& d, std::vector<uint8_t>& alpha, int& w, int& h) {
  JsonObjectConst s = doc["strip"].as<JsonObjectConst>();
  d.chars = s["chars"] | "";
  d.cells.clear();
  for (JsonArrayConst c : s["cells"].as<JsonArrayConst>()) {
    readout::Cell cell;
    cell.sx = c[0] | 0;
    cell.w = c[1] | 0;
    cell.advance = c[2] | 0;
    d.cells.push_back(cell);
  }
  d.pad = s["pad"] | 0;
  d.height = s["height"] | 0;
  w = s["width"] | 0;
  h = s["height"] | 0;
  alpha.clear();
  for (JsonVariantConst a : s["alpha"].as<JsonArrayConst>()) alpha.push_back((uint8_t)a.as<int>());
}

static int drawCase(JsonObjectConst c, int clipX0, int clipY0, int clipX1, int clipY1, std::vector<uint16_t>& pixels) {
  readout::Description d;
  std::vector<uint8_t> alpha;
  int sw = 0, sh = 0;
  read_strip(d, alpha, sw, sh);
  d.x = c["x"] | 0;
  d.y = c["y"] | 0;
  d.center = strcmp(c["align"] | "right", "center") == 0;
  d.color = (uint16_t)(c["color"] | 0);
  const int w = c["w"] | 0, h = c["h"] | 0;
  pixels.assign((size_t)w * h, (uint16_t)(c["ground"] | 0));
  needleraster::Target t;
  t.pixels = pixels.data();
  t.stride = w;
  t.width = w;
  t.height = h;
  t.clipX0 = clipX0;
  t.clipY0 = clipY0;
  t.clipX1 = clipX1;
  t.clipY1 = clipY1;
  readout::draw(t, d, alpha.data(), sw, sh, c["text"] | "", c["opacity"] | 256);
  return w;
}

static void every_draw_is_the_designers_picture() {
  JsonArrayConst draws = doc["draws"].as<JsonArrayConst>();
  TEST_ASSERT_GREATER_OR_EQUAL_MESSAGE(4, draws.size(), "fewer pictures than the designer has");
  int failures = 0;
  for (JsonObjectConst c : draws) {
    std::vector<uint16_t> pixels;
    const int w = drawCase(c, -1, -1, -1, -1, pixels);
    const std::string expected = c["expected"].as<std::string>();
    int diff = 0, firstX = -1, firstY = -1;
    for (size_t i = 0; i < pixels.size(); i++) {
      char hex[5];
      std::snprintf(hex, sizeof hex, "%04x", pixels[i]);
      if (expected.compare(i * 4, 4, hex) != 0) {
        if (diff == 0) {
          firstX = (int)i % w;
          firstY = (int)i / w;
        }
        diff++;
      }
    }
    if (diff) {
      failures++;
      std::printf("FAIL %s: %d pixels differ, first at (%d, %d)\n", c["name"].as<const char*>(), diff, firstX, firstY);
    }
  }
  TEST_ASSERT_EQUAL_INT_MESSAGE(0, failures, "readout pictures failed, see above");
}

// A region redrawn on its own (a needle's box while it swings): inside the
// clip the pixels of the whole picture, outside it nothing touched.
static void a_clip_draws_its_part_of_the_picture() {
  int failures = 0;
  for (JsonObjectConst c : doc["draws"].as<JsonArrayConst>()) {
    std::vector<uint16_t> whole, part;
    const int w = drawCase(c, -1, -1, -1, -1, whole);
    const int h = (int)whole.size() / w;
    const int x0 = w / 3, y0 = h / 4, x1 = w - w / 4, y1 = h - 2;
    drawCase(c, x0, y0, x1, y1, part);
    const uint16_t ground = (uint16_t)(c["ground"] | 0);
    for (int y = 0; y < h; y++)
      for (int x = 0; x < w; x++) {
        const bool inside = x >= x0 && x < x1 && y >= y0 && y < y1;
        const uint16_t want = inside ? whole[(size_t)y * w + x] : ground;
        if (part[(size_t)y * w + x] != want) failures++;
      }
  }
  TEST_ASSERT_EQUAL_INT_MESSAGE(0, failures, "a clipped readout differs from the whole one");
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
  RUN_TEST(every_value_formats_as_the_designer_writes_it);
  RUN_TEST(every_draw_is_the_designers_picture);
  RUN_TEST(a_clip_draws_its_part_of_the_picture);
  return UNITY_END();
}
