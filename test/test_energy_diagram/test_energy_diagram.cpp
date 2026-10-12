// The shared energy diagram vectors (schaltli-designer
// lib/energy-diagram/vectors.json) against the firmware's EnergyDiagram, on
// the PC:
//
//   pio test -e native
//
// energy-diagram-vectors.json beside this file is a byte-for-byte copy of
// the designer's. With the designer checked out beside this repo, a copy
// that differs fails here - copy it again, never edit it. Without the
// designer the check is skipped and says so.

#include <ArduinoJson.h>
#include <unity.h>

#include <cmath>
#include <cstdio>
#include <fstream>
#include <sstream>
#include <string>

#include "project/EnergyDiagram.h"
#include "project/PolygonRaster.h"

using namespace energydiagram;

static const char* COPY = "test/test_energy_diagram/energy-diagram-vectors.json";
static const char* DESIGNER = "../schaltli-designer/lib/energy-diagram/vectors.json";

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

// The designer's toBeCloseTo(x, 1): within 0.05.
static bool near(double a, double b, double within = 0.05) { return std::fabs(a - b) < within; }

// The vectors' nodes, with `lines` lines each where a case says so.
static std::vector<Node> nodesOf(JsonObjectConst lines = JsonObjectConst()) {
    std::vector<Node> out;
    for (JsonObjectConst n : doc["nodes"].as<JsonArrayConst>()) {
        Node node;
        node.id = n["id"].as<std::string>();
        node.role = roleFrom(n["role"].as<std::string>());
        node.x = n["x"] | 0.0;
        node.y = n["y"] | 0.0;
        node.reverseSign = n["reverseSign"] | false;
        node.lines = lines.isNull() ? 0 : (lines[node.id] | 0);
        out.push_back(node);
    }
    return out;
}

static std::vector<Value> valuesOf(const std::vector<Node>& nodes, JsonObjectConst values) {
    std::vector<Value> out;
    for (const Node& n : nodes) {
        Value v;
        JsonObjectConst e = values[n.id];
        if (!e.isNull()) {
            if (e["watts"].is<double>()) {
                v.hasWatts = true;
                v.watts = e["watts"].as<double>();
            }
            if (e["charge"].is<double>()) {
                v.hasCharge = true;
                v.charge = e["charge"].as<double>();
            }
        }
        out.push_back(v);
    }
    return out;
}

static const char* alignName(Align a) { return a == Align::Left ? "left" : a == Align::Right ? "right" : "center"; }

static void the_copy_is_the_designers() {
    std::string copy, designer;
    TEST_ASSERT_TRUE_MESSAGE(readFile(COPY, copy), "the vectors' copy is missing");
    if (!readFile(DESIGNER, designer)) {
        TEST_IGNORE_MESSAGE("schaltli-designer not checked out beside this repo: the copy is not compared");
        return;
    }
    TEST_ASSERT_TRUE_MESSAGE(copy == designer, "energy-diagram-vectors.json differs from the designer's - copy it again");
}

static void resolve_every_vector() {
    JsonArrayConst cases = doc["resolve"].as<JsonArrayConst>();
    TEST_ASSERT_GREATER_OR_EQUAL_MESSAGE(4, cases.size(), "fewer resolve vectors than the designer has");
    int failures = 0;
    const std::vector<Node> nodes = nodesOf();
    for (JsonObjectConst c : cases) {
        for (const Resolved& r : resolve(nodes, valuesOf(nodes, c["values"]))) {
            JsonArrayConst e = c["expected"][r.id];
            if (!near(r.watts, e[0] | -1.0, 0.001) || r.gives != (e[1] | false)) {
                failures++;
                std::printf("FAIL resolve %s: %s %.2f %d\n", c["name"].as<const char*>(), r.id.c_str(), r.watts, r.gives);
            }
        }
    }
    TEST_ASSERT_EQUAL_INT_MESSAGE(0, failures, "resolve vectors failed, see above");
}

static void sankey_every_vector() {
    int failures = 0;
    const std::vector<Node> nodes = nodesOf();
    for (JsonObjectConst c : doc["sankey"].as<JsonArrayConst>()) {
        const Size size{c["size"]["width"] | 0.0, c["size"]["height"] | 0.0};
        const SankeyLayout l = sankeyLayout(resolve(nodes, valuesOf(nodes, c["values"])), size, c["fullScale"] | 1000.0);
        JsonArrayConst bars = c["expected"]["bars"].as<JsonArrayConst>();
        bool ok = l.bars.size() == bars.size();
        for (size_t i = 0; ok && i < bars.size(); i++) {
            JsonObjectConst e = bars[i];
            const Bar& b = l.bars[i];
            ok = b.id == e["id"].as<std::string>() && (b.side == Side::Left) == (std::string(e["side"] | "") == "left") &&
                 near(b.box.x, e["x"] | 0.0) && near(b.box.y, e["y"] | 0.0) && near(b.box.height, e["height"] | 0.0);
        }
        JsonObjectConst bus = c["expected"]["bus"];
        ok = ok && near(l.bus.x, bus["x"] | 0.0) && near(l.bus.y, bus["y"] | 0.0) && near(l.bus.width, bus["width"] | 0.0) &&
             near(l.bus.height, bus["height"] | 0.0);
        if (!ok) {
            failures++;
            std::printf("FAIL sankey %s\n", c["name"].as<const char*>());
            for (const Bar& b : l.bars) std::printf("  bar %s %.2f %.2f %.2f\n", b.id.c_str(), b.box.x, b.box.y, b.box.height);
            std::printf("  bus %.2f %.2f %.2f %.2f\n", l.bus.x, l.bus.y, l.bus.width, l.bus.height);
        }
    }
    TEST_ASSERT_EQUAL_INT_MESSAGE(0, failures, "sankey vectors failed, see above");
}

static void flow_every_vector() {
    int failures = 0;
    const std::vector<Node> nodes = nodesOf();
    for (JsonObjectConst c : doc["flowLines"].as<JsonArrayConst>()) {
        const FlowLayout l = flowLayout(nodes, resolve(nodes, valuesOf(nodes, c["values"])));
        const std::string id = c["id"].as<std::string>();
        JsonObjectConst e = c["expected"];
        bool ok = false;
        for (const FlowLine& line : l.lines) {
            if (line.id != id) continue;
            ok = near(line.x0, e["x0"] | 0.0) && near(line.y0, e["y0"] | 0.0) && near(line.x1, e["x1"] | 0.0) && near(line.y1, e["y1"] | 0.0) &&
                 near(line.width, e["width"] | 0.0) && line.live == (e["live"] | false);
        }
        if (!ok) {
            failures++;
            std::printf("FAIL flow %s\n", c["name"].as<const char*>());
        }
    }
    for (JsonObjectConst c : doc["flowLabels"].as<JsonArrayConst>()) {
        const FlowLayout l = flowLayout(nodes, resolve(nodes, {}));
        JsonObjectConst e = c["expected"];
        bool ok = false;
        for (const FlowNode& n : l.nodes) {
            if (n.id != c["id"].as<std::string>()) continue;
            ok = near(n.labelX, e["x"] | 0.0, 0.001) && near(n.labelY, e["y"] | 0.0, 0.001) &&
                 n.labelBeside == (std::string(e["align"] | "") == "start");
        }
        if (!ok) {
            failures++;
            std::printf("FAIL flow label %s\n", c["name"].as<const char*>());
        }
    }
    TEST_ASSERT_EQUAL_INT_MESSAGE(0, failures, "flow vectors failed, see above");
}

static bool sameBox(const Box& b, JsonObjectConst e) {
    return near(b.x, e["x"] | -1.0, 0.001) && near(b.y, e["y"] | -1.0, 0.001) && near(b.width, e["width"] | -1.0, 0.001) &&
           near(b.height, e["height"] | -1.0, 0.001);
}

static bool sameLabel(const LabelBox& b, JsonObjectConst e) {
    return near(b.x, e["x"] | -1.0, 0.001) && near(b.y, e["y"] | -1.0, 0.001) && near(b.width, e["width"] | -1.0, 0.001) &&
           std::string(alignName(b.align)) == (e["align"] | "");
}

static void boxes_every_vector() {
    JsonArrayConst cases = doc["boxes"].as<JsonArrayConst>();
    TEST_ASSERT_GREATER_OR_EQUAL_MESSAGE(4, cases.size(), "fewer boxes vectors than the designer has");
    int failures = 0;
    for (JsonObjectConst c : cases) {
        const std::vector<Node> nodes = nodesOf(c["lines"]);
        const std::vector<Resolved> r = resolve(nodes, valuesOf(nodes, c["values"]));
        const LabelMetrics m{c["metrics"]["fontSize"] | 16.0, c["metrics"]["valueFontSize"] | 16.0};
        DiagramBoxes b;
        if (std::string(c["mode"] | "") == "flow") b = flowBoxes(flowLayout(nodes, r), m);
        else {
            const Size size{c["size"]["width"] | 0.0, c["size"]["height"] | 0.0};
            b = sankeyBoxes(sankeyLayout(r, size, c["fullScale"] | 1000.0), nodes, size, m);
        }
        bool ok = c["expected"]["bus"].isNull() ? !b.hasBus : (b.hasBus && sameLabel(b.bus, c["expected"]["bus"]));
        for (JsonPairConst kv : c["expected"]["nodes"].as<JsonObjectConst>()) {
            bool found = false;
            for (const NodeBoxes& n : b.nodes) {
                if (n.id != kv.key().c_str()) continue;
                found = sameBox(n.icon, kv.value()["icon"]) && sameLabel(n.label, kv.value()["label"]);
                if (!found) std::printf("  %s icon %.0f,%.0f %.0f label %.0f,%.0f %.0f %s\n", n.id.c_str(), n.icon.x, n.icon.y, n.icon.width, n.label.x,
                                        n.label.y, n.label.width, alignName(n.label.align));
            }
            ok = ok && found;
        }
        if (!ok) {
            failures++;
            std::printf("FAIL boxes %s\n", c["name"].as<const char*>());
        }
    }
    TEST_ASSERT_EQUAL_INT_MESSAGE(0, failures, "boxes vectors failed, see above");
}

static void dots_and_waves_every_vector() {
    int failures = 0;
    for (JsonObjectConst c : doc["dotGap"].as<JsonArrayConst>()) {
        if (!near(dotGap(c["watts"] | 0.0), c["expected"] | -1.0)) {
            failures++;
            std::printf("FAIL dotGap %.0f: %.3f\n", c["watts"] | 0.0, dotGap(c["watts"] | 0.0));
        }
    }
    for (JsonObjectConst c : doc["dots"].as<JsonArrayConst>()) {
        FlowLine line;
        line.x0 = c["line"]["x0"] | 0.0;
        line.y0 = c["line"]["y0"] | 0.0;
        line.x1 = c["line"]["x1"] | 0.0;
        line.y1 = c["line"]["y1"] | 0.0;
        const std::vector<Point> pts = dotsOnLine(line, c["phase"] | 0.0, c["gives"] | false, c["watts"] | 0.0);
        JsonArrayConst e = c["expected"].as<JsonArrayConst>();
        bool ok = pts.size() == e.size();
        for (size_t i = 0; ok && i < pts.size(); i++) ok = near(pts[i].x, e[i][0] | 0.0, 0.006) && near(pts[i].y, e[i][1] | 0.0, 0.006);
        if (!ok) {
            failures++;
            std::printf("FAIL dots phase %.2f: %zu dots\n", c["phase"] | 0.0, pts.size());
        }
    }
    for (JsonObjectConst c : doc["wave"].as<JsonArrayConst>()) {
        const double x = c["x"] | 0.0, phase = c["phase"] | 0.0;
        auto rgb = [](JsonArrayConst a) { return std::array<int, 3>{a[0] | 0, a[1] | 0, a[2] | 0}; };
        const std::array<int, 3> got = waveColour(rgb(c["band"]), rgb(c["ground"]), x, phase);
        if (!near(waveMix(x, phase), c["mix"] | -1.0) || got != rgb(c["colour"])) {
            failures++;
            std::printf("FAIL wave x %.0f phase %.2f: %d %d %d\n", x, phase, got[0], got[1], got[2]);
        }
    }
    TEST_ASSERT_EQUAL_INT_MESSAGE(0, failures, "dot and wave vectors failed, see above");
}

static void shapes_every_vector() {
    JsonArrayConst cases = doc["shapes"].as<JsonArrayConst>();
    TEST_ASSERT_GREATER_OR_EQUAL_MESSAGE(8, cases.size(), "fewer shapes than the designer has");
    int failures = 0;
    for (JsonObjectConst c : cases) {
        JsonArrayConst a = c["args"].as<JsonArrayConst>();
        auto n = [&](int i) { return a[i].as<double>(); };
        const std::string kind = c["kind"].as<std::string>();
        std::vector<Point8> pts = kind == "disc"      ? discOutline(n(0), n(1), n(2))
                                  : kind == "ring"    ? ringOutline(n(0), n(1), n(2), n(3))
                                  : kind == "capsule" ? capsuleOutline(n(0), n(1), n(2), n(3), n(4))
                                  : kind == "arc"     ? arcOutline(n(0), n(1), n(2), n(3), a[4].as<int>())
                                  : kind == "bar"     ? barOutline(n(0), n(1), n(2), n(3), n(4), a[5].as<bool>())
                                                      : rectOutline(n(0), n(1), n(2), n(3));
        std::vector<PolygonRaster::Point> raster;
        long long sumX = 0, sumY = 0;
        for (const Point8& p : pts) {
            raster.push_back({p.x, p.y});
            sumX += p.x;
            sumY += p.y;
        }
        long long full = 0, partial = 0, coverage = 0, weighted = 0;
        PolygonRaster::Sink sink;
        sink.fullRun = [&](int16_t y, int16_t x0, int16_t x1) {
            for (int x = x0; x <= x1; x++) {
                full++;
                coverage += 16;
                weighted += 16LL * (x * 7 + y * 13);
            }
        };
        sink.partial = [&](int16_t y, int16_t x, uint8_t cov) {
            partial++;
            coverage += cov;
            weighted += (long long)cov * (x * 7 + y * 13);
        };
        PolygonRaster::fill(raster, 0, 2000, sink);
        JsonObjectConst e = c["expected"];
        if ((long long)pts.size() != (e["points"] | -1LL) || sumX != (e["sumX"] | -1LL) || sumY != (e["sumY"] | -1LL) || full != (e["full"] | -1LL) ||
            partial != (e["partial"] | -1LL) || coverage != (e["coverage"] | -1LL) || weighted != (e["weighted"] | -1LL)) {
            failures++;
            std::printf("FAIL shape %s: %zu points, %lld %lld, full %lld partial %lld coverage %lld weighted %lld\n", c["name"].as<const char*>(), pts.size(), sumX,
                        sumY, full, partial, coverage, weighted);
        }
    }
    TEST_ASSERT_EQUAL_INT_MESSAGE(0, failures, "shape vectors failed, see above");
}

static void rgb565_and_mixing() {
    for (JsonObjectConst c : doc["rgb565"].as<JsonArrayConst>()) {
        const std::string hex = c["hex"].as<std::string>();
        const long v = std::strtol(hex.c_str() + 1, nullptr, 16);
        TEST_ASSERT_EQUAL_UINT16(c["expected"].as<uint16_t>(), rgb565((v >> 16) & 255, (v >> 8) & 255, v & 255));
        const std::array<int, 3> e = expand565(c["expected"].as<uint16_t>());
        TEST_ASSERT_EQUAL_INT(c["expanded"][0].as<int>(), e[0]);
        TEST_ASSERT_EQUAL_INT(c["expanded"][1].as<int>(), e[1]);
        TEST_ASSERT_EQUAL_INT(c["expanded"][2].as<int>(), e[2]);
    }
    for (JsonObjectConst c : doc["mix565"].as<JsonArrayConst>())
        TEST_ASSERT_EQUAL_UINT16(c["expected"].as<uint16_t>(), mix565(c["under"].as<uint16_t>(), c["over"].as<uint16_t>(), c["w"].as<int>()));
}

static void storage_dots_every_vector() {
    int failures = 0;
    const std::vector<Node> nodes = nodesOf();
    for (JsonObjectConst c : doc["storageDots"].as<JsonArrayConst>()) {
        const std::vector<Resolved> r = resolve(nodes, valuesOf(nodes, c["values"]));
        const FlowLayout l = flowLayout(nodes, r);
        const FlowLine* line = nullptr;
        const FlowNode* node = nullptr;
        const Resolved* rb = nullptr;
        for (const FlowLine& x : l.lines)
            if (x.id == "battery") line = &x;
        for (const FlowNode& x : l.nodes)
            if (x.id == "battery") node = &x;
        for (const Resolved& x : r)
            if (x.id == "battery") rb = &x;
        const std::vector<PathDot> dots = storageDots(*line, *node, c["phase"] | 0.0, rb->gives, rb->watts);
        JsonArrayConst e = c["expected"].as<JsonArrayConst>();
        bool ok = dots.size() == e.size();
        for (size_t i = 0; ok && i < dots.size(); i++) {
            ok = near(dots[i].x, e[i][0] | 0.0) && near(dots[i].y, e[i][1] | 0.0) && near(dots[i].alpha, e[i][2] | 0.0) &&
                 (dots[i].onBar ? 1 : 0) == (e[i][3] | -1) && (dots[i].onLine ? 1 : 0) == (e[i][4] | -1);
        }
        if (!ok) {
            failures++;
            std::printf("FAIL storage dots %s: %zu dots\n", c["name"].as<const char*>(), dots.size());
        }
    }
    for (JsonObjectConst c : doc["trackColour"].as<JsonArrayConst>()) {
        auto rgb = [](JsonArrayConst a) { return std::array<int, 3>{a[0] | 0, a[1] | 0, a[2] | 0}; };
        if (trackColour(rgb(c["bar"]), rgb(c["ground"])) != rgb(c["expected"])) failures++;
    }
    TEST_ASSERT_EQUAL_INT_MESSAGE(0, failures, "storage dot vectors failed, see above");
}

static void watts_in_a_line() {
    TEST_ASSERT_EQUAL_STRING("92 W", withWatts("{watts} W", true, 92.4).c_str());
    TEST_ASSERT_EQUAL_STRING("93 W", withWatts("{watts} W", true, 92.5).c_str());
    TEST_ASSERT_EQUAL_STRING("-40 W", withWatts("{watts} W", true, -40).c_str());
    TEST_ASSERT_EQUAL_STRING("- W", withWatts("{watts} W", false, 0).c_str());
    TEST_ASSERT_EQUAL_STRING("0 W · 0", withWatts("{watts} W · {watts}", true, 0).c_str());
    TEST_ASSERT_EQUAL_STRING("{live:soc} %", withWatts("{live:soc} %", true, 5).c_str());
}

static void a_tap() {
    const std::vector<Node> nodes = nodesOf();
    Value solar;
    solar.hasWatts = true;
    solar.watts = 120;
    std::vector<Value> values(nodes.size());
    values[1] = solar;
    const std::vector<Resolved> r = resolve(nodes, values);
    const Size size{676, 396};
    TEST_ASSERT_EQUAL_STRING("solar", nodeAtTap(false, nodes, r, size, 1000, 114 + 31, 52).c_str());
    TEST_ASSERT_EQUAL_STRING("", nodeAtTap(false, nodes, r, size, 1000, 114 + 33, 52).c_str());
    TEST_ASSERT_EQUAL_STRING("", nodeAtTap(false, nodes, r, size, 1000, 338, 186).c_str());
    const SankeyLayout l = sankeyLayout(r, size, 1000);
    for (const Slot& s : l.slots) {
        const double x = s.side == Side::Left ? 10 : size.width - 10;
        TEST_ASSERT_EQUAL_STRING(s.id.c_str(), nodeAtTap(true, nodes, r, size, 1000, x, s.y + s.height / 2).c_str());
        TEST_ASSERT_EQUAL_STRING("", nodeAtTap(true, nodes, r, size, 1000, size.width / 2, s.y + s.height / 2).c_str());
    }
}

int main(int, char**) {
    std::string json;
    if (!readFile(COPY, json)) {
        std::printf("missing %s\n", COPY);
        return 1;
    }
    deserializeJson(doc, json);
    UNITY_BEGIN();
    RUN_TEST(the_copy_is_the_designers);
    RUN_TEST(resolve_every_vector);
    RUN_TEST(sankey_every_vector);
    RUN_TEST(flow_every_vector);
    RUN_TEST(boxes_every_vector);
    RUN_TEST(dots_and_waves_every_vector);
    RUN_TEST(shapes_every_vector);
    RUN_TEST(rgb565_and_mixing);
    RUN_TEST(storage_dots_every_vector);
    RUN_TEST(watts_in_a_line);
    RUN_TEST(a_tap);
    return UNITY_END();
}
