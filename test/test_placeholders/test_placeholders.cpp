// The shared placeholder vectors (schaltli-designer lib/placeholders/
// vectors.json) against the firmware's evaluator, on the PC:
//
//   pio test -e native
//
// placeholder-vectors.json beside this file is a byte-for-byte copy of the
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

#include "project/Placeholders.h"

// pio test runs the program from the project directory.
static const char* COPY = "test/test_placeholders/placeholder-vectors.json";
static const char* DESIGNER = "../schaltli-designer/lib/placeholders/vectors.json";

static bool readFile(const char* path, std::string& out) {
    std::ifstream in(path, std::ios::binary);
    if (!in) return false;
    std::ostringstream buffer;
    buffer << in.rdbuf();
    out = buffer.str();
    return true;
}

void setUp() {}
void tearDown() {}

static void every_vector_resolves_as_in_the_designer() {
    std::string json;
    TEST_ASSERT_TRUE_MESSAGE(readFile(COPY, json), "placeholder-vectors.json not found - run from the project directory");
    JsonDocument doc;
    TEST_ASSERT_TRUE_MESSAGE(deserializeJson(doc, json) == DeserializationError::Ok, "placeholder-vectors.json does not parse");
    JsonArray cases = doc["cases"].as<JsonArray>();
    // An empty or truncated copy must not pass by having nothing to check.
    TEST_ASSERT_GREATER_THAN_MESSAGE(30, cases.size(), "fewer vectors than the designer has");

    int failures = 0;
    for (JsonObject vector : cases) {
        std::map<std::string, std::string> values;
        for (JsonPair value : vector["values"].as<JsonObject>()) values[value.key().c_str()] = value.value().as<std::string>();
        placeholders::Separators separators;
        if (vector["decimal"].is<const char*>()) separators.decimal = vector["decimal"].as<std::string>();
        if (vector["thousands"].is<const char*>()) separators.thousands = vector["thousands"].as<std::string>();
        auto lookup = [&](const placeholders::Reference& reference) -> std::optional<std::string> {
            auto found = values.find(reference.ns + ":" + reference.path);
            if (found == values.end()) return std::nullopt;
            return found->second;
        };
        const std::string actual = placeholders::resolve(vector["text"].as<std::string>(), lookup, separators);
        const std::string expected = vector["expected"].as<std::string>();
        if (actual != expected) {
            failures++;
            std::printf("FAIL %s\n  text     %s\n  expected %s\n  actual   %s\n", vector["name"].as<const char*>(),
                        vector["text"].as<const char*>(), expected.c_str(), actual.c_str());
        }
    }
    TEST_ASSERT_EQUAL_INT_MESSAGE(0, failures, "vectors failed, see above");
}

static void the_copy_is_the_designers_byte_for_byte() {
    std::string designer;
    if (!readFile(DESIGNER, designer)) {
        TEST_IGNORE_MESSAGE("schaltli-designer is not checked out beside this repo - the copy is not compared");
    }
    std::string copy;
    TEST_ASSERT_TRUE(readFile(COPY, copy));
    TEST_ASSERT_TRUE_MESSAGE(copy == designer,
                             "test/test_placeholders/placeholder-vectors.json differs from the designer's "
                             "lib/placeholders/vectors.json - copy it again, never edit it");
}

static void the_topics_a_text_refers_to_each_once() {
    auto topics = placeholders::referencedTopics("{topic:a/b:F1} {topic:c ?? 0} {topic:a/b} {device:id} {screen} {topic:van/data#temp}");
    TEST_ASSERT_EQUAL_size_t(3, topics.size());
    TEST_ASSERT_EQUAL_STRING("a/b", topics[0].c_str());
    TEST_ASSERT_EQUAL_STRING("c", topics[1].c_str());
    TEST_ASSERT_EQUAL_STRING("van/data#temp", topics[2].c_str());
}

int main(int, char**) {
    UNITY_BEGIN();
    RUN_TEST(every_vector_resolves_as_in_the_designer);
    RUN_TEST(the_copy_is_the_designers_byte_for_byte);
    RUN_TEST(the_topics_a_text_refers_to_each_once);
    return UNITY_END();
}
