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
  std::map<int, chartcore::RangeView> views_;
  bool baseSubscribed_ = false;
  bool redraw_ = false;
  uint32_t checkAt_ = 0;
};
