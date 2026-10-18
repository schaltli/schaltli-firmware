// The shared screen graph vectors (schaltli-designer lib/screen-graph/vectors.json)
// against the firmware's ScreenGraph, on the PC:
//
//   pio test -e native
//
// screen-graph-vectors.json beside this file is a byte-for-byte copy of the
// designer's. With the designer checked out beside this repo, a copy that
// differs fails here - copy it again, never edit it. Without the designer
// the check is skipped and says so. The tab's colour is the designer's to
// work out (the export carries it); its cases are not run here.

#include <ArduinoJson.h>
#include <unity.h>

#include <cstdio>
#include <fstream>
#include <sstream>
#include <string>

#include "project/ScreenGraph.h"

using namespace screengraph;

static const char* COPY = "test/test_screen_graph/screen-graph-vectors.json";
static const char* DESIGNER = "../schaltli-designer/lib/screen-graph/vectors.json";

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

static std::vector<std::string> strings(JsonArrayConst a) {
    std::vector<std::string> out;
    for (JsonVariantConst v : a) out.push_back(v.as<std::string>());
    return out;
}

static Edge edgeOf(JsonVariantConst v) {
    Edge e = Edge::Up;
    TEST_ASSERT_TRUE(edgeFrom(v.as<std::string>(), e));
    return e;
}

static bool sameRect(const Rect& r, JsonObjectConst e) {
    return r.x == (e["x"] | -1) && r.y == (e["y"] | -1) && r.width == (e["width"] | -1) && r.height == (e["height"] | -1);
}

static void the_numbers_are_the_designers() {
    JsonObjectConst t = doc["pullTab"];
    TEST_ASSERT_EQUAL(TAB_LENGTH, t["length"] | 0);
    TEST_ASSERT_EQUAL(TAB_THICKNESS, t["thickness"] | 0);
    TEST_ASSERT_EQUAL(TAB_RADIUS, t["radius"] | 0);
    TEST_ASSERT_EQUAL(TAB_INSET, t["inset"] | 0);
    TEST_ASSERT_EQUAL(TAB_NUDGE, t["nudge"] | 0);
    TEST_ASSERT_EQUAL(TAB_PERIOD_MS, t["period"] | 0);
    TEST_ASSERT_EQUAL(TAB_NUDGE_MS, t["nudgeMs"] | 0);
}

static void row_step_every_vector() {
    int failures = 0;
    for (JsonObjectConst c : doc["rowStep"].as<JsonArrayConst>()) {
        std::string actual = rowStep(strings(c["mainRow"]), c["current"].as<std::string>(), c["page"] | "", c["delta"] | 0);
        std::string expected = c["expect"].isNull() ? "" : c["expect"].as<std::string>();
        if (actual != expected) {
            std::printf("rowStep %s %d: %s, expected %s\n", c["current"].as<const char*>(), c["delta"] | 0, actual.c_str(), expected.c_str());
            failures++;
        }
    }
    TEST_ASSERT_EQUAL(0, failures);
}

static void tab_sides_every_vector() {
    int failures = 0;
    for (JsonObjectConst c : doc["tabSides"].as<JsonArrayConst>()) {
        JsonObjectConst s = c["screen"];
        GraphScreen screen;
        screen.id = s["id"].as<std::string>();
        for (JsonPairConst n : s["neighbours"].as<JsonObjectConst>()) {
            Edge e;
            if (edgeFrom(n.key().c_str(), e)) screen.neighbours[static_cast<int>(e)] = n.value().as<std::string>();
        }
        if (!s["back"].isNull()) {
            screen.hasBack = edgeFrom(s["back"].as<std::string>(), screen.back);
        }
        for (JsonPairConst a : s["buttonActions"].as<JsonObjectConst>()) {
            screen.swipes[a.key().c_str()] = {a.value()["type"] | "", a.value()["side"] | "", a.value()["targetScreenId"] | ""};
        }
        std::string actual, expected;
        for (Edge e : tabSides(screen, strings(c["mainRow"]))) actual += std::string(edgeName(e)) + " ";
        for (JsonVariantConst e : c["expect"].as<JsonArrayConst>()) expected += e.as<std::string>() + " ";
        if (actual != expected) {
            std::printf("tabSides %s: [%s], expected [%s]\n", c["name"].as<const char*>(), actual.c_str(), expected.c_str());
            failures++;
        }
    }
    TEST_ASSERT_EQUAL(0, failures);
}

static void geometry_every_vector() {
    int failures = 0;
    for (JsonObjectConst c : doc["contentArea"].as<JsonArrayConst>()) {
        if (!sameRect(contentArea(c["width"] | 0, c["height"] | 0, c["edge"] | "", c["thickness"] | 0), c["expect"])) {
            std::printf("contentArea %s %d\n", c["edge"] | "-", c["thickness"] | 0);
            failures++;
        }
    }
    for (JsonObjectConst c : doc["tabRect"].as<JsonArrayConst>()) {
        JsonObjectConst a = c["area"];
        Rect area{a["x"] | 0, a["y"] | 0, a["width"] | 0, a["height"] | 0};
        if (!sameRect(tabRect(edgeOf(c["edge"]), area, c["nudge"] | 0), c["expect"])) {
            std::printf("tabRect %s at %d,%d %dx%d nudge %d\n", c["edge"].as<const char*>(), area.x, area.y, area.width, area.height, c["nudge"] | 0);
            failures++;
        }
    }
    for (JsonObjectConst c : doc["nudge"].as<JsonArrayConst>()) {
        long ms = c["ms"] | 0L;
        if (nudgeAt(ms) != (c["expect"] | -1)) {
            std::printf("nudge %ld: %d, expected %d\n", ms, nudgeAt(ms), c["expect"] | -1);
            failures++;
        }
    }
    TEST_ASSERT_EQUAL(0, failures);
}

static void a_swipe_id_and_its_side_go_both_ways() {
    for (Edge e : {Edge::Up, Edge::Down, Edge::Left, Edge::Right}) {
        Edge back;
        TEST_ASSERT_TRUE(sideOfSwipe(swipeFor(e), back));
        TEST_ASSERT_TRUE(back == e);
    }
    Edge none;
    TEST_ASSERT_FALSE(sideOfSwipe("button-0", none));
}

static void the_copy_is_the_designers_byte_for_byte() {
    std::string designer;
    if (!readFile(DESIGNER, designer)) {
        TEST_IGNORE_MESSAGE("schaltli-designer not checked out beside this repo - the copy is not compared");
        return;
    }
    std::string copy;
    TEST_ASSERT_TRUE(readFile(COPY, copy));
    TEST_ASSERT_TRUE_MESSAGE(copy == designer,
                             "test/test_screen_graph/screen-graph-vectors.json differs from the designer's "
                             "lib/screen-graph/vectors.json - copy it again, never edit it");
}

int main(int, char**) {
    UNITY_BEGIN();
    std::string json;
    if (!readFile(COPY, json) || deserializeJson(doc, json) != DeserializationError::Ok) {
        std::printf("screen-graph-vectors.json missing or unreadable - run from the project directory\n");
        return 1;
    }
    RUN_TEST(the_numbers_are_the_designers);
    RUN_TEST(row_step_every_vector);
    RUN_TEST(tab_sides_every_vector);
    RUN_TEST(geometry_every_vector);
    RUN_TEST(a_swipe_id_and_its_side_go_both_ways);
    RUN_TEST(the_copy_is_the_designers_byte_for_byte);
    return UNITY_END();
}
