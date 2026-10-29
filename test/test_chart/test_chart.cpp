// The chart's vectors (schaltli-designer lib/chart/vectors.json, written by
// its e2e/chart-vectors.spec.ts) against the firmware's ChartCore, on the PC:
//
//   pio test -e native
//
// chart-vectors.json beside this file is a byte-for-byte copy of the
// designer's. With the designer checked out beside this repo, a copy that
// differs fails here - copy it again, never edit it. Without the designer
// the check is skipped and says so.

#include <ArduinoJson.h>
#include <unity.h>

#include <cmath>
#include <cstdio>
#include <fstream>
#include <map>
#include <sstream>
#include <string>

#include "project/ChartCore.h"
#include "project/ChartShapes.h"

using namespace chartcore;

static const char* COPY = "test/test_chart/chart-vectors.json";
static const char* DESIGNER = "../schaltli-designer/lib/chart/vectors.json";

static bool readFile(const char* path, std::string& out) {
  std::ifstream in(path, std::ios::binary);
  if (!in) return false;
  std::ostringstream buffer;
  buffer << in.rdbuf();
  out = buffer.str();
  return true;
}

static JsonDocument doc;
static std::map<std::string, Calendar> zones;

void setUp() {}
void tearDown() {}

// The layout's numbers are rounded to tenths. ArduinoJson keeps a number of
// few digits as a float, so a value read from a payload can land a hair on
// the other side of a .x5 and round one tenth away - as on the device, where
// the payloads are read the same way. Within one step of rounding, then;
// the texts must match exactly.
static bool near(double a, double b) { return std::fabs(a - b) < 0.1 + 1e-3; }

static void expectNear(double expected, double actual, const std::string& what) {
  if (!near(expected, actual)) {
    char msg[300];
    std::snprintf(msg, sizeof msg, "%s: expected %.6f, got %.6f", what.c_str(), expected, actual);
    TEST_FAIL_MESSAGE(msg);
  }
}

static void expectText(const std::string& expected, const std::string& actual, const std::string& what) {
  if (expected != actual) {
    std::string msg = what + ": expected \"" + expected + "\", got \"" + actual + "\"";
    TEST_FAIL_MESSAGE(msg.c_str());
  }
}

static void test_copy_is_the_designers() {
  std::string copy, designer;
  TEST_ASSERT_TRUE_MESSAGE(readFile(COPY, copy), "chart-vectors.json missing beside the test");
  if (!readFile(DESIGNER, designer)) {
    TEST_IGNORE_MESSAGE("schaltli-designer not checked out beside this repo: copy not compared");
    return;
  }
  TEST_ASSERT_TRUE_MESSAGE(copy == designer, "chart-vectors.json differs from the designer's: copy it again");
}

static Calendar calendarFrom(JsonObjectConst z) {
  Calendar c;
  for (JsonObjectConst d : z["days"].as<JsonArrayConst>()) c.days.push_back({d["date"].as<std::string>(), d["start"].as<double>()});
  return c;
}

static void test_windows() {
  int i = 0;
  for (JsonObjectConst v : doc["windows"].as<JsonArrayConst>()) {
    JsonObjectConst in = v["in"];
    Chart chart = chartFrom(in["chart"]);
    const Calendar& cal = zones[in["tz"].as<std::string>()];
    Window w = chartWindow(chart, in["range"].as<std::string>(), in["now"].as<double>(), cal, in["back"].as<double>());
    JsonObjectConst out = v["out"];
    const std::string at = "window " + std::to_string(i++);
    expectNear(out["from"], w.from, at + " from");
    expectNear(out["to"], w.to, at + " to");
    expectText(out["source"].as<std::string>(), w.source, at + " source");
    expectNear(out["step"], w.step, at + " step");
    JsonArrayConst dates = out["dates"];
    TEST_ASSERT_EQUAL_MESSAGE(dates.size(), w.dates.size(), (at + " dates").c_str());
    for (size_t k = 0; k < dates.size(); k++) expectText(dates[k].as<std::string>(), w.dates[k], at + " date");
  }
}

static void test_range_steps() {
  const Calendar& cal = zones["zurich"];
  // The steps' now: the vectors' NOON (2026-10-14 12:00:30 in Zurich).
  const double now = midnightOf("2026-10-14", cal) + 12 * 3600 + 30;
  int i = 0;
  for (JsonObjectConst v : doc["steps"].as<JsonArrayConst>()) {
    JsonObjectConst in = v["in"];
    RangeView view;
    view.range = in["view"]["range"].as<std::string>();
    view.back = in["view"]["back"].as<double>();
    const std::string step = in["step"].as<std::string>();
    RangeView got;
    if (step == "narrow") got = narrowed(view, now, cal);
    else if (step == "widen") got = widened(view, now, cal);
    else if (step == "earlier") got = moved(view, true, now, cal);
    else if (step == "later") got = moved(view, false, now, cal);
    else if (step == "set") got = setRange(in["arg"].as<std::string>());
    else got = settled(dragged(view, in["arg"].as<double>()), now, cal);
    const std::string at = "step " + std::to_string(i++) + " " + step;
    expectText(v["out"]["range"].as<std::string>(), got.range, at + " range");
    expectNear(v["out"]["back"], got.back, at + " back");
  }
}

static void expectPoints(JsonArrayConst expected, const std::vector<std::vector<Point>>& got, const std::string& at) {
  TEST_ASSERT_EQUAL_MESSAGE(expected.size(), got.size(), (at + " runs").c_str());
  for (size_t r = 0; r < expected.size(); r++) {
    JsonArrayConst run = expected[r];
    TEST_ASSERT_EQUAL_MESSAGE(run.size(), got[r].size(), (at + " points").c_str());
    for (size_t p = 0; p < run.size(); p++) {
      expectNear(run[p][0], got[r][p].first, at + " x");
      expectNear(run[p][1], got[r][p].second, at + " y");
    }
  }
}

static void expectBoxes(JsonArrayConst expected, const std::vector<Box>& got, const std::string& at) {
  TEST_ASSERT_EQUAL_MESSAGE(expected.size(), got.size(), (at + " count").c_str());
  for (size_t k = 0; k < expected.size(); k++) {
    expectNear(expected[k]["x"], got[k].x, at + " x");
    expectNear(expected[k]["y"], got[k].y, at + " y");
    expectNear(expected[k]["w"], got[k].w, at + " w");
    expectNear(expected[k]["h"], got[k].h, at + " h");
  }
}

static void expectAxis(JsonObjectConst e, const AxisLayout& a, const std::string& at) {
  expectNear(e["min"], a.min, at + " min");
  expectNear(e["max"], a.max, at + " max");
  JsonArrayConst ticks = e["ticks"];
  TEST_ASSERT_EQUAL_MESSAGE(ticks.size(), a.ticks.size(), (at + " ticks").c_str());
  for (size_t k = 0; k < ticks.size(); k++) {
    expectNear(ticks[k]["y"], a.ticks[k].y, at + " tick y");
    expectText(ticks[k]["label"].as<std::string>(), a.ticks[k].label, at + " tick label");
  }
}

static void test_layouts() {
  for (JsonObjectConst v : doc["layouts"].as<JsonArrayConst>()) {
    const std::string name = v["name"].as<std::string>();
    JsonObjectConst in = v["in"];
    Chart chart = chartFrom(in["chart"]);
    JsonObjectConst opt = in["options"];
    LayoutOptions o;
    o.width = opt["width"];
    o.height = opt["height"];
    o.fontSize = opt["fontSize"];
    o.now = opt["now"];
    o.cal = zones[opt["tz"].as<std::string>()];
    o.decimal = opt["decimal"] | ".";
    Window w = chartWindow(chart, in["range"].as<std::string>(), o.now, o.cal, in["back"].as<double>());
    std::vector<SeriesData> data;
    for (const Series& s : chart.series) {
      std::vector<JsonObjectConst> payloads;
      for (JsonObjectConst p : in["payloads"][s.id].as<JsonArrayConst>()) payloads.push_back(p);
      data.push_back(seriesData(s, payloads, o.cal));
    }
    Layout L = chartLayout(chart, w, data, o);
    JsonObjectConst out = v["out"];

    expectNear(out["plot"]["x"], L.plot.x, name + " plot x");
    expectNear(out["plot"]["y"], L.plot.y, name + " plot y");
    expectNear(out["plot"]["w"], L.plot.w, name + " plot w");
    expectNear(out["plot"]["h"], L.plot.h, name + " plot h");
    expectAxis(out["left"], L.left, name + " left");
    TEST_ASSERT_EQUAL_MESSAGE(!out["right"].isNull(), L.hasRight, (name + " right").c_str());
    if (L.hasRight) expectAxis(out["right"], L.right, name + " right");

    JsonArrayConst times = out["times"];
    TEST_ASSERT_EQUAL_MESSAGE(times.size(), L.times.size(), (name + " times").c_str());
    for (size_t k = 0; k < times.size(); k++) {
      expectNear(times[k]["x"], L.times[k].x, name + " time x");
      expectText(times[k]["label"].as<std::string>(), L.times[k].label, name + " time label");
    }
    JsonArrayConst mids = out["midnights"];
    TEST_ASSERT_EQUAL_MESSAGE(mids.size(), L.midnights.size(), (name + " midnights").c_str());
    for (size_t k = 0; k < mids.size(); k++) {
      expectNear(mids[k]["x"], L.midnights[k].x, name + " midnight x");
      expectText(mids[k]["label"].as<std::string>(), L.midnights[k].label, name + " midnight label");
    }
    JsonArrayConst zs = out["zones"];
    TEST_ASSERT_EQUAL_MESSAGE(zs.size(), L.zones.size(), (name + " zones").c_str());
    for (size_t k = 0; k < zs.size(); k++) {
      expectNear(zs[k]["y"], L.zones[k].box.y, name + " zone y");
      expectNear(zs[k]["h"], L.zones[k].box.h, name + " zone h");
    }

    JsonArrayConst series = out["series"];
    TEST_ASSERT_EQUAL_MESSAGE(series.size(), L.series.size(), (name + " series").c_str());
    for (size_t k = 0; k < series.size(); k++) {
      JsonObjectConst e = series[k];
      const SeriesLayout& g = L.series[k];
      const std::string at = name + " " + g.id;
      expectPoints(e["lines"], g.lines, at + " lines");
      expectPoints(e["areas"], g.areas, at + " areas");
      expectPoints(e["ranges"], g.ranges, at + " ranges");
      expectBoxes(e["bands"], g.bands, at + " bands");
      expectBoxes(e["bars"], g.bars, at + " bars");
      JsonArrayConst starts = e["starts"];
      TEST_ASSERT_EQUAL_MESSAGE(starts.size(), g.starts.size(), (at + " starts").c_str());
      for (size_t s = 0; s < starts.size(); s++) expectNear(starts[s], g.starts[s], at + " start");
      JsonArrayConst marks = e["marks"];
      TEST_ASSERT_EQUAL_MESSAGE(marks.size(), g.marks.size(), (at + " marks").c_str());
      for (size_t m = 0; m < marks.size(); m++) {
        expectNear(marks[m]["x"], g.marks[m].x, at + " mark x");
        expectNear(marks[m]["y"], g.marks[m].y, at + " mark y");
        expectText(marks[m]["label"].as<std::string>(), g.marks[m].label, at + " mark label");
      }
      TEST_ASSERT_EQUAL_MESSAGE(!e["nowPoint"].isNull(), g.hasNowPoint, (at + " nowPoint").c_str());
      if (g.hasNowPoint) {
        expectNear(e["nowPoint"][0], g.nowPoint.first, at + " nowPoint x");
        expectNear(e["nowPoint"][1], g.nowPoint.second, at + " nowPoint y");
      }
    }

    JsonArrayConst legend = out["legend"];
    TEST_ASSERT_EQUAL_MESSAGE(legend.size(), L.legend.size(), (name + " legend").c_str());
    for (size_t k = 0; k < legend.size(); k++) {
      const std::string at = name + " legend " + std::to_string(k);
      expectNear(legend[k]["x"], L.legend[k].x, at + " x");
      expectNear(legend[k]["y"], L.legend[k].y, at + " y");
      expectText(legend[k]["align"].as<std::string>(), L.legend[k].align, at + " align");
      expectNear(legend[k]["swatch"]["x"], L.legend[k].swatch.x, at + " swatch x");
      JsonArrayConst figs = legend[k]["figures"];
      TEST_ASSERT_EQUAL_MESSAGE(figs.size(), L.legend[k].figures.size(), (at + " figures").c_str());
      for (size_t f = 0; f < figs.size(); f++) expectText(figs[f].as<std::string>(), L.legend[k].figures[f], at + " figure");
    }

    JsonArrayConst texts = out["texts"];
    TEST_ASSERT_EQUAL_MESSAGE(texts.size(), L.texts.size(), (name + " texts").c_str());
    for (size_t k = 0; k < texts.size(); k++) {
      const std::string at = name + " text " + std::to_string(k);
      expectText(texts[k]["text"].as<std::string>(), L.texts[k].text, at);
      expectNear(texts[k]["x"], L.texts[k].x, at + " x");
      expectNear(texts[k]["y"], L.texts[k].y, at + " y");
      expectNear(texts[k]["w"], L.texts[k].w, at + " w");
      expectText(texts[k]["align"].as<std::string>(), L.texts[k].align, at + " align");
      expectText(texts[k]["tone"].as<std::string>(), L.texts[k].tone, at + " tone");
    }

    // The fills a board draws (designer lib/chart-shapes.ts), on colour and on grey.
    for (int grey = 0; grey < 2; grey++) {
      chartshapes::Look look;
      look.grid = "#cccccc";
      look.grey = grey == 1;
      const std::vector<chartshapes::Fill> fills = chartshapes::chartShapes(L, look);
      JsonArrayConst expected = v[grey ? "greyShapes" : "shapes"];
      const std::string at = name + (grey ? " grey" : "") + " shapes";
      TEST_ASSERT_EQUAL_MESSAGE(expected.size(), fills.size(), (at + " count").c_str());
      for (size_t k = 0; k < expected.size(); k++) {
        JsonObjectConst e = expected[k];
        const chartshapes::Fill& f = fills[k];
        const std::string fat = at + " " + std::to_string(k);
        expectText(e["colour"].as<std::string>(), f.colour, fat + " colour");
        TEST_ASSERT_EQUAL_MESSAGE(e["alpha"].as<int>(), f.alpha, (fat + " alpha").c_str());
        TEST_ASSERT_EQUAL_MESSAGE(!e["clip"].isNull(), f.hasClip, (fat + " clip").c_str());
        JsonArrayConst pts = e["points"];
        TEST_ASSERT_EQUAL_MESSAGE(pts.size(), f.points.size(), (fat + " points").c_str());
        // An eighth apart at most: a layout number one rounding step away moves a corner by under one.
        for (size_t q = 0; q < pts.size(); q++) {
          TEST_ASSERT_TRUE_MESSAGE(std::abs(pts[q]["x"].as<int>() - f.points[q].x) <= 1, (fat + " x").c_str());
          TEST_ASSERT_TRUE_MESSAGE(std::abs(pts[q]["y"].as<int>() - f.points[q].y) <= 1, (fat + " y").c_str());
        }
      }
    }

    if (out["now"].isNull()) {
      TEST_ASSERT_FALSE_MESSAGE(L.nowOn || L.nowPast, (name + " now").c_str());
    } else if (out["now"]["past"].is<bool>()) {
      TEST_ASSERT_TRUE_MESSAGE(L.nowPast, (name + " now past").c_str());
    } else {
      TEST_ASSERT_TRUE_MESSAGE(L.nowOn, (name + " now on").c_str());
      expectNear(out["now"]["x"], L.nowX, name + " now x");
    }
  }
}

// No recorder has spoken yet: an empty table. The 4.3B looped for ever over
// the midnights of a window whose every day began at 0 (2026-10-29).
static void test_without_a_table() {
  Calendar empty;
  TEST_ASSERT_EQUAL_STRING("1970-01-04", dateOf(86400 * 3 + 5, empty).c_str());
  TEST_ASSERT_TRUE(midnightOf("1970-01-04", empty) == 86400 * 3);
  Chart c;
  Window w = chartWindow(c, "24h", 0, empty);
  TEST_ASSERT_TRUE(w.from == 0 && w.to == 86400);
  LayoutOptions o;
  o.width = 400;
  o.height = 200;
  Layout L = chartLayout(c, w, {}, o);
  TEST_ASSERT_EQUAL(5, L.times.size());
}

static void test_numbers() {
  TEST_ASSERT_EQUAL_STRING("3", fixed(2.5, 0).c_str());
  TEST_ASSERT_EQUAL_STRING("-3", fixed(-2.5, 0).c_str());
  TEST_ASSERT_EQUAL_STRING("1.00", fixed(1.005, 2).c_str());
  TEST_ASSERT_EQUAL_STRING("-0.0", fixed(-0.04, 1).c_str());
  TEST_ASSERT_EQUAL_STRING("12,5", axisLabel(12.5, 2.5, ",").c_str());
  TEST_ASSERT_EQUAL_STRING("Mi", weekdayOf("2026-10-14").c_str());
  TEST_ASSERT_EQUAL_STRING("2026-11-01", addDays("2026-10-31", 1).c_str());
  TEST_ASSERT_EQUAL_STRING("2:05 h", runtimeText(125).c_str());
}

// A chart as the designer exports it (device contract 2.13, the spec's water
// page): every field the core reads.
static void test_chart_from_the_export() {
  JsonDocument d;
  deserializeJson(d, R"({
    "mode": "time", "scroll": true, "legend": "top-right",
    "left": { "scale": "fixed", "min": 0, "max": 100, "unit": "%" },
    "zones": [{ "axis": "left", "to": 15, "color": "#cccccc", "colorDark": "#333333" }],
    "series": [
      { "id": "s1", "source": { "kind": "topic", "topic": "schaltli/state/tank/1/level" }, "name": "Frischwasser",
        "valueKind": "level", "unit": "%", "draw": "steps", "color": "#6750a4", "colorDark": "#d0bcff", "annotations": ["current"] },
      { "id": "s2", "source": { "kind": "balance" }, "name": "Rest", "valueKind": "power", "draw": "stacked", "stack": "e",
        "invert": true, "factor": 0.001, "decimals": 2, "axis": "right", "range": true, "color": "#888888" }
    ]
  })");
  Chart c = chartFrom(d.as<JsonObjectConst>());
  TEST_ASSERT_EQUAL_STRING("time", c.mode.c_str());
  TEST_ASSERT_TRUE(c.scroll);
  TEST_ASSERT_EQUAL_STRING("fixed", c.left.scale.c_str());
  TEST_ASSERT_TRUE(c.left.hasMin && c.left.hasMax);
  TEST_ASSERT_EQUAL_FLOAT(100, c.left.max);
  TEST_ASSERT_FALSE(c.hasRight);
  TEST_ASSERT_EQUAL(1, c.zones.size());
  TEST_ASSERT_TRUE(c.zones[0].hasTo && !c.zones[0].hasFrom);
  TEST_ASSERT_EQUAL_STRING("#333333", c.zones[0].colorDark.c_str());
  TEST_ASSERT_EQUAL(2, c.series.size());
  TEST_ASSERT_EQUAL_STRING("schaltli/state/tank/1/level", c.series[0].topic.c_str());
  TEST_ASSERT_FALSE(c.series[0].balance);
  TEST_ASSERT_TRUE(c.series[0].annotated("current"));
  TEST_ASSERT_TRUE(c.series[1].balance && c.series[1].invert && c.series[1].range);
  TEST_ASSERT_TRUE(c.series[1].hasFactor && c.series[1].hasDecimals);
  TEST_ASSERT_EQUAL(2, c.series[1].decimals);
  TEST_ASSERT_EQUAL_STRING("right", c.series[1].axis.c_str());
  TEST_ASSERT_EQUAL_STRING("e", c.series[1].stack.c_str());
}

int main(int, char**) {
  std::string text;
  if (!readFile(COPY, text)) {
    std::printf("chart-vectors.json missing\n");
    return 1;
  }
  deserializeJson(doc, text);
  for (JsonPairConst z : doc["zones"].as<JsonObjectConst>()) zones[z.key().c_str()] = calendarFrom(z.value());
  UNITY_BEGIN();
  RUN_TEST(test_copy_is_the_designers);
  RUN_TEST(test_numbers);
  RUN_TEST(test_without_a_table);
  RUN_TEST(test_chart_from_the_export);
  RUN_TEST(test_windows);
  RUN_TEST(test_range_steps);
  RUN_TEST(test_layouts);
  return UNITY_END();
}
