// The Guard's runs and gesture (src/project/Guard.h), on the PC:
//
//   pio test -e native -f test_guard
//
// The runs are held to the designer's lib/fixtures/guard-spans.json
// (tools/guard-fixture.js writes guard_fixture.h from it); the gesture to
// docs/2026-11-15-guard.md as e2e/guard.spec.ts holds the designer's.

#include <unity.h>

#include "project/Guard.h"
#include "guard_fixture.h"

void setUp() {}
void tearDown() {}

static void check(const guard::Runs& runs, const GuardRun* want, int n, const char* what) {
  int i = 0;
  for (size_t y = 0; y < runs.size(); y++) {
    for (const auto& s : runs[y]) {
      TEST_ASSERT_TRUE_MESSAGE(i < n, what);
      TEST_ASSERT_EQUAL_INT_MESSAGE(want[i].y, (int)y, what);
      TEST_ASSERT_EQUAL_INT_MESSAGE(want[i].a, s.first, what);
      TEST_ASSERT_EQUAL_INT_MESSAGE(want[i].b, s.second, what);
      i++;
    }
  }
  TEST_ASSERT_EQUAL_INT_MESSAGE(n, i, what);
}

static void the_runs_match_the_designers_fixture() {
  for (int k = 0; k < GUARD_CASE_COUNT; k++) {
    const GuardCase& c = GUARD_CASES[k];
    check(guard::lockRuns(c.size), c.lock, c.lockN, "lock");
    check(guard::arrowRuns(c.size), c.arrows, c.arrowsN, "arrows");
  }
}

static void a_plain_tap_shows_the_hint_and_the_cover_goes() {
  guard::Cover c;
  c.down(1000, 100, 130);
  TEST_ASSERT_FALSE(c.unlocked());
  c.move(1050, 102, 129, 27);
  TEST_ASSERT_TRUE(c.up(1080));
  TEST_ASSERT_TRUE(c.phase == guard::Phase::Dropping);
  c.tick(1080 + guard::ANIM_MS);
  TEST_ASSERT_FALSE(c.showing());
}

static void past_the_travel_it_locks_in_and_the_lift_does_nothing() {
  guard::Cover c;
  c.down(0, 100, 130);
  TEST_ASSERT_FALSE(c.move(10, 100, 103, 27));  // 27 is not more than 27
  TEST_ASSERT_TRUE(c.move(20, 100, 102, 27));
  TEST_ASSERT_TRUE(c.unlocked());
  TEST_ASSERT_FALSE(c.up(30));
  TEST_ASSERT_TRUE(c.unlocked());
}

static void never_down_and_not_sideways() {
  guard::Cover c;
  c.down(0, 100, 130);
  c.move(10, 100, 170, 27);
  TEST_ASSERT_EQUAL_INT(0, c.offset);
  c.move(20, 200, 130, 27);
  TEST_ASSERT_EQUAL_INT(0, c.offset);
  TEST_ASSERT_FALSE(c.up(30));  // moved far: no hint
}

static void it_waits_five_seconds_and_a_late_change_tops_up_to_two() {
  guard::Cover c;
  c.down(0, 100, 130);
  c.move(100, 100, 50, 27);
  c.tick(300);
  TEST_ASSERT_TRUE(c.phase == guard::Phase::Open);
  c.changed(1100);  // 4 s left: stays
  TEST_ASSERT_EQUAL_UINT32(100 + guard::RELOCK_MS, c.until);
  c.changed(4600);  // 0.5 s left: 2 s from now
  TEST_ASSERT_EQUAL_UINT32(6600, c.until);
  c.tick(6599);
  TEST_ASSERT_TRUE(c.unlocked());
  c.tick(6600);
  TEST_ASSERT_TRUE(c.phase == guard::Phase::Dropping);
  c.tick(6800);
  TEST_ASSERT_FALSE(c.showing());
}

int main(int, char**) {
  UNITY_BEGIN();
  RUN_TEST(the_runs_match_the_designers_fixture);
  RUN_TEST(a_plain_tap_shows_the_hint_and_the_cover_goes);
  RUN_TEST(past_the_travel_it_locks_in_and_the_lift_does_nothing);
  RUN_TEST(never_down_and_not_sideways);
  RUN_TEST(it_waits_five_seconds_and_a_late_change_tops_up_to_two);
  return UNITY_END();
}
