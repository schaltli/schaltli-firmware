// The shared navigator vectors (schaltli-designer lib/navigator/vectors.json)
// against the firmware's NavigatorLayout, on the PC:
//
//   pio test -e native
//
// navigator-vectors.json beside this file is a byte-for-byte copy of the
// designer's. With the designer checked out beside this repo, a copy that
// differs fails here - copy it again, never edit it. Without the designer
// the check is skipped and says so.

#include <ArduinoJson.h>
#include <unity.h>

#include <cstdio>
#include <fstream>
#include <sstream>
#include <string>

#include "project/NavigatorLayout.h"

using namespace navigatorlayout;

static const char* COPY = "test/test_navigator/navigator-vectors.json";
static const char* DESIGNER = "../schaltli-designer/lib/navigator/vectors.json";

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

static Layout layoutOf(JsonObjectConst l) {
    return layout(edgeFrom(l["edge"].as<std::string>()), std::string(l["shows"] | "icons") == "iconsAndText", l["screenWidth"] | 0,
                  l["screenHeight"] | 0, l["count"] | 0);
}

static bool sameRect(const Rect& r, JsonObjectConst e) {
    return r.x == (e["x"] | 0) && r.y == (e["y"] | 0) && r.width == (e["width"] | 0) && r.height == (e["height"] | 0);
}

static void screens_every_vector() {
    int failures = 0;
    for (JsonObjectConst c : doc["screens"].as<JsonArrayConst>()) {
        std::vector<ListedScreen> screens;
        std::vector<std::string> ids;
        for (JsonObjectConst s : c["screens"].as<JsonArrayConst>()) {
            screens.push_back({s["isMaster"] | false, std::string(s["screenType"] | "") == "popup", s["hidden"] | false});
            ids.push_back(s["id"].as<std::string>());
        }
        std::string actual, expected;
        for (int i : listedScreens(screens)) actual += ids[i] + " ";
        for (JsonVariantConst id : c["expected"].as<JsonArrayConst>()) expected += id.as<std::string>() + " ";
        if (actual != expected) {
            failures++;
            std::printf("FAIL screens %s: expected [%s], actual [%s]\n", c["name"].as<const char*>(), expected.c_str(), actual.c_str());
        }
    }
    TEST_ASSERT_EQUAL_INT_MESSAGE(0, failures, "screens vectors failed, see above");
}

static void layout_every_vector() {
    JsonArrayConst cases = doc["layout"].as<JsonArrayConst>();
    TEST_ASSERT_GREATER_OR_EQUAL_MESSAGE(7, cases.size(), "fewer layout vectors than the designer has");
    int failures = 0;
    for (JsonObjectConst c : cases) {
        const Layout l = layoutOf(c["layout"]);
        JsonObjectConst e = c["expected"];
        if (!sameRect(l.strip, e["strip"]) || l.entryLength != (e["entryLength"] | -1) || maxScroll(l) != (e["maxScroll"] | -1)) {
            failures++;
            std::printf("FAIL layout %s: strip %d,%d %dx%d entry %d max %d\n", c["name"].as<const char*>(), l.strip.x, l.strip.y, l.strip.width,
                        l.strip.height, l.entryLength, maxScroll(l));
        }
    }
    TEST_ASSERT_EQUAL_INT_MESSAGE(0, failures, "layout vectors failed, see above");
}

static void geometry_every_vector() {
    int failures = 0, count = 0;
    for (JsonObjectConst c : doc["entryRect"].as<JsonArrayConst>()) {
        count++;
        const Rect r = entryRect(layoutOf(c["layout"]), c["index"] | 0, c["scroll"] | 0);
        if (!sameRect(r, c["expected"])) {
            failures++;
            std::printf("FAIL entryRect %s: %d,%d %dx%d\n", c["name"].as<const char*>(), r.x, r.y, r.width, r.height);
        }
    }
    for (JsonObjectConst c : doc["scrollToShow"].as<JsonArrayConst>()) {
        count++;
        const int actual = scrollToShow(layoutOf(c["layout"]), c["index"] | 0, c["scroll"] | 0);
        if (actual != (c["expected"] | -999)) {
            failures++;
            std::printf("FAIL scrollToShow %s: %d\n", c["name"].as<const char*>(), actual);
        }
    }
    for (JsonObjectConst c : doc["entryAt"].as<JsonArrayConst>()) {
        count++;
        const int actual = entryAt(layoutOf(c["layout"]), c["x"] | 0, c["y"] | 0, c["scroll"] | 0);
        if (actual != (c["expected"] | -999)) {
            failures++;
            std::printf("FAIL entryAt %s: %d\n", c["name"].as<const char*>(), actual);
        }
    }
    for (JsonObjectConst c : doc["pageScroll"].as<JsonArrayConst>()) {
        count++;
        const int actual = pageScroll(layoutOf(c["layout"]), c["scroll"] | 0, c["direction"] | 1);
        if (actual != (c["expected"] | -999)) {
            failures++;
            std::printf("FAIL pageScroll %s: %d\n", c["name"].as<const char*>(), actual);
        }
    }
    TEST_ASSERT_GREATER_OR_EQUAL_MESSAGE(20, count, "fewer geometry vectors than the designer has");
    TEST_ASSERT_EQUAL_INT_MESSAGE(0, failures, "geometry vectors failed, see above");
}

// Paging past hidden screens and the start screen (contract 2.7) - the
// firmware's own, not among the shared vectors.
static void paging_skips_hidden_screens_and_wraps() {
    const std::vector<bool> hidden = {false, true, false, true};
    TEST_ASSERT_EQUAL_INT(2, pagedIndex(hidden, 0, 1));
    TEST_ASSERT_EQUAL_INT(0, pagedIndex(hidden, 2, 1));
    TEST_ASSERT_EQUAL_INT(2, pagedIndex(hidden, 0, -1));
    // From a hidden screen reached by a goto, on to the next shown one.
    TEST_ASSERT_EQUAL_INT(2, pagedIndex(hidden, 1, 1));
    TEST_ASSERT_EQUAL_INT(0, pagedIndex(hidden, 1, -1));
    // Nothing shown: stay.
    TEST_ASSERT_EQUAL_INT(1, pagedIndex({true, true}, 1, 1));
    // Without hidden screens exactly the old modulo.
    TEST_ASSERT_EQUAL_INT(0, pagedIndex({false, false, false}, 2, 1));
}

static void a_device_starts_on_the_first_shown_screen() {
    TEST_ASSERT_EQUAL_INT(2, firstShown({true, true, false, false}));
    TEST_ASSERT_EQUAL_INT(0, firstShown({false, true}));
    TEST_ASSERT_EQUAL_INT(0, firstShown({true, true}));
    TEST_ASSERT_EQUAL_INT(0, firstShown({}));
}

static void the_copy_is_the_designers_byte_for_byte() {
    std::string designer;
    if (!readFile(DESIGNER, designer)) {
        TEST_IGNORE_MESSAGE("schaltli-designer is not checked out beside this repo - the copy is not compared");
    }
    std::string copy;
    TEST_ASSERT_TRUE(readFile(COPY, copy));
    TEST_ASSERT_TRUE_MESSAGE(copy == designer,
                             "test/test_navigator/navigator-vectors.json differs from the designer's "
                             "lib/navigator/vectors.json - copy it again, never edit it");
}

int main(int, char**) {
    UNITY_BEGIN();
    std::string json;
    if (!readFile(COPY, json) || deserializeJson(doc, json) != DeserializationError::Ok) {
        std::printf("navigator-vectors.json missing or unreadable - run from the project directory\n");
        return 1;
    }
    RUN_TEST(screens_every_vector);
    RUN_TEST(layout_every_vector);
    RUN_TEST(geometry_every_vector);
    RUN_TEST(paging_skips_hidden_screens_and_wraps);
    RUN_TEST(a_device_starts_on_the_first_shown_screen);
    RUN_TEST(the_copy_is_the_designers_byte_for_byte);
    return UNITY_END();
}
