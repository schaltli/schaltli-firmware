// What a display holds of the recorder's history (designer
// tasks/charts-firmware-plan.md F3): the shown screen's payloads and no
// others, the recorder's clock and table of days, whether it is there.
//
//   pio test -e native

#include <ArduinoJson.h>
#include <unity.h>

#include <cmath>

#include "project/ChartHistory.h"

using namespace charthistory;

void setUp() {}
void tearDown() {}

static chartcore::Chart chartOf(const char* json) {
  JsonDocument d;
  deserializeJson(d, json);
  return chartcore::chartFrom(d.as<JsonObjectConst>());
}

// 2026-10-14 in Zurich began at 1791928800 (UTC+2), the 13th and the 15th a
// day before and after; the clock says 12:00:30 on the 14th.
static const char* NOW_PAYLOAD =
    R"({"time":1791972030,"tz":"Europe/Zurich","days":[{"date":"2026-10-13","start":1791842400},{"date":"2026-10-14","start":1791928800},{"date":"2026-10-15","start":1792015200}]})";

static void test_clock_and_days() {
  History h;
  TEST_ASSERT_FALSE(h.hasClock());
  TEST_ASSERT_TRUE(h.now(5000) == 0);
  TEST_ASSERT_TRUE(h.onMessage(NOW, NOW_PAYLOAD, 5000));
  TEST_ASSERT_TRUE(h.hasClock());
  TEST_ASSERT_TRUE(h.now(7500) == 1791972030 + 2.5);
  TEST_ASSERT_EQUAL(3, h.calendar().days.size());
  TEST_ASSERT_EQUAL_STRING("2026-10-14", chartcore::dateOf(1791972030, h.calendar()).c_str());
  // The same days again: nothing to redraw.
  TEST_ASSERT_FALSE(h.onMessage(NOW, NOW_PAYLOAD, 65000));
}

static void test_recorder_presence() {
  History h;
  TEST_ASSERT_FALSE(h.recorderOnline());
  TEST_ASSERT_TRUE(h.onMessage(RECORDER, R"({"state":"online","id":"pekaway"})", 0));
  TEST_ASSERT_TRUE(h.recorderOnline());
  TEST_ASSERT_TRUE(h.onMessage(RECORDER, R"({"state":"offline","id":"pekaway"})", 0));
  TEST_ASSERT_FALSE(h.recorderOnline());
  TEST_ASSERT_FALSE(h.onMessage(RECORDER, "", 0));
}

static void test_the_shown_screens_topics_only() {
  History h;
  h.onMessage(NOW, NOW_PAYLOAD, 1000);
  chartcore::Chart c = chartOf(R"({"mode":"time","series":[
    {"id":"s1","source":{"kind":"topic","topic":"van/tank#level"},"valueKind":"level","draw":"line"},
    {"id":"s2","source":{"kind":"balance"},"valueKind":"power","draw":"stacked"}]})");
  std::vector<const chartcore::Chart*> charts = {&c};
  std::vector<std::string> t24 = h.topicsFor(charts, "24h", 0, h.now(1000));
  TEST_ASSERT_EQUAL(1, t24.size());
  TEST_ASSERT_EQUAL_STRING("schaltli/history/van/tank~level/day/2026-10-14", t24[0].c_str());
  std::vector<std::string> t1 = h.topicsFor(charts, "1h", 0, h.now(1000));
  TEST_ASSERT_EQUAL_STRING("schaltli/history/van/tank~level/recent", t1[0].c_str());

  std::vector<std::string> added, dropped;
  h.setWanted(t24, added, dropped);
  TEST_ASSERT_EQUAL(1, added.size());
  TEST_ASSERT_EQUAL(0, dropped.size());
  TEST_ASSERT_TRUE(h.onMessage(t24[0], R"({"date":"2026-10-14","step":900,"mean":[50]})", 1000));
  TEST_ASSERT_FALSE(h.onMessage(t24[0], R"({"date":"2026-10-14","step":900,"mean":[50]})", 1000));
  // A topic nobody wants is not kept.
  TEST_ASSERT_FALSE(h.onMessage("schaltli/history/other/recent", "{}", 1000));
  TEST_ASSERT_EQUAL_STRING("", h.payload("schaltli/history/other/recent").c_str());
  TEST_ASSERT_TRUE(h.heldBytes() > 0);

  // Another screen: the day is let go.
  added.clear();
  dropped.clear();
  h.setWanted(t1, added, dropped);
  TEST_ASSERT_EQUAL_STRING(t1[0].c_str(), added[0].c_str());
  TEST_ASSERT_EQUAL_STRING(t24[0].c_str(), dropped[0].c_str());
  TEST_ASSERT_EQUAL(0, h.heldBytes());
}

int main(int, char**) {
  UNITY_BEGIN();
  RUN_TEST(test_clock_and_days);
  RUN_TEST(test_recorder_presence);
  RUN_TEST(test_the_shown_screens_topics_only);
  return UNITY_END();
}
