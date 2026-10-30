#pragma once

// The chart's core (designer docs/2026-10-16-charts.md, device contract
// §2.13; designer tasks/charts-firmware-plan.md F1): the window a chart
// shows, the steps of a screen's time range, a series' history read from
// the recorder's payloads, and the layout - every place, range and text a
// renderer needs. A port of the designer's lib/chart-window.ts,
// lib/chart-range.ts and lib/chart-layout.ts, function by function, held to
// their vectors (test/test_chart). No Arduino: it runs on the PC too.

// ProjectTypes.h holds a Chart, and the host tools that build ProjectTypes.h
// without ArduinoJson (tools/level-shape, switch-shape, theme-variant) need
// none of what reads JSON: only that is behind it.
#if __has_include(<ArduinoJson.h>)
#include <ArduinoJson.h>
#define CHARTCORE_JSON 1
#endif

#include <string>
#include <utility>
#include <vector>

namespace chartcore {

// ---- days: the recorder's table (schaltli/history/now `days`)

struct Day {
  std::string date;  // "YYYY-MM-DD"
  double start = 0;  // unix second it began
};

struct Calendar {
  std::vector<Day> days;
};

std::string addDays(const std::string& date, int n);
int daysBetween(const std::string& a, const std::string& b);
std::string dateOf(double t, const Calendar& cal);
double midnightOf(const std::string& date, const Calendar& cal);
/** «Mo» … «So». */
std::string weekdayOf(const std::string& date);
// A midnight's label: «Fr 30.10.» (lib/chart-layout.ts dayLabelOf).
std::string dayLabelOf(const std::string& date);

// ---- the chart, as the project has it

struct Axis {
  std::string scale = "auto";  // auto, fixed, at-least
  bool hasMin = false, hasMax = false;
  double min = 0, max = 0;
  std::string unit;
};

struct Zone {
  std::string axis = "left";
  bool hasFrom = false, hasTo = false;
  double from = 0, to = 0;
  std::string color, colorDark;
};

struct Series {
  std::string id;
  bool balance = false;  // source kind balance, else a topic
  std::string topic;
  std::string name;
  std::string valueKind = "level";  // level, power, state
  bool invert = false;
  bool hasFactor = false;
  double factor = 1;
  std::string unit;
  bool hasDecimals = false;
  int decimals = 0;
  std::string draw = "line";
  std::string stack;
  std::string axis = "left";
  std::string color, colorDark;
  bool range = false;
  std::string dailyValue;  // empty: the kind's default
  std::vector<std::string> annotations;
  bool annotated(const char* a) const;
};

struct Chart {
  std::string mode = "time";  // time, per-day
  bool hasDays = false;
  int days = 7;
  bool scroll = false;
  Axis left;
  bool hasRight = false;
  Axis right;
  std::vector<Zone> zones;
  std::string legend = "top-right";
  std::vector<Series> series;
};

/** A chart's properties as the export writes them. */
#ifdef CHARTCORE_JSON
Chart chartFrom(JsonObjectConst properties);
#endif

// ---- the window (lib/chart-window.ts)

struct Window {
  double from = 0, to = 0;
  std::string source;  // recent, recent6, day, daily
  double step = 0;
  std::vector<std::string> dates;
};

/** range: "1h", "6h", "24h", "7d". */
Window chartWindow(const Chart& chart, const std::string& range, double now, const Calendar& cal, double back = 0);

/** Where a topic's history lies under schaltli/history/: its `#` written `~`. */
std::string historyName(const std::string& topic);

/** The retained topics a chart reads for a window (lib/chart-window.ts historyTopicsFor). */
std::vector<std::string> historyTopicsFor(const Chart& chart, const Window& window);

// ---- a screen's range (lib/chart-range.ts)

struct RangeView {
  std::string range = "24h";
  double back = 0;
};

double spanOf(const std::string& range);
RangeView narrowed(const RangeView& v, double now, const Calendar& cal);
RangeView widened(const RangeView& v, double now, const Calendar& cal);
RangeView moved(const RangeView& v, bool earlier, double now, const Calendar& cal);
RangeView setRange(const std::string& range);
RangeView dragged(const RangeView& v, double seconds);
RangeView settled(const RangeView& v, double now, const Calendar& cal);

// ---- a series' history (lib/chart-layout.ts seriesData)

struct Step {
  double t = 0, len = 0, mean = 0, min = 0, max = 0;
};

struct DayBar {
  std::string date;
  bool hasUp = false, hasDown = false;
  double up = 0, down = 0;
};

struct SeriesData {
  std::vector<Step> steps;
  std::vector<std::pair<double, double>> on;
  std::vector<DayBar> days;
};

#ifdef CHARTCORE_JSON
SeriesData seriesData(const Series& s, const std::vector<JsonObjectConst>& payloads, const Calendar& cal);
#endif

// ---- numbers and texts

double niceStep(double span, int count = 4);
std::string fixed(double v, int decimals);
std::string axisLabel(double value, double step, const std::string& decimal = ".");
std::string valueText(double v, const Series& s, const std::string& decimal = ".");
std::string energyText(double wh, const std::string& decimal = ".");
std::string runtimeText(double minutes);

// ---- the layout

struct Box {
  double x = 0, y = 0, w = 0, h = 0;
};

using Point = std::pair<double, double>;

struct AxisTick {
  double value = 0, y = 0;
  std::string label;
};

struct AxisLayout {
  double min = 0, max = 0;
  std::vector<AxisTick> ticks;
};

struct Mark {
  double x = 0, y = 0;
  std::string label;
  bool below = false;
};

// How an area fades from one row to another (lib/chart-layout.ts Fade).
struct Fade {
  double y0 = 0, a0 = 0, y1 = 0, a1 = 0;
};

// Straight segments a curve between two points is drawn with
// (lib/chart-layout.ts CURVE_SEGMENTS).
constexpr int CURVE_SEGMENTS = 6;

// A run of points as a smooth curve, flattened (lib/chart-layout.ts smoothCurve).
std::vector<Point> smoothCurve(const std::vector<Point>& points);

struct SeriesLayout {
  std::string id, draw, axis, color, colorDark;
  int pattern = 0;
  std::vector<std::vector<Point>> lines, areas, ranges;
  // Each area's fade, by row, one per area.
  std::vector<Fade> areaFades;
  std::vector<Box> bands;
  std::vector<double> starts;
  std::vector<Box> bars;
  std::vector<Mark> marks;
  bool hasNowPoint = false;
  Point nowPoint;
  // A stack's NowPoint: on zero, in the chart's now colour.
  bool nowOnStack = false;
};

struct LegendRow {
  std::string id;
  double x = 0, y = 0;
  std::string align;
  Box swatch;
  std::string color, colorDark;
  int pattern = 0;
  std::string name;
  std::vector<std::string> figures;
};

struct Text {
  std::string text;
  double x = 0, y = 0, w = 0, h = 0;
  std::string align;  // left, center, right
  std::string tone;   // text, muted
};

struct Labelled {
  double x = 0;
  std::string label;
};

struct ZoneBox {
  Box box;
  std::string color, colorDark;
};

struct Layout {
  Box plot;
  AxisLayout left;
  bool hasRight = false;
  AxisLayout right;
  std::vector<Labelled> times, midnights;
  std::vector<ZoneBox> zones;
  std::vector<SeriesLayout> series;
  std::vector<LegendRow> legend;
  std::vector<Text> texts;
  // now: on the plot (x), past its right edge, or neither.
  bool nowOn = false, nowPast = false;
  double nowX = 0;
};

struct LayoutOptions {
  double width = 0, height = 0, fontSize = 14, now = 0;
  Calendar cal;
  std::string decimal = ".";
};

/** data: one SeriesData per series, in the chart's series order. */
Layout chartLayout(const Chart& chart, const Window& window, const std::vector<SeriesData>& data, const LayoutOptions& o);

}  // namespace chartcore
