#pragma once

// The energy diagram's core (schaltli-designer docs/2026-10-11-energy-
// diagram.md, device contract 2.8): what each node gives or takes, the
// Sankey's bars, slots and bands, the Flow's circles and lines, where the
// names, lines and icons stand, where a tap lands, the dots and the wave.
//
// A line-by-line port of the designer's lib/energy-diagram.ts; the Android
// app has a third. All three are held to the same cases,
// test/test_energy_diagram/energy-diagram-vectors.json - a byte-for-byte
// copy of the designer's lib/energy-diagram/vectors.json. No Arduino: `pio
// test -e native` runs the vectors on the PC. Places are doubles as in the
// designer; a renderer rounds them where it draws.

#include <array>
#include <string>
#include <vector>

namespace energydiagram {

enum class Role { Source, Sink, Storage, Rest, Bus };

Role roleFrom(const std::string& name);

struct Point {
    double x = 0;
    double y = 0;
};

struct Box {
    double x = 0;
    double y = 0;
    double width = 0;
    double height = 0;
};

struct Size {
    double width = 0;
    double height = 0;
};

// A node as the export gives it: what the core needs of it.
struct Node {
    std::string id;
    Role role = Role::Rest;
    double x = 0;
    double y = 0;
    bool reverseSign = false;
    int lines = 0;  // how many lines under its name (at most MAX_NODE_LINES)
};

// What a node's topics say now.
struct Value {
    bool hasWatts = false;
    double watts = 0;
    bool hasCharge = false;
    double charge = 0;
};

// A node as it is now: never negative watts, and whether it gives (to the bus) or takes.
struct Resolved {
    std::string id;
    Role role = Role::Rest;
    double x = 0;
    double y = 0;
    double watts = 0;
    bool gives = false;
    bool hasCharge = false;
    double charge = 0;
};

constexpr int MAX_NODE_LINES = 3;

// `values[i]` belongs to `nodes[i]`. The bus is not among the result.
std::vector<Resolved> resolve(const std::vector<Node>& nodes, const std::vector<Value>& values);

// `{watts}` put into a line: the watts whole, "-" when unknown.
std::string withWatts(const std::string& text, bool known, double watts);

// JavaScript's Math.round: halves go up, -2.5 to -2.
double jsRound(double v);

// ---- Sankey (spec decision 8)
constexpr double SANKEY_LABEL_W = 170;
constexpr double SANKEY_BAR_W = 12;
constexpr double SANKEY_BUS_W = 16;
constexpr double SANKEY_GAP = 14;
constexpr double SANKEY_MIN_BAND = 3;
constexpr double SANKEY_SLOT_MIN = 66;
constexpr double SANKEY_UNDER_BAR = 4;
constexpr double SANKEY_UNDER_BUS = 2;
constexpr int SANKEY_SEGMENTS = 24;

enum class Side { Left, Right };

struct Bar {
    std::string id;
    Side side = Side::Left;
    Box box;
};
struct Slot {
    std::string id;
    Side side = Side::Left;
    double y = 0;
    double height = 0;
};
struct Band {
    std::string id;
    // SEGMENTS + 1 points along the top, the same back along the bottom.
    std::vector<Point> polygon;
};
struct SankeyLayout {
    std::vector<Bar> bars;
    std::vector<Slot> slots;
    std::vector<Band> bands;
    Box bus;
};

SankeyLayout sankeyLayout(const std::vector<Resolved>& nodes, Size size, double fullScale);

// ---- Flow (spec decision 9)
constexpr double FLOW_NODE_R = 32;
constexpr double FLOW_BUS_R = 14;
constexpr double FLOW_LINE_GAP = 4;
constexpr double FLOW_RING_GAP = 7;
constexpr double FLOW_LABEL_GAP = 10;

double lineWidth(double watts);

enum class Align { Left, Center, Right };

struct FlowNode {
    std::string id;
    double x = 0;
    double y = 0;
    double r = 0;
    bool live = false;
    bool hasRing = false;
    double ringR = 0;
    double ringFraction = 0;
    // Where its name's baseline starts; beside: left-aligned right of the circle.
    double labelX = 0;
    double labelY = 0;
    bool labelBeside = false;
};
struct FlowLine {
    std::string id;
    double x0 = 0, y0 = 0, x1 = 0, y1 = 0;
    double width = 0;
    bool live = false;
};
struct FlowLayout {
    double busX = 0, busY = 0, busR = 0;
    std::vector<FlowNode> nodes;
    std::vector<FlowLine> lines;
};

FlowLayout flowLayout(const std::vector<Node>& nodes, const std::vector<Resolved>& resolved);

// ---- Names, lines and icons (energy-contract C1)
constexpr double ROW_LEADING = 1.2;

struct LabelMetrics {
    double fontSize = 16;
    double valueFontSize = 16;
};
struct LabelBox {
    double x = 0;
    double y = 0;
    double width = 0;
    Align align = Align::Left;
};
struct NodeBoxes {
    std::string id;
    Box icon;
    LabelBox label;
};
struct DiagramBoxes {
    bool hasBus = false;
    LabelBox bus;
    std::vector<NodeBoxes> nodes;
};

double rowStep(double size);
double labelHeight(int lines, const LabelMetrics& m);
DiagramBoxes sankeyBoxes(const SankeyLayout& layout, const std::vector<Node>& nodes, Size size, const LabelMetrics& m);
constexpr double FLOW_LABEL_W = 160;
double flowIcon();
DiagramBoxes flowBoxes(const FlowLayout& layout, const LabelMetrics& m);

// The node a tap at (x, y), relative to the diagram, lands on; "" for none.
std::string nodeAtTap(bool sankey, const std::vector<Node>& nodes, const std::vector<Resolved>& resolved, Size size, double fullScale,
                      double x, double y);

// ---- Motion (spec decision 10)
constexpr double DOT_PACE = 40;
constexpr double DOT_GAP_1W = 120;
constexpr double DOT_GAP_FULL = 8;
constexpr double DOT_R = 3.5;

double dotGap(double watts);
double advanceDots(double phase, double seconds, double watts);
std::vector<Point> dotsOnLine(const FlowLine& line, double phase, bool gives, double watts);

constexpr double WAVE_PX = 48;
constexpr double WAVE_SECONDS = 4;

double advanceWave(double phase, double seconds);
double waveMix(double x, double phase);
std::array<int, 3> waveColour(const std::array<int, 3>& band, const std::array<int, 3>& ground, double x, double phase);

}  // namespace energydiagram
