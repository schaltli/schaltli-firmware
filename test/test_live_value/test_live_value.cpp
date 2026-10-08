// The shared live value vectors (schaltli-designer lib/live-value/vectors.json)
// against the firmware's LiveValue and CombinedTopics, on the PC:
//
//   pio test -e native
//
// live-value-vectors.json beside this file is a byte-for-byte copy of the
// designer's. With the designer checked out beside this repo, a copy that
// differs fails here - copy it again, never edit it. Without the designer
// the check is skipped and says so.

#include <ArduinoJson.h>
#include <unity.h>

#include <cstdio>
#include <fstream>
#include <map>
#include <sstream>
#include <string>

#include "project/LiveValueJson.h"

static const char* COPY = "test/test_live_value/live-value-vectors.json";
static const char* DESIGNER = "../schaltli-designer/lib/live-value/vectors.json";

static bool readFile(const char* path, std::string& out) {
    std::ifstream in(path, std::ios::binary);
    if (!in) return false;
    std::ostringstream buffer;
    buffer << in.rdbuf();
    out = buffer.str();
    return true;
}

static JsonDocument doc;

void setUp() {}
void tearDown() {}

static void load() {
    std::string json;
    TEST_ASSERT_TRUE_MESSAGE(readFile(COPY, json), "live-value-vectors.json not found - run from the project directory");
    TEST_ASSERT_TRUE_MESSAGE(deserializeJson(doc, json) == DeserializationError::Ok, "live-value-vectors.json does not parse");
}

static placeholders::Separators separatorsOf(JsonObjectConst vector) {
    placeholders::Separators separators;
    if (vector["decimal"].is<const char*>()) separators.decimal = vector["decimal"].as<std::string>();
    if (vector["thousands"].is<const char*>()) separators.thousands = vector["thousands"].as<std::string>();
    return separators;
}

static std::optional<std::string> valueOf(JsonObjectConst vector) {
    if (!vector["value"].is<const char*>()) return std::nullopt;
    return vector["value"].as<std::string>();
}

static void evaluate_every_vector() {
    JsonArrayConst cases = doc["evaluate"].as<JsonArrayConst>();
    TEST_ASSERT_GREATER_THAN_MESSAGE(30, cases.size(), "fewer evaluate vectors than the designer has");
    int failures = 0;
    for (JsonObjectConst vector : cases) {
        livevalue::LiveValue lv;
        lv.id = "lv1";
        for (JsonObjectConst rule : vector["rules"].as<JsonArrayConst>()) lv.rules.push_back({rule["op"] | "==", rule["operand"] | "", {}});
        const livevalue::Applies applies = livevalue::evaluate(lv, valueOf(vector));
        std::string actual = applies.branch == livevalue::Applies::NoValueYet ? "noValueYet"
                           : applies.branch == livevalue::Applies::Otherwise  ? "otherwise"
                                                                               : std::to_string(applies.rule);
        std::string expected = vector["applies"].is<int>() ? std::to_string(vector["applies"].as<int>()) : vector["applies"].as<std::string>();
        if (actual != expected) {
            failures++;
            std::printf("FAIL evaluate %s: expected %s, actual %s\n", vector["name"].as<const char*>(), expected.c_str(), actual.c_str());
        }
    }
    TEST_ASSERT_EQUAL_INT_MESSAGE(0, failures, "evaluate vectors failed, see above");
}

static void text_every_vector() {
    JsonArrayConst cases = doc["text"].as<JsonArrayConst>();
    TEST_ASSERT_GREATER_THAN_MESSAGE(20, cases.size(), "fewer text vectors than the designer has");
    int failures = 0;
    for (JsonObjectConst vector : cases) {
        const livevalue::LiveValue lv = livevalue::parseLiveValue(vector["liveValue"].as<JsonObjectConst>());
        const std::string actual = livevalue::textOf(lv, valueOf(vector), separatorsOf(vector));
        const std::string expected = vector["expected"].as<std::string>();
        if (actual != expected) {
            failures++;
            std::printf("FAIL text %s: expected [%s], actual [%s]\n", vector["name"].as<const char*>(), expected.c_str(), actual.c_str());
        }
    }
    TEST_ASSERT_EQUAL_INT_MESSAGE(0, failures, "text vectors failed, see above");
}

static void resolve_every_vector() {
    JsonArrayConst cases = doc["resolve"].as<JsonArrayConst>();
    TEST_ASSERT_GREATER_THAN_MESSAGE(5, cases.size(), "fewer resolve vectors than the designer has");
    int failures = 0;
    for (JsonObjectConst vector : cases) {
        const auto liveValues = livevalue::parseLiveValues(vector["liveValues"].as<JsonArrayConst>());
        JsonObjectConst values = vector["values"].as<JsonObjectConst>();
        auto lookup = [&](const livevalue::Source& source) -> std::optional<std::string> {
            const std::string key = source.ns + ":" + source.path;
            if (!values[key].is<const char*>()) return std::nullopt;
            return values[key].as<std::string>();
        };
        const std::string actual = livevalue::resolveLiveText(vector["text"].as<std::string>(), liveValues, lookup, placeholders::Separators{});
        const std::string expected = vector["expected"].as<std::string>();
        if (actual != expected) {
            failures++;
            std::printf("FAIL resolve %s: expected [%s], actual [%s]\n", vector["name"].as<const char*>(), expected.c_str(), actual.c_str());
        }
    }
    TEST_ASSERT_EQUAL_INT_MESSAGE(0, failures, "resolve vectors failed, see above");
}

static void combined_every_vector() {
    JsonArrayConst cases = doc["combined"].as<JsonArrayConst>();
    TEST_ASSERT_GREATER_THAN_MESSAGE(10, cases.size(), "fewer combined vectors than the designer has");
    int failures = 0;
    for (JsonObjectConst vector : cases) {
        const auto topics = combined::parse(vector["combinedTopics"].as<JsonArrayConst>());
        std::vector<std::string> circular;
        const auto ordered = combined::evaluationOrder(topics, &circular);
        if (vector["circular"].is<JsonArrayConst>()) {
            std::string expected, actual;
            for (JsonVariantConst name : vector["circular"].as<JsonArrayConst>()) expected += name.as<std::string>() + " ";
            for (const std::string& name : circular) actual += name + " ";
            if (actual != expected) {
                failures++;
                std::printf("FAIL combined %s: circular expected [%s], actual [%s]\n", vector["name"].as<const char*>(), expected.c_str(), actual.c_str());
            }
            continue;
        }
        JsonObjectConst values = vector["values"].as<JsonObjectConst>();
        const auto computed = combined::compute(ordered, [&](const std::string& path) -> std::optional<std::string> {
            if (!values[path].is<const char*>()) return std::nullopt;
            return values[path].as<std::string>();
        });
        for (JsonPairConst expected : vector["expected"].as<JsonObjectConst>()) {
            const std::string want = expected.value().isNull() ? "(none)" : expected.value().as<std::string>();
            auto found = computed.find(expected.key().c_str());
            const std::string got = (found == computed.end() || !found->second) ? "(none)" : *found->second;
            if (got != want) {
                failures++;
                std::printf("FAIL combined %s: %s expected %s, actual %s\n", vector["name"].as<const char*>(), expected.key().c_str(), want.c_str(), got.c_str());
            }
        }
    }
    TEST_ASSERT_EQUAL_INT_MESSAGE(0, failures, "combined vectors failed, see above");
}

// What a device subscribes to and redraws for: a combined topic's own topics
// and those of the combined topics it reads, bare, each once.
static void input_topics_follow_combined_topics_down() {
    JsonDocument json;
    deserializeJson(json, R"([
      {"id":"c1","name":"anyLight","mode":"any","conditions":[
        {"source":{"namespace":"topic","path":"light/a"},"op":"yes"},
        {"source":{"namespace":"topic","path":"light/b#state"},"op":"yes"}]},
      {"id":"c2","name":"alarm","mode":"all","conditions":[
        {"source":{"namespace":"combined","path":"anyLight"},"op":"yes"},
        {"source":{"namespace":"topic","path":"light/a"},"op":"yes"},
        {"source":{"namespace":"topic","path":"night"},"op":"yes"}]}
    ])");
    const auto topics = combined::parse(json.as<JsonArrayConst>());
    std::string joined;
    for (const std::string& t : combined::inputTopics(topics, "alarm")) joined += t + " ";
    TEST_ASSERT_EQUAL_STRING("light/a light/b night ", joined.c_str());
    TEST_ASSERT_TRUE(combined::inputTopics(topics, "missing").empty());
}

static void the_copy_is_the_designers_byte_for_byte() {
    std::string designer;
    if (!readFile(DESIGNER, designer)) {
        TEST_IGNORE_MESSAGE("schaltli-designer is not checked out beside this repo - the copy is not compared");
    }
    std::string copy;
    TEST_ASSERT_TRUE(readFile(COPY, copy));
    TEST_ASSERT_TRUE_MESSAGE(copy == designer,
                             "test/test_live_value/live-value-vectors.json differs from the designer's "
                             "lib/live-value/vectors.json - copy it again, never edit it");
}

int main(int, char**) {
    UNITY_BEGIN();
    load();
    RUN_TEST(evaluate_every_vector);
    RUN_TEST(text_every_vector);
    RUN_TEST(resolve_every_vector);
    RUN_TEST(combined_every_vector);
    RUN_TEST(input_topics_follow_combined_topics_down);
    RUN_TEST(the_copy_is_the_designers_byte_for_byte);
    return UNITY_END();
}
