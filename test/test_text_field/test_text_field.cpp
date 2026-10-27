// The text field's shared rules (TextField.h), on the PC:
//
//   pio test -e native -f test_text_field
//
// The same cases as the designer's e2e/text-field.spec.ts «the rules every
// device shares», which tests lib/text-field.ts - the original this header
// is ported from.

#include <unity.h>

#include <string>

#include "project/TextField.h"

void setUp() {}
void tearDown() {}

static void lengthIsCharacters() {
  // Split after each escape: "\xBCe" would read the e as a third hex digit.
  TEST_ASSERT_EQUAL(6, textfield::textLength("Gr\xC3\xBC" "ezi"));  // Grüezi
  TEST_ASSERT_EQUAL(6, textfield::textLength("Stra\xC3\x9F" "e"));  // Straße
  TEST_ASSERT_EQUAL(1, textfield::textLength("\xF0\x9F\x98\x80"));  // one emoji, four bytes
  TEST_ASSERT_EQUAL(0, textfield::textLength(""));
}

static void clampNeverHalves() {
  TEST_ASSERT_EQUAL_STRING("Gr\xC3\xBC", textfield::clampText("Gr\xC3\xBC" "ezi", 3).c_str());
  TEST_ASSERT_EQUAL_STRING("a\xF0\x9F\x98\x80", textfield::clampText("a\xF0\x9F\x98\x80" "b", 2).c_str());
  TEST_ASSERT_EQUAL_STRING("ab", textfield::clampText("ab", 5).c_str());
}

static void boundsAndSending() {
  textfield::Bounds b = textfield::bounds(0, 255);
  TEST_ASSERT_EQUAL(0, b.minLength);
  TEST_ASSERT_EQUAL(255, b.maxLength);
  b = textfield::bounds(20, 10);
  TEST_ASSERT_EQUAL(10, b.minLength);
  TEST_ASSERT_EQUAL(10, b.maxLength);
  b = textfield::bounds(0, 999);
  TEST_ASSERT_EQUAL(255, b.maxLength);
  b = textfield::bounds(0, 0);
  TEST_ASSERT_EQUAL(1, b.maxLength);
  TEST_ASSERT_FALSE(textfield::canSend("ab", textfield::bounds(3, 10)));
  TEST_ASSERT_TRUE(textfield::canSend("abc", textfield::bounds(3, 10)));
  TEST_ASSERT_TRUE(textfield::canSend("", textfield::bounds(0, 10)));
}

static void hiddenIsDots() {
  TEST_ASSERT_EQUAL_STRING("Gr\xC3\xBC" "ezi", textfield::shownText("Gr\xC3\xBC" "ezi", false, true).c_str());
  TEST_ASSERT_EQUAL_STRING("\xE2\x80\xA2\xE2\x80\xA2\xE2\x80\xA2\xE2\x80\xA2",
                           textfield::shownText("Gr\xC3\xBC\xF0\x9F\x98\x80", true, true).c_str());
  TEST_ASSERT_EQUAL_STRING("**", textfield::shownText("ab", true, false).c_str());
}

static void geometry() {
  textfield::Geometry g = textfield::geometry(240, 48);
  TEST_ASSERT_EQUAL(9, g.radius);
  TEST_ASSERT_EQUAL(12, g.pad);
  TEST_ASSERT_EQUAL(46, g.lineY);
  TEST_ASSERT_EQUAL(2, g.lineHeight);
  g = textfield::geometry(20, 10);
  TEST_ASSERT_EQUAL(2, g.radius);
  TEST_ASSERT_EQUAL(2, g.pad);
  TEST_ASSERT_EQUAL(8, g.lineY);
  // A font taller than the field: the line still centred, as Math.trunc does.
  TEST_ASSERT_EQUAL(textfield::baseline(10, 14, 4), (10 - 18) / 2 + 14);
}

int main(int, char**) {
  UNITY_BEGIN();
  RUN_TEST(lengthIsCharacters);
  RUN_TEST(clampNeverHalves);
  RUN_TEST(boundsAndSending);
  RUN_TEST(hiddenIsDots);
  RUN_TEST(geometry);
  return UNITY_END();
}
