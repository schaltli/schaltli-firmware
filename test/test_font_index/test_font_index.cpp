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

// The boards' own device.json, as the build tool writes it: every 1-bit font
// draws as itself; with the 1-bit fonts dropped, as T14 will, each draws as
// a 4-bit font of its own weight.
static void board(const char* path) {
  FILE* f = fopen(path, "rb");
  TEST_ASSERT_NOT_NULL_MESSAGE(f, path);
  std::string json;
  char buf[4096];
  size_t n;
  while ((n = fread(buf, 1, sizeof buf, f)) > 0) json.append(buf, n);
  fclose(f);
  JsonDocument doc;
  TEST_ASSERT_FALSE(deserializeJson(doc, json));
  JsonArrayConst fonts = doc["fonts"].as<JsonArrayConst>();

  Index all;
  fontindex::read(fonts, all);
  JsonDocument fourOnly;
  JsonArray kept = fourOnly["fonts"].to<JsonArray>();
  int old = 0;
  for (JsonObjectConst font : fonts) {
    if ((font["bpp"] | 1) == 4) kept.add(font);
  }
  Index dropped;
  fontindex::read(fourOnly["fonts"].as<JsonArrayConst>(), dropped);

  for (JsonObjectConst font : fonts) {
    if ((font["bpp"] | 1) == 4) continue;
    old++;
    const std::string id = font["id"] | "";
    TEST_ASSERT_EQUAL_STRING_MESSAGE(id.c_str(), fontindex::resolve(all, id).c_str(), path);
    const std::string by = fontindex::resolve(dropped, id);
    TEST_ASSERT_FALSE_MESSAGE(by.empty(), (std::string(path) + ": " + id + " has no replacement").c_str());
    for (JsonObjectConst f4 : fonts) {
      if (by != (f4["id"] | "")) continue;
      TEST_ASSERT_EQUAL_STRING_MESSAGE(font["weight"] | "regular", f4["weight"] | "regular", id.c_str());
    }
  }
  TEST_ASSERT_TRUE_MESSAGE(old >= 30, path);
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
