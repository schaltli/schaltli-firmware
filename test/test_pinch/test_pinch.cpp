// Pinch in and out from one contact's samples (src/project/PinchDetector.h),
// on the PC:
//
//   pio test -e native -f test_pinch
//
// Coordinates are the 4.3B's, 800x480.

#include <unity.h>

#include "project/PinchDetector.h"

void setUp() {}
void tearDown() {}

// Two fingers on a horizontal line around (400, 240), `apart` px apart.
static void two(PinchDetector &p, int apart, uint32_t ms)
{
  p.sample(2, 400 - apart / 2, 240, 400 + apart / 2, 240, ms);
}

static void fingers_drawn_together_are_a_pinch_in()
{
  PinchDetector p;
  p.begin(0);
  p.sample(1, 250, 240, 0, 0, 0);
  for (int i = 0; i <= 10; i++) two(p, 300 - i * 20, 30 + i * 10);
  TEST_ASSERT_TRUE(p.multi());
  TEST_ASSERT_EQUAL(PinchDetector::PinchIn, p.finish());
  TEST_ASSERT_EQUAL(300, p.startDistance());
  TEST_ASSERT_EQUAL(100, p.endDistance());
  TEST_ASSERT_EQUAL(30, p.secondFingerAfterMs());
}

static void fingers_drawn_apart_are_a_pinch_out()
{
  PinchDetector p;
  p.begin(0);
  for (int i = 0; i <= 10; i++) two(p, 120 + i * 25, i * 10);
  TEST_ASSERT_EQUAL(PinchDetector::PinchOut, p.finish());
}

static void two_fingers_dragged_side_by_side_are_neither()
{
  PinchDetector p;
  p.begin(0);
  for (int i = 0; i <= 10; i++) p.sample(2, 200 + i * 30, 240, 400 + i * 30, 240, i * 10);
  TEST_ASSERT_TRUE(p.multi());
  TEST_ASSERT_EQUAL(PinchDetector::None, p.finish());
}

static void a_small_twitch_of_close_fingers_is_neither_though_its_ratio_is_large()
{
  // 40 px to 25 px is a ratio of 0.63, and 15 px of travel.
  PinchDetector p;
  p.begin(0);
  two(p, 40, 0);
  two(p, 25, 10);
  TEST_ASSERT_EQUAL(PinchDetector::None, p.finish());
}

static void a_large_but_proportionally_small_change_is_neither()
{
  // 600 px to 520 px: 80 px of travel, a ratio of 0.87.
  PinchDetector p;
  p.begin(0);
  two(p, 600, 0);
  two(p, 520, 10);
  TEST_ASSERT_EQUAL(PinchDetector::None, p.finish());
}

static void the_last_two_finger_sample_counts_not_what_the_remaining_finger_does()
{
  PinchDetector p;
  p.begin(0);
  two(p, 300, 0);
  two(p, 100, 50);
  // One finger lifts first; the other wanders off.
  p.sample(1, 700, 400, 0, 0, 60);
  p.sample(1, 790, 470, 0, 0, 70);
  TEST_ASSERT_EQUAL(PinchDetector::PinchIn, p.finish());
  TEST_ASSERT_EQUAL(100, p.endDistance());
}

static void one_finger_is_never_a_pinch()
{
  PinchDetector p;
  p.begin(0);
  for (int i = 0; i <= 10; i++) p.sample(1, 100 + i * 50, 240, 0, 0, i * 10);
  TEST_ASSERT_FALSE(p.multi());
  TEST_ASSERT_EQUAL(PinchDetector::None, p.finish());
}

static void a_single_two_finger_sample_says_nothing_about_a_change()
{
  PinchDetector p;
  p.begin(0);
  two(p, 300, 0);
  TEST_ASSERT_TRUE(p.multi());
  TEST_ASSERT_EQUAL(PinchDetector::None, p.finish());
}

static void a_new_contact_forgets_the_last()
{
  PinchDetector p;
  p.begin(0);
  two(p, 300, 0);
  two(p, 100, 10);
  p.begin(1000);
  TEST_ASSERT_FALSE(p.multi());
  TEST_ASSERT_EQUAL(PinchDetector::None, p.finish());
}

int main()
{
  UNITY_BEGIN();
  RUN_TEST(fingers_drawn_together_are_a_pinch_in);
  RUN_TEST(fingers_drawn_apart_are_a_pinch_out);
  RUN_TEST(two_fingers_dragged_side_by_side_are_neither);
  RUN_TEST(a_small_twitch_of_close_fingers_is_neither_though_its_ratio_is_large);
  RUN_TEST(a_large_but_proportionally_small_change_is_neither);
  RUN_TEST(the_last_two_finger_sample_counts_not_what_the_remaining_finger_does);
  RUN_TEST(one_finger_is_never_a_pinch);
  RUN_TEST(a_single_two_finger_sample_says_nothing_about_a_change);
  RUN_TEST(a_new_contact_forgets_the_last);
  return UNITY_END();
}
