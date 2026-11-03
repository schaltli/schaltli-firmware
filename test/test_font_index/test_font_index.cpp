// FontIndex on the PC (pio test -e native): which font draws an id, with the
// colour boards' `replaces` (designer docs/2026-11-01-4bit-fonts.md, T8).
//
// While an old 1-bit font is still declared, a project naming it draws it as
// it was made. Once a DDF drops it - T14 does, for the colour boards - the
// 4-bit font replacing it draws instead, so a project still on the board
// from before keeps every text until the next deploy brings it migrated.

#include <unity.h>

#include <ArduinoJson.h>
#include <cstdio>
#include <string>

#include "project/FontIndex.h"

using Index = fontindex::Index<std::string>;

static const char* SMALL = R"({"fonts":[
  {"id":"font-old","file":"fonts/old.bdf"},
  {"id":"font-new","file":"fonts/new.bdf","bpp":4,"replaces":["font-old","font-gone"]},
  {"id":"font-other","file":"fonts/other.bdf","bpp":4,"replaces":["font-gone"]},
  {"id":"","file":"fonts/nameless.bdf"}
]})";

void test_declared_draws_as_itself() {
  JsonDocument doc;
  TEST_ASSERT_FALSE(deserializeJson(doc, SMALL));
  Index index;
  fontindex::read(doc["fonts"].as<JsonArrayConst>(), index);
  TEST_ASSERT_EQUAL_STRING("font-old", fontindex::resolve(index, std::string("font-old")).c_str());
  TEST_ASSERT_EQUAL_STRING("font-new", fontindex::resolve(index, std::string("font-new")).c_str());
  TEST_ASSERT_EQUAL_STRING("font-old", index.firstId.c_str());
  TEST_ASSERT_EQUAL(3, (int)index.fileById.size());
}

void test_dropped_draws_as_its_replacement_the_first_claim_wins() {
  JsonDocument doc;
  TEST_ASSERT_FALSE(deserializeJson(doc, SMALL));
  Index index;
  fontindex::read(doc["fonts"].as<JsonArrayConst>(), index);
  TEST_ASSERT_EQUAL_STRING("font-new", fontindex::resolve(index, std::string("font-gone")).c_str());
  TEST_ASSERT_EQUAL_STRING("", fontindex::resolve(index, std::string("font-never")).c_str());
}

// The boards' own device.json, as the build tool writes it since T14: no
// 1-bit font left, and every id the 4-bit fonts' `replaces` name - 30 and
// more, the 1-bit fonts the board carried - draws as the 4-bit font that
// names it first. Where the PaperS3, which keeps its 1-bit fonts, declares
// the same id, the replacement has that font's weight.
static std::string readFile(const char* path) {
  FILE* f = fopen(path, "rb");
  TEST_ASSERT_NOT_NULL_MESSAGE(f, path);
  std::string json;
  char buf[4096];
  size_t n;
  while ((n = fread(buf, 1, sizeof buf, f)) > 0) json.append(buf, n);
  fclose(f);
  return json;
}

static void board(const char* path) {
  JsonDocument doc;
  TEST_ASSERT_FALSE(deserializeJson(doc, readFile(path)));
  JsonArrayConst fonts = doc["fonts"].as<JsonArrayConst>();
  JsonDocument paper;
  TEST_ASSERT_FALSE(deserializeJson(paper, readFile("ddf-source-papers3/device.json")));

  Index index;
  fontindex::read(fonts, index);
  int replaced = 0;
  for (JsonObjectConst font : fonts) {
    TEST_ASSERT_EQUAL_INT_MESSAGE(4, font["bpp"] | 1, (std::string(path) + ": " + (font["id"] | "") + " is not 4-bit").c_str());
    for (JsonVariantConst old : font["replaces"].as<JsonArrayConst>()) {
      const std::string id = old | "";
      replaced++;
      const std::string by = fontindex::resolve(index, id);
      TEST_ASSERT_FALSE_MESSAGE(by.empty(), (std::string(path) + ": " + id + " has no replacement").c_str());
      std::string byWeight;
      for (JsonObjectConst f4 : fonts)
        if (by == (f4["id"] | "")) byWeight = f4["weight"] | "regular";
      for (JsonObjectConst f1 : paper["fonts"].as<JsonArrayConst>())
        if (id == (f1["id"] | "")) TEST_ASSERT_EQUAL_STRING_MESSAGE(f1["weight"] | "regular", byWeight.c_str(), id.c_str());
    }
  }
  TEST_ASSERT_TRUE_MESSAGE(replaced >= 30, path);
}

void test_the_knob() { board("ddf-source/device.json"); }
void test_the_4_inch() { board("ddf-source-waveshare4/device.json"); }
void test_the_4v3b() { board("ddf-source-waveshare4v3b/device.json"); }

int main(int, char**) {
  UNITY_BEGIN();
  RUN_TEST(test_declared_draws_as_itself);
  RUN_TEST(test_dropped_draws_as_its_replacement_the_first_claim_wins);
  RUN_TEST(test_the_knob);
  RUN_TEST(test_the_4_inch);
  RUN_TEST(test_the_4v3b);
  return UNITY_END();
}
