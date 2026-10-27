// The time field's shared rules (TimeField.h), on the PC:
//
//   pio test -e native -f test_time_field
//
// The same cases as the designer's e2e/time-field.spec.ts «the rules every
// device shares», which tests lib/time-field.ts.

#include <unity.h>

#include <string>

#include "project/TimeField.h"

using namespace timefield;

void setUp() {}
void tearDown() {}

static void expectTime(const char* value, int hour, int minute) {
  TimeOfDay t;
  TEST_ASSERT_TRUE_MESSAGE(parse(value, t), value);
  TEST_ASSERT_EQUAL_MESSAGE(hour, t.hour, value);
  TEST_ASSERT_EQUAL_MESSAGE(minute, t.minute, value);
}

static void readsWhatHomeAssistantWrites() {
  expectTime("06:45", 6, 45);
  expectTime("6:45", 6, 45);
  expectTime("06:45:00", 6, 45);
  expectTime(" 23:59:59.123456 ", 23, 59);
  expectTime("2026-10-27T06:45:00+01:00", 6, 45);
  expectTime("2026-10-27 18:05:10Z", 18, 5);
  expectTime("2026-10-27T07:30-05:00", 7, 30);
}

static void anythingElseIsNoTime() {
  for (const char* value : {"", "unknown", "24:00", "12:60", "12:5", "123:00", "12:00:60", "12:00:", "12:00:00.", "7pm", "06h45", "2026-10-27"}) {
    TimeOfDay t;
    TEST_ASSERT_FALSE_MESSAGE(parse(value, t), value);
  }
  TEST_ASSERT_EQUAL_STRING("--:--", shown("unknown").c_str());
}

static void shownIn24Hours() {
  TEST_ASSERT_EQUAL_STRING("06:05", format(TimeOfDay{6, 5}).c_str());
  TEST_ASSERT_EQUAL_STRING("18:30", shown("18:30:00").c_str());
}

static void expectStep(int h, int m, int step, int eh, int em) {
  const TimeOfDay t = onStep(TimeOfDay{h, m}, step);
  TEST_ASSERT_EQUAL(eh, t.hour);
  TEST_ASSERT_EQUAL(em, t.minute);
}

static void onTheStepToTheNearest() {
  expectStep(6, 43, 5, 6, 45);
  expectStep(6, 42, 5, 6, 40);
  expectStep(6, 58, 5, 7, 0);
  expectStep(23, 58, 5, 0, 0);
  expectStep(6, 7, 15, 6, 0);
  expectStep(6, 8, 15, 6, 15);
  expectStep(6, 15, 30, 6, 30);
  expectStep(6, 43, 1, 6, 43);
}

static void sentAsHomeAssistantWrites() {
  TEST_ASSERT_EQUAL_STRING("06:45:00", payload(TimeOfDay{6, 45}).c_str());
  TEST_ASSERT_EQUAL(5, step(0));
  TEST_ASSERT_EQUAL(15, step(15));
  TEST_ASSERT_EQUAL(5, step(7));
}

int main(int, char**) {
  UNITY_BEGIN();
  RUN_TEST(readsWhatHomeAssistantWrites);
  RUN_TEST(anythingElseIsNoTime);
  RUN_TEST(shownIn24Hours);
  RUN_TEST(onTheStepToTheNearest);
  RUN_TEST(sentAsHomeAssistantWrites);
  return UNITY_END();
}
