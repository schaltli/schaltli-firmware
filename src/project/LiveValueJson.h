#pragma once

// Live values and combined topics read from the export (schaltli-designer
// docs/device-contract.md 2.6) - apart from LiveValue.h and CombinedTopics.h
// so that those stay free of ArduinoJson: ProjectTypes.h includes them, and
// the host tools that build ProjectTypes.h have no ArduinoJson.

#include <ArduinoJson.h>

#include <vector>

#include "CombinedTopics.h"
#include "LiveValue.h"

namespace livevalue {

// A live value as the export writes it.
LiveValue parseLiveValue(JsonObjectConst json);
std::vector<LiveValue> parseLiveValues(JsonArrayConst json);
Source parseSource(JsonObjectConst json);

}  // namespace livevalue

namespace combined {

std::vector<CombinedTopic> parse(JsonArrayConst json);

}  // namespace combined

namespace scaled {

// A scale or a write format as the export writes it (contract §2.11).
Scale parseScale(JsonObjectConst json);
WriteFormat parseWriteFormat(JsonObjectConst json);

}  // namespace scaled
