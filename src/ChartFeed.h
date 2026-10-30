#pragma once

// A board's side of the charts' history (designer device contract §2.13,
// tasks/charts-firmware-plan.md F3): what its shown screen's charts read,
// subscribed and let go as screens, ranges and days change; the recorder's
// clock and presence; and when a chart needs drawing again. One class for the
// three boards' mains, around the Arduino-free holder (ChartHistory.h).

#include <Arduino.h>
#include <ArduinoJson.h>

#include <map>
#include <vector>

#include "interfaces/IMessagingClient.h"
#include "project/ChartHistory.h"
#include "project/ProjectTypes.h"

class ChartFeed {
 public:
  /** A project was (re)loaded: every view back to its screen's Time range, nothing held. */
  void onProjectLoaded(IMessagingClient& mqtt) {
    views_.clear();
    std::vector<std::string> added, dropped;
    history_.setWanted({}, added, dropped);
    for (const std::string& t : dropped) mqtt.unsubscribe(String(t.c_str()));
    baseSubscribed_ = false;
    checkAt_ = 0;
  }

  /** A message: true when it was the history's (then nothing else is to be done with it). */
  bool onMessage(const String& topic, const String& payload) {
    if (!topic.startsWith(charthistory::PREFIX)) return false;
    if (history_.onMessage(topic.c_str(), payload.c_str(), millis())) redraw_ = true;
    // The clock's first word, or a new day: the topics to read may have changed.
    if (topic == charthistory::NOW) checkAt_ = 0;
    return true;
  }

  /**
   * What the shown screen (and an open popup over it) reads, subscribed;
   * what it no longer reads, let go. Cheap to call often: it only talks to
   * the broker when something changed.
   */
  void update(const ProjectConfig& project, int screenIndex, int popupIndex, IMessagingClient& mqtt) {
    if (!mqtt.isConnected()) return;
    std::vector<const chartcore::Chart*> charts;
    bool any = false;
    for (const Screen& s : project.screens) any = any || hasChart(s.objects);
    for (const Screen& s : project.popups) any = any || hasChart(s.objects);
    if (any && !baseSubscribed_) {
      mqtt.subscribe(charthistory::NOW);
      mqtt.subscribe(charthistory::RECORDER);
      baseSubscribed_ = true;
    }
    shownScreen_ = screenIndex;
    if (screenIndex >= 0 && screenIndex < (int)project.screens.size()) collect(project.screens[screenIndex].objects, charts);
    if (popupIndex >= 0 && popupIndex < (int)project.popups.size()) collect(project.popups[popupIndex].objects, charts);
    std::vector<std::string> topics;
    if (!charts.empty() && history_.hasClock()) {
      const chartcore::RangeView& v = view(project, screenIndex);
      topics = history_.topicsFor(charts, v.range, v.back, history_.now(millis()));
    }
    std::vector<std::string> added, dropped;
    history_.setWanted(topics, added, dropped);
    for (const std::string& t : dropped) mqtt.unsubscribe(String(t.c_str()));
    for (const std::string& t : added) mqtt.subscribe(String(t.c_str()));
    if (!added.empty() || !dropped.empty()) redraw_ = true;
    checkAt_ = millis() + 60000;
  }

  /** From loop(): once a minute, and right after the clock spoke, the topics are checked again. */
  bool due() const { return checkAt_ == 0 || (long)(millis() - checkAt_) >= 0; }

  /** Whether a chart on the shown screen needs drawing again; asking clears it. */
  bool takeRedraw() {
    const bool r = redraw_;
    redraw_ = false;
    return r;
  }

  /** A screen's view: its Time range until an action or a drag changed it. */
  chartcore::RangeView& view(const ProjectConfig& project, int screenIndex) {
    auto it = views_.find(screenIndex);
    if (it != views_.end()) return it->second;
    chartcore::RangeView v;
    if (screenIndex >= 0 && screenIndex < (int)project.screens.size() && !project.screens[screenIndex].timeRange.isEmpty())
      v.range = project.screens[screenIndex].timeRange.c_str();
    return views_[screenIndex] = v;
  }

  const charthistory::History& history() const { return history_; }

  /**
   * One of the four range actions (contract 2.13, ChartCore.h RangeView) on a
   * screen's view; true when it was one. The topics are fitted again on the
   * next loop, and the chart drawn again.
   */
  bool applyAction(const ButtonAction& action, const ProjectConfig& project, int screenIndex) {
    const std::string type = action.type.c_str();
    if (type != "narrow-range" && type != "widen-range" && type != "move-range" && type != "set-range") return false;
    chartcore::RangeView& v = view(project, screenIndex);
    const double t = now();
    const chartcore::Calendar& cal = history_.calendar();
    if (type == "narrow-range") v = chartcore::narrowed(v, t, cal);
    else if (type == "widen-range") v = chartcore::widened(v, t, cal);
    else if (type == "move-range") v = chartcore::moved(v, action.rangeDirection != "later", t, cal);
    else v = chartcore::setRange(action.range.isEmpty() ? "24h" : action.range.c_str());
    checkAt_ = 0;
    redraw_ = true;
    return true;
  }

  /** A scrolling chart at (x, y) on a screen, for a finger to move through time; null where none. */
  static const ScreenObject* scrollingChartAt(const ProjectConfig& project, int screenIndex, int x, int y) {
    if (screenIndex < 0 || screenIndex >= (int)project.screens.size()) return nullptr;
    for (const ScreenObject& o : project.screens[screenIndex].objects) {
      if (!o.properties.chart || !o.properties.chart->chart.scroll) continue;
      if (x >= o.x && y >= o.y && x < o.x + o.width && y < o.y + o.height) return &o;
    }
    return nullptr;
  }

  /** A finger down on a scrolling chart: the view it started from, and the seconds one pixel of its plot is. */
  void beginDrag(const ProjectConfig& project, int screenIndex, const ScreenObject& chart, int x) {
    dragStartView_ = view(project, screenIndex);
    dragScreen_ = screenIndex;
    dragStartX_ = x;
    chartcore::LayoutOptions o;
    o.width = chart.width;
    o.height = chart.height;
    o.fontSize = 14;
    for (const Font& f : project.fonts)
      if (f.id == chart.properties.chart->fontId && f.size > 0) o.fontSize = f.size;
    const chartcore::Window w = chartcore::chartWindow(chart.properties.chart->chart, dragStartView_.range, now(), history_.calendar(), dragStartView_.back);
    const chartcore::Layout l = chartcore::chartLayout(chart.properties.chart->chart, w, std::vector<chartcore::SeriesData>(chart.properties.chart->chart.series.size()), o);
    secondsPerPx_ = chartcore::spanOf(dragStartView_.range) / std::max(1.0, l.plot.w);
    dragging_ = true;
  }

  bool dragging() const { return dragging_; }

  /**
   * Whether a board that animates lets the NowPoint pulse: on, until a test
   * stills it (?set=chartpulse=off) to compare pixels at rest.
   */
  bool pulsing() const { return pulsing_; }
  void setPulsing(bool on) { pulsing_ = on; }

  /** The finger moved: dragged right, the past comes in from the left. */
  void dragTo(const ProjectConfig& project, int x) {
    if (!dragging_) return;
    chartcore::RangeView& v = view(project, dragScreen_);
    const chartcore::RangeView next = chartcore::dragged(dragStartView_, (x - dragStartX_) * secondsPerPx_);
    if (next.back == v.back && next.range == v.range) return;
    v = next;
    redraw_ = true;
  }

  /** Let go: settled on whole days from 24 hours up, the topics fitted again. */
  void endDrag(const ProjectConfig& project) {
    if (!dragging_) return;
    dragging_ = false;
    chartcore::RangeView& v = view(project, dragScreen_);
    v = chartcore::settled(v, now(), history_.calendar());
    checkAt_ = 0;
    redraw_ = true;
  }

  /** The view of the screen shown last fitted to (update). */
  chartcore::RangeView shownView(const ProjectConfig& project) { return view(project, shownScreen_); }

  /** For the test interface (?set=charts): what it holds, as JSON. */
  String describe() const {
    JsonDocument doc;
    doc["recorder"] = history_.recorderOnline();
    doc["clock"] = history_.hasClock() ? history_.now(millis()) : 0;
    doc["days"] = history_.calendar().days.size();
    JsonArray wanted = doc["wanted"].to<JsonArray>();
    JsonArray held = doc["held"].to<JsonArray>();
    for (const std::string& t : history_.wanted()) {
      wanted.add(t.c_str());
      if (!history_.payload(t).empty()) held.add(t.c_str());
    }
    doc["bytes"] = history_.heldBytes();
    doc["pulse"] = pulsing_;
    for (const auto& v : views_) {
      JsonObject o = doc["views"][String(v.first)].to<JsonObject>();
      o["range"] = v.second.range.c_str();
      o["back"] = v.second.back;
    }
    String out;
    serializeJson(doc, out);
    return out;
  }
  double now() const { return history_.now(millis()); }

  static bool hasChart(const std::vector<ScreenObject>& objects) {
    for (const ScreenObject& o : objects)
      if (o.properties.chart || hasChart(o.children)) return true;
    return false;
  }

 private:
  static void collect(const std::vector<ScreenObject>& objects, std::vector<const chartcore::Chart*>& out) {
    for (const ScreenObject& o : objects) {
      if (o.properties.chart) out.push_back(&o.properties.chart->chart);
      collect(o.children, out);
    }
  }

  charthistory::History history_;
  bool pulsing_ = true;
  std::map<int, chartcore::RangeView> views_;
  bool baseSubscribed_ = false;
  bool redraw_ = false;
  int shownScreen_ = -1;
  bool dragging_ = false;
  chartcore::RangeView dragStartView_;
  int dragScreen_ = -1;
  int dragStartX_ = 0;
  double secondsPerPx_ = 1;
  uint32_t checkAt_ = 0;
};
