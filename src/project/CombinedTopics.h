#pragma once

// Combined topics: a yes/no the device works out itself from other values -
// all, or any, of its conditions apply (schaltli-designer
// docs/2026-10-07-live-values.md decisions 10-15, device contract §2.6).
// A port of the designer's lib/combined-topics.ts; the cases are the
// «combined» section of test/test_live_value/live-value-vectors.json.
//
// The export writes them already in evaluation order. evaluationOrder() is
// here anyway: the shared cases give them in any order, and a hand-edited
// file then still computes right.

#include <ArduinoJson.h>

#include <functional>
#include <map>
#include <optional>
#include <string>
#include <vector>

#include "project/LiveValue.h"

namespace combined {

struct Condition {
    livevalue::Source source;
    std::string op;
    std::string operand;
};

struct CombinedTopic {
    std::string id;
    std::string name;
    bool any = false;  // "any"; otherwise "all"
    std::vector<Condition> conditions;
};

// "true", "false", or nullopt for no value yet.
using Value = std::optional<std::string>;
using TopicLookup = std::function<std::optional<std::string>(const std::string& path)>;

// The topics in an order where each comes after every combined topic it
// reads; empty, with `circular` filled (the chain, from the first name and
// back to it), where none exists.
std::vector<CombinedTopic> evaluationOrder(const std::vector<CombinedTopic>& topics, std::vector<std::string>* circular = nullptr);

// One combined topic from what its conditions read.
Value evaluate(const CombinedTopic& topic, const std::function<std::optional<std::string>(const livevalue::Source&)>& read);

// Every one's value, computed in the order given - each reading a combined
// topic computed before it, a topic through `topic`.
std::map<std::string, Value> compute(const std::vector<CombinedTopic>& ordered, const TopicLookup& topic);

// Whether any of them reads `topicPath` (its `#json.path` left off).
bool readsTopic(const std::vector<CombinedTopic>& topics, const std::string& topicPath);

std::vector<CombinedTopic> parse(JsonArrayConst json);

}  // namespace combined
