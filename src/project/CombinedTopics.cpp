#include "project/CombinedTopics.h"

#include <algorithm>

// The designer's lib/combined-topics.ts, line by line where it can be.

namespace combined {

static std::vector<std::string> combinedUses(const CombinedTopic& topic) {
    std::vector<std::string> names;
    for (const Condition& c : topic.conditions)
        if (c.source.ns == "combined" && std::find(names.begin(), names.end(), c.source.path) == names.end()) names.push_back(c.source.path);
    return names;
}

std::vector<CombinedTopic> evaluationOrder(const std::vector<CombinedTopic>& topics, std::vector<std::string>* circular) {
    std::map<std::string, const CombinedTopic*> byName;
    for (const CombinedTopic& t : topics) byName[t.name] = &t;
    std::vector<CombinedTopic> order;
    std::map<std::string, int> state;  // 1 visiting, 2 done
    std::vector<std::string> cycle;

    std::function<void(const std::string&, std::vector<std::string>)> visit = [&](const std::string& name, std::vector<std::string> path) {
        if (!cycle.empty()) return;
        auto found = byName.find(name);
        if (found == byName.end()) return;
        if (state[name] == 2) return;
        if (state[name] == 1) {
            auto at = std::find(path.begin(), path.end(), name);
            std::vector<std::string> loop(at, path.end());
            // The same cycle reads the same whichever topic the walk began at.
            size_t start = 0;
            for (size_t i = 1; i < loop.size(); i++)
                if (loop[i] < loop[start]) start = i;
            for (size_t i = 0; i < loop.size(); i++) cycle.push_back(loop[(start + i) % loop.size()]);
            cycle.push_back(cycle.front());
            return;
        }
        state[name] = 1;
        path.push_back(name);
        for (const std::string& used : combinedUses(*found->second)) {
            visit(used, path);
            if (!cycle.empty()) return;
        }
        state[name] = 2;
        order.push_back(*found->second);
    };
    for (const CombinedTopic& t : topics) visit(t.name, {});
    if (!cycle.empty()) {
        if (circular) *circular = cycle;
        return {};
    }
    return order;
}

Value evaluate(const CombinedTopic& topic, const std::function<std::optional<std::string>(const livevalue::Source&)>& read) {
    if (topic.conditions.empty()) return std::nullopt;
    bool unknown = false;
    for (const Condition& condition : topic.conditions) {
        const auto value = read(condition.source);
        if (!value) {
            unknown = true;
            continue;
        }
        const bool yes = livevalue::matches(*value, condition.op, condition.operand);
        if (topic.any && yes) return std::string("true");
        if (!topic.any && !yes) return std::string("false");
    }
    if (unknown) return std::nullopt;
    return std::string(topic.any ? "false" : "true");
}

std::map<std::string, Value> compute(const std::vector<CombinedTopic>& ordered, const TopicLookup& topic) {
    std::map<std::string, Value> values;
    auto read = [&](const livevalue::Source& source) -> std::optional<std::string> {
        if (source.ns == "combined") {
            auto found = values.find(source.path);
            return found == values.end() ? std::nullopt : found->second;
        }
        if (source.ns == "topic") return topic(source.path);
        return std::nullopt;
    };
    for (const CombinedTopic& t : ordered) values[t.name] = evaluate(t, read);
    return values;
}

bool readsTopic(const std::vector<CombinedTopic>& topics, const std::string& topicPath) {
    for (const CombinedTopic& t : topics)
        for (const Condition& c : t.conditions)
            if (c.source.ns == "topic" && c.source.path.substr(0, c.source.path.find('#')) == topicPath) return true;
    return false;
}

std::vector<CombinedTopic> parse(JsonArrayConst json) {
    std::vector<CombinedTopic> out;
    for (JsonObjectConst t : json) {
        CombinedTopic topic;
        topic.id = t["id"] | "";
        topic.name = t["name"] | "";
        topic.any = std::string(t["mode"] | "all") == "any";
        for (JsonObjectConst c : t["conditions"].as<JsonArrayConst>()) {
            topic.conditions.push_back({livevalue::parseSource(c["source"].as<JsonObjectConst>()), c["op"] | "==", c["operand"] | ""});
        }
        out.push_back(topic);
    }
    return out;
}

}  // namespace combined
