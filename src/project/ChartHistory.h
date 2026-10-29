#pragma once

// What a display holds of the recorder's history (designer
// docs/2026-10-16-charts.md decision 7, device contract §2.13;
// tasks/charts-firmware-plan.md decision 5): the retained payloads the shown
// screen's charts read, and nothing else - leaving a screen lets them go -
// the recorder's clock and table of days, and whether it is there. No
// Arduino, no MQTT: the board's main subscribes what `wanted` names and hands
// every message on schaltli/history/ here.

#include <cstdint>
#include <map>
#include <string>
#include <vector>

#include "ChartCore.h"

namespace charthistory {

static const char* const PREFIX = "schaltli/history/";
static const char* const NOW = "schaltli/history/now";
static const char* const RECORDER = "schaltli/history/recorder";

class History {
 public:
  /**
   * The payload topics the given charts read for a view, at `nowSeconds`:
   * what a board subscribes to beside NOW and RECORDER.
   */
  std::vector<std::string> topicsFor(const std::vector<const chartcore::Chart*>& charts, const std::string& range, double back,
                                     double nowSeconds) const;

  /**
   * Keeps exactly these payload topics: what else it holds is let go.
   * Returns the ones it did not want before (to subscribe) through `added`
   * and the ones it no longer wants (to unsubscribe) through `dropped`.
   */
  void setWanted(const std::vector<std::string>& topics, std::vector<std::string>& added, std::vector<std::string>& dropped);

  /** A message on schaltli/history/…; true when it changes what a shown chart draws. */
  bool onMessage(const std::string& topic, const std::string& payload, uint32_t millisNow);

  /** Now in unix seconds: the recorder's clock, run on by the board's; 0 before it spoke. */
  double now(uint32_t millisNow) const;
  bool hasClock() const { return clockAt_ != 0; }
  bool recorderOnline() const { return recorderOnline_; }
  const chartcore::Calendar& calendar() const { return calendar_; }

  /** The payload held for a topic; empty for none. */
  const std::string& payload(const std::string& topic) const;
  size_t heldBytes() const;
  const std::vector<std::string>& wanted() const { return wanted_; }

 private:
  std::vector<std::string> wanted_;
  std::map<std::string, std::string> payloads_;
  chartcore::Calendar calendar_;
  double clock_ = 0;
  uint32_t clockAt_ = 0;
  bool recorderOnline_ = false;
};

}  // namespace charthistory
