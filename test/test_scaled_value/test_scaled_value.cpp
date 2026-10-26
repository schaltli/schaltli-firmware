// The shared scaled value vectors (schaltli-designer
// lib/scaled-value/vectors.json) against the firmware's ScaledValue and the
// scaled topics of CombinedTopics, on the PC:
//
//   pio test -e native
//
// scaled-value-vectors.json beside this file is a byte-for-byte copy of the
// designer's. With the designer checked out beside this repo, a copy that
// differs fails here - copy it again, never edit it. Without the designer
// the check is skipped and says so.

#include <ArduinoJson.h>
#include <unity.h>

#include <cstdio>
#include <fstream>
#include <sstream>
#include <string>

#include "project/LiveValueJson.h"
#include "project/ScaledValue.h"

static const char* COPY = "test/test_scaled_value/scaled-value-vectors.json";
static const char* DESIGNER = "../schaltli-designer/lib/scaled-value/vectors.json";

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
    TEST_ASSERT_TRUE_MESSAGE(readFile(COPY, json), "scaled-value-vectors.json not found - run from the project directory");
    TEST_ASSERT_TRUE_MESSAGE(deserializeJson(doc, json) == DeserializationError::Ok, "scaled-value-vectors.json does not parse");
}

static std::string shown(const std::optional<std::string>& value) { return value ? "\"" + *value + "\"" : "(none)"; }

static std::optional<std::string> resultOf(JsonObjectConst vector) {
    if (vector["result"].isNull()) return std::nullopt;
    return vector["result"].as<std::string>();
}

static void scale_every_vector() {
    JsonArrayConst cases = doc["scale"].as<JsonArrayConst>();
    TEST_ASSERT_GREATER_THAN_MESSAGE(40, cases.size(), "fewer scale vectors than the designer has");
    int failures = 0;
    for (JsonObjectConst vector : cases) {
        scaled::Scale scale;
        scale.numerator = vector["numerator"].as<int64_t>();
        scale.denominator = vector["denominator"].as<int64_t>();
        scale.offset = vector["offset"].as<std::string>();
        scale.decimals = vector["decimals"].as<int>();
        const auto got = scaled::scaleValue(vector["value"].as<std::string>(), scale);
        const auto want = resultOf(vector);
        if (got != want) {
            failures++;
            std::printf("FAIL scale \"%s\" x %lld/%lld + %s, %d: expected %s, actual %s\n", vector["value"].as<const char*>(),
                        static_cast<long long>(scale.numerator), static_cast<long long>(scale.denominator), scale.offset.c_str(), scale.decimals,
                        shown(want).c_str(), shown(got).c_str());
        }
    }
    TEST_ASSERT_EQUAL_INT_MESSAGE(0, failures, "scale vectors failed, see above");
}

static void write_every_vector() {
    JsonArrayConst cases = doc["write"].as<JsonArrayConst>();
    TEST_ASSERT_GREATER_THAN_MESSAGE(5, cases.size(), "fewer write vectors than the designer has");
    int failures = 0;
    for (JsonObjectConst vector : cases) {
        const scaled::WriteFormat format = scaled::parseWriteFormat(vector["format"].as<JsonObjectConst>());
        const auto got = scaled::formatWrite(vector["value"].as<std::string>(), &format);
        const auto want = resultOf(vector);
        if (got != want) {
            failures++;
            std::printf("FAIL write \"%s\" as %s: expected %s, actual %s\n", vector["value"].as<const char*>(), format.text.c_str(), shown(want).c_str(),
                        shown(got).c_str());
        }
    }
    TEST_ASSERT_EQUAL_INT_MESSAGE(0, failures, "write vectors failed, see above");
    TEST_ASSERT_EQUAL_STRING("21.5", scaled::formatWrite("21.5", nullptr)->c_str());
}

static void combined_every_vector() {
    JsonArrayConst cases = doc["combined"].as<JsonArrayConst>();
    TEST_ASSERT_GREATER_THAN_MESSAGE(5, cases.size(), "fewer combined vectors than the designer has");
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

// A scaled topic's source is subscribed to and redrawn for, as a condition's is.
static void a_scaled_topic_reads_its_source() {
    JsonDocument json;
    deserializeJson(json, R"([
      {"id":"c1","name":"celsius","mode":"scale","conditions":[],
       "scale":{"source":{"namespace":"topic","path":"weather#kelvin"},"numerator":1,"denominator":1,"offset":"-273.15","decimals":2}},
      {"id":"c2","name":"fahrenheit","mode":"scale","conditions":[],
       "scale":{"source":{"namespace":"combined","path":"celsius"},"numerator":9,"denominator":5,"offset":"32","decimals":1}}
    ])");
    const auto topics = combined::parse(json.as<JsonArrayConst>());
    std::string joined;
    for (const std::string& t : combined::inputTopics(topics, "fahrenheit")) joined += t + " ";
    TEST_ASSERT_EQUAL_STRING("weather ", joined.c_str());
    TEST_ASSERT_TRUE(combined::readsTopic(topics, "weather"));
    TEST_ASSERT_FALSE(combined::readsTopic(topics, "kelvin"));
}

// A factor that is not a whole number, or none at all where one is needed,
// is not a scale a device guesses at: no value.
static void a_broken_scale_gives_no_value() {
    JsonDocument json;
    deserializeJson(json, R"({"text":"{value}","numerator":2.5})");
    const scaled::WriteFormat format = scaled::parseWriteFormat(json.as<JsonObjectConst>());
    TEST_ASSERT_FALSE(scaled::formatWrite("4", &format).has_value());
}

static void the_copy_is_the_designers_byte_for_byte() {
    std::string designer;
    if (!readFile(DESIGNER, designer)) {
        TEST_IGNORE_MESSAGE("schaltli-designer is not checked out beside this repo - the copy is not compared");
    }
    std::string copy;
    TEST_ASSERT_TRUE(readFile(COPY, copy));
    TEST_ASSERT_TRUE_MESSAGE(copy == designer,
                             "test/test_scaled_value/scaled-value-vectors.json differs from the designer's "
                             "lib/scaled-value/vectors.json - copy it again, never edit it");
}

int main(int, char**) {
    UNITY_BEGIN();
    load();
    RUN_TEST(scale_every_vector);
    RUN_TEST(write_every_vector);
    RUN_TEST(combined_every_vector);
    RUN_TEST(a_scaled_topic_reads_its_source);
    RUN_TEST(a_broken_scale_gives_no_value);
    RUN_TEST(the_copy_is_the_designers_byte_for_byte);
    return UNITY_END();
}
