// The time field's picker (TimePicker.h), on the PC:
//
//   pio test -e native -f test_time_picker

#include <unity.h>

#include "project/TimePicker.h"

using namespace timepicker;

void setUp() {}
void tearDown() {}

static void opensOnTheStep() {
  timefield::TimeOfDay shown{6, 43};
  State s = open(&shown, 5);
  TEST_ASSERT_EQUAL(6, s.hour.index);
  TEST_ASSERT_EQUAL(9, s.minute.index);  // 45 on 5
  TEST_ASSERT_EQUAL(12, s.minute.count);
  TEST_ASSERT_EQUAL_STRING("06:45", timefield::format(value(s)).c_str());
  timefield::TimeOfDay late{23, 58};
  TEST_ASSERT_EQUAL_STRING("00:00", timefield::format(value(open(&late, 5))).c_str());
  TEST_ASSERT_EQUAL_STRING("00:00", timefield::format(value(open(nullptr, 15))).c_str());
}

static void aDragFollowsTheFinger() {
  timefield::TimeOfDay shown{6, 30};
  State s = open(&shown, 5);
  const int H = 56;
  // Up by a third of an item: still 30, drawn a third higher.
  drag(s.minute, -H / 3.0f, H);
  TEST_ASSERT_EQUAL_STRING("06:30", timefield::format(value(s)).c_str());
  TEST_ASSERT_TRUE(s.minute.offset < 0);
  // On past half: 35 is in the middle, the offset now below.
  drag(s.minute, -H / 3.0f, H);
  TEST_ASSERT_EQUAL_STRING("06:35", timefield::format(value(s)).c_str());
  TEST_ASSERT_TRUE(s.minute.offset > 0);
  // Down a long way: back past 30 and across the hour without carrying it.
  drag(s.minute, H * 7.0f, H);
  TEST_ASSERT_EQUAL_STRING("06:00", timefield::format(value(s)).c_str());
  drag(s.minute, H * 1.0f, H);
  TEST_ASSERT_EQUAL_STRING("06:55", timefield::format(value(s)).c_str());
}

static void releaseSettlesAndAFlickRunsOn() {
  timefield::TimeOfDay shown{6, 30};
  State s = open(&shown, 5);
  const int H = 56;
  drag(s.minute, -20, H);
  release(s.minute, 0, H, 1000);  // let go still: settles where it is
  TEST_ASSERT_EQUAL_STRING("06:30", timefield::format(value(s)).c_str());
  TEST_ASSERT_TRUE(animate(s.minute, 1100));
  TEST_ASSERT_TRUE(s.minute.offset < 0 && s.minute.offset > -20);
  TEST_ASSERT_TRUE(animate(s.minute, 1000 + SETTLE_MS));
  TEST_ASSERT_EQUAL_FLOAT(0, s.minute.offset);
  TEST_ASSERT_FALSE(animate(s.minute, 1300));
  // A flick upwards at 1 px/ms: about three values on.
  release(s.hour, -1.0f, H, 2000);
  TEST_ASSERT_EQUAL(9, s.hour.index);
  TEST_ASSERT_TRUE(s.hour.offset < 0);
  // Never more than eight.
  release(s.hour, -50.0f, H, 3000);
  TEST_ASSERT_EQUAL(17, s.hour.index);
}

static void aTapBringsTheNeighbour() {
  timefield::TimeOfDay shown{6, 30};
  State s = open(&shown, 15);
  Frame f = frameFor(800, 480, false, Style::Wheels);
  const int below = f.minute.y + f.minute.h / 2 + f.itemH;
  TEST_ASSERT_EQUAL(1, neighbourAt(f.minute, f.itemH, below));
  nudge(s.minute, 1, f.itemH, 0);
  TEST_ASSERT_EQUAL_STRING("06:45", timefield::format(value(s)).c_str());
  TEST_ASSERT_EQUAL_FLOAT(static_cast<float>(f.itemH), s.minute.offset);  // drawn where it was
  nudge(s.minute, 1, f.itemH, 0);
  TEST_ASSERT_EQUAL_STRING("06:00", timefield::format(value(s)).c_str());  // wraps, the hour stays
}

static void framesPerScreen() {
  Frame f = frameFor(800, 480, false, Style::Wheels);
  TEST_ASSERT_EQUAL(192, f.panel.y);
  TEST_ASSERT_TRUE(f.hour.x + f.hour.w < f.minute.x);
  TEST_ASSERT_TRUE(f.minute.x + f.minute.w < f.ok.x);
  TEST_ASSERT_TRUE(f.ok.y > f.cancel.y + f.cancel.h - 1);
  TEST_ASSERT_TRUE(f.hour.y + f.hour.h <= 480);
  TEST_ASSERT_TRUE(f.itemH >= 50);
  TEST_ASSERT_TRUE(partAt(f, f.ok.x + 1, f.ok.y + 1) == Part::Ok);
  TEST_ASSERT_TRUE(partAt(f, f.minute.x + 1, f.minute.y + 1) == Part::Minute);
  // The knob: everything inside the circle of 180 round (180, 180).
  f = frameFor(360, 360, true, Style::Wheels);
  for (const Rect& r : {f.hour, f.minute, f.ok, f.cancel}) {
    for (int cx : {r.x, r.x + r.w}) {
      for (int cy : {r.y, r.y + r.h}) {
        const long dx = cx - 180, dy = cy - 180;
        TEST_ASSERT_TRUE_MESSAGE(dx * dx + dy * dy <= 180L * 180, "a corner outside the bezel");
      }
    }
  }
  // The PaperS3: buttons above and below, inside the panel.
  f = frameFor(960, 540, false, Style::Buttons);
  TEST_ASSERT_TRUE(f.hourUp.y >= f.panel.y);
  TEST_ASSERT_TRUE(f.hourDown.y + f.hourDown.h <= 540);
  TEST_ASSERT_TRUE(partAt(f, f.minuteUp.x + 2, f.minuteUp.y + 2) == Part::MinuteUp);
}

int main(int, char**) {
  UNITY_BEGIN();
  RUN_TEST(opensOnTheStep);
  RUN_TEST(aDragFollowsTheFinger);
  RUN_TEST(releaseSettlesAndAFlickRunsOn);
  RUN_TEST(aTapBringsTheNeighbour);
  RUN_TEST(framesPerScreen);
  return UNITY_END();
}
