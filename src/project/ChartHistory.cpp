#include "ChartHistory.h"

#include <ArduinoJson.h>

#include <algorithm>

namespace charthistory {

static const std::string EMPTY;

std::vector<std::string> History::topicsFor(const std::vector<const chartcore::Chart*>& charts, const std::string& range, double back,
                                            double nowSeconds) const {
  std::vector<std::string> out;
  for (const chartcore::Chart* c : charts) {
    const chartcore::Window w = chartcore::chartWindow(*c, range, nowSeconds, calendar_, back);
    for (const std::string& t : chartcore::historyTopicsFor(*c, w))
      if (std::find(out.begin(), out.end(), t) == out.end()) out.push_back(t);
  }
  return out;
}

void History::setWanted(const std::vector<std::string>& topics, std::vector<std::string>& added, std::vector<std::string>& dropped) {
  for (const std::string& t : topics)
    if (std::find(wanted_.begin(), wanted_.end(), t) == wanted_.end()) added.push_back(t);
  for (const std::string& t : wanted_)
    if (std::find(topics.begin(), topics.end(), t) == topics.end()) {
      dropped.push_back(t);
      payloads_.erase(t);
    }
  wanted_ = topics;
}

bool History::onMessage(const std::string& topic, const std::string& payload, uint32_t millisNow) {
  if (topic == NOW) {
    JsonDocument doc;
    if (deserializeJson(doc, payload)) return false;
    clock_ = doc["time"] | 0.0;
    clockAt_ = millisNow == 0 ? 1 : millisNow;
    chartcore::Calendar cal;
    for (JsonObjectConst d : doc["days"].as<JsonArrayConst>()) cal.days.push_back({d["date"].as<std::string>(), d["start"].as<double>()});
    // A day turned: the windows' days move on - redraw.
    const bool turned = cal.days.size() != calendar_.days.size() || (!cal.days.empty() && cal.days.back().date != calendar_.days.back().date);
    calendar_ = cal;
    return turned;
  }
  if (topic == RECORDER) {
    JsonDocument doc;
    const bool online = !payload.empty() && !deserializeJson(doc, payload) && std::string(doc["state"] | "") == "online";
    const bool changed = online != recorderOnline_;
    recorderOnline_ = online;
    return changed;
  }
  if (std::find(wanted_.begin(), wanted_.end(), topic) == wanted_.end()) return false;
  if (payload.empty()) return payloads_.erase(topic) > 0;
  std::string& held = payloads_[topic];
  if (held == payload) return false;
  held = payload;
  return true;
}

double History::now(uint32_t millisNow) const {
  if (clockAt_ == 0) return 0;
  return clock_ + static_cast<uint32_t>(millisNow - clockAt_) / 1000.0;
}

const std::string& History::payload(const std::string& topic) const {
  auto it = payloads_.find(topic);
  return it == payloads_.end() ? EMPTY : it->second;
}

size_t History::heldBytes() const {
  size_t n = 0;
  for (const auto& p : payloads_) n += p.second.size();
  return n;
}

}  // namespace charthistory
