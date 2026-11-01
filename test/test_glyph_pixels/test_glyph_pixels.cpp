// GlyphPixels on the PC (pio test -e native): how a BDF row's hex digits
// become pixels, and how a 4-bit font's coverage is blended in RGB565
// (designer docs/2026-11-01-4bit-fonts.md; tasks/4bit-fonts-todo.md T3).

#include <unity.h>

#include <ArduinoJson.h>

#include "project/GlyphPixels.h"
#include "blend565_cases.h"

using namespace glyphpixels;

static uint16_t pack(JsonObjectConst c) {
  return (uint16_t)(((c["r"].as<int>() & 31) << 11) | ((c["g"].as<int>() & 63) << 5) | (c["b"].as<int>() & 31));
}

void test_blend_matches_the_designers_cases() {
  JsonDocument doc;
  TEST_ASSERT_FALSE(deserializeJson(doc, BLEND565_CASES));
  JsonArrayConst cases = doc.as<JsonArrayConst>();
  TEST_ASSERT_TRUE(cases.size() >= 10);
  for (JsonObjectConst c : cases) {
    const uint16_t got = blendCoverage565(pack(c["under"]), pack(c["ink"]), (uint8_t)c["coverage"].as<int>());
    TEST_ASSERT_EQUAL_HEX16_MESSAGE(pack(c["out"]), got, c["note"].as<const char*>());
  }
}

void test_coverage_reads_digits_left_to_right() {
  const char* row = "0F7A";
  uint32_t words[1];
  parseHexRow(row, 4, words);
  TEST_ASSERT_EQUAL_UINT8(0, rowCoverage(words, 1, 4, 0));
  TEST_ASSERT_EQUAL_UINT8(15, rowCoverage(words, 1, 4, 1));
  TEST_ASSERT_EQUAL_UINT8(7, rowCoverage(words, 1, 4, 2));
  TEST_ASSERT_EQUAL_UINT8(10, rowCoverage(words, 1, 4, 3));
  // Outside the row: nothing.
  TEST_ASSERT_EQUAL_UINT8(0, rowCoverage(words, 1, 4, -1));
  TEST_ASSERT_EQUAL_UINT8(0, rowCoverage(words, 1, 4, 4));
}

void test_coverage_across_words() {
  // Eleven pixels: two words, word 0 holding the rightmost eight digits.
  const char* row = "123456789AB";
  uint32_t words[2];
  TEST_ASSERT_EQUAL_UINT16(2, wordsFor(11));
  parseHexRow(row, 11, words);
  for (int i = 0; i < 11; i++) TEST_ASSERT_EQUAL_UINT8(i + 1, rowCoverage(words, 2, 11, i));
}

void test_one_bit_rows_unchanged() {
  // "C0" is the 1-bit row 1100 0000: bits counted from the right end.
  uint32_t words[1];
  parseHexRow("C0", 2, words);
  TEST_ASSERT_TRUE(rowBit(words, 1, 7));
  TEST_ASSERT_TRUE(rowBit(words, 1, 6));
  TEST_ASSERT_FALSE(rowBit(words, 1, 5));
  TEST_ASSERT_FALSE(rowBit(words, 1, 8));
}

void test_blend_ends() {
  TEST_ASSERT_EQUAL_HEX16(0x1234, blendCoverage565(0x1234, 0xFFFF, 0));
  TEST_ASSERT_EQUAL_HEX16(0xFFFF, blendCoverage565(0x1234, 0xFFFF, 15));
}

int main(int, char**) {
  UNITY_BEGIN();
  RUN_TEST(test_blend_matches_the_designers_cases);
  RUN_TEST(test_coverage_reads_digits_left_to_right);
  RUN_TEST(test_coverage_across_words);
  RUN_TEST(test_one_bit_rows_unchanged);
  RUN_TEST(test_blend_ends);
  return UNITY_END();
}
