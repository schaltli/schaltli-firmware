// The popup fence's geometry and the dimming (src/project/PopupFence.h), on the
// PC:
//
//   pio test -e native
//
// The numbers are the designer's popupFence for the Knob (360x360 round) and
// the 4.3B (800x480), e2e/popup-screens.spec.ts.

#include <unity.h>

#include "project/PopupFence.h"

void setUp() {}
void tearDown() {}

static PopupFence knob() {
  PopupFence f;
  f.present = true;
  f.circle = true;
  f.x = 19;
  f.y = 19;
  f.width = 322;
  f.height = 322;
  return f;
}

static PopupFence board43b() {
  PopupFence f;
  f.present = true;
  f.x = 42;
  f.y = 26;
  f.width = 716;
  f.height = 429;
  return f;
}

static void a_circle_holds_its_centre_and_its_edge_but_not_its_square_corner() {
  const PopupFence f = knob();
  TEST_ASSERT_TRUE(f.contains(180, 180));
  TEST_ASSERT_TRUE(f.contains(19, 180));
  TEST_ASSERT_TRUE(f.contains(180, 340));
  TEST_ASSERT_FALSE(f.contains(18, 180));
  TEST_ASSERT_FALSE(f.contains(23, 23));
  TEST_ASSERT_FALSE(f.contains(8, 180));
}

static void a_rectangle_holds_its_first_pixel_and_not_one_past_its_last() {
  const PopupFence f = board43b();
  TEST_ASSERT_TRUE(f.contains(42, 26));
  TEST_ASSERT_TRUE(f.contains(757, 454));
  TEST_ASSERT_FALSE(f.contains(758, 454));
  TEST_ASSERT_FALSE(f.contains(41, 100));
  TEST_ASSERT_FALSE(f.contains(400, 455));
}

static void no_fence_holds_nothing() {
  PopupFence f;
  TEST_ASSERT_FALSE(f.contains(0, 0));
}

static void halving_keeps_black_and_makes_white_mid_grey() {
  TEST_ASSERT_EQUAL_HEX16(0x0000, halveRgb565(0x0000));
  TEST_ASSERT_EQUAL_HEX16(0x7BEF, halveRgb565(0xFFFF));
  TEST_ASSERT_EQUAL_HEX16(0x7800, halveRgb565(0xF800));  // red 31 -> 15
  TEST_ASSERT_EQUAL_HEX16(0x03E0, halveRgb565(0x07E0));  // green 63 -> 31
  TEST_ASSERT_EQUAL_HEX16(0x000F, halveRgb565(0x001F));  // blue 31 -> 15
}

int main(int, char**) {
  UNITY_BEGIN();
  RUN_TEST(a_circle_holds_its_centre_and_its_edge_but_not_its_square_corner);
  RUN_TEST(a_rectangle_holds_its_first_pixel_and_not_one_past_its_last);
  RUN_TEST(no_fence_holds_nothing);
  RUN_TEST(halving_keeps_black_and_makes_white_mid_grey);
  return UNITY_END();
}
