#pragma once

// The screen graph as the export carries it (schaltli-designer device
// contract §2.10, docs/2026-10-17-screen-graph.md): next and previous over
// `mainRow`, which edges have a pull tab, the content area beside a
// navigator's strip, the tab's box, and the nudge. A device computes nothing
// else: the swipes' actions come decided.
//
// A port of the designer's lib/screen-graph-device.ts and lib/pull-tabs.ts;
// the Android app has a third. All are held to the same cases,
// test/test_screen_graph/screen-graph-vectors.json - a byte-for-byte copy of
// the designer's lib/screen-graph/vectors.json. Whole pixels, no Arduino:
// `pio test -e native` runs the vectors on the PC.

#include <map>
#include <string>
#include <vector>

namespace screengraph {

enum class Edge { Up, Down, Left, Right };

constexpr int EDGE_COUNT = 4;

// "up", "down", "left", "right"; false for anything else.
bool edgeFrom(const std::string& name, Edge& out);
const char* edgeName(Edge edge);
// The edge across: up for down.
Edge opposite(Edge edge);

// The swipe that pulls in the neighbour on an edge: swiping up shows the
// screen below, so Down is "swipe-up".
const char* swipeFor(Edge side);
// The edge a swipe pulls its neighbour in from; false for no swipe id.
bool sideOfSwipe(const std::string& swipeId, Edge& out);

// What a swipe does, as far as the graph cares.
struct Action {
    std::string type;
    std::string side;            // goto-neighbour
    std::string targetScreenId;  // goto-screen
};

struct GraphScreen {
    std::string id;
    // By Edge; empty where nothing lies.
    std::string neighbours[EDGE_COUNT];
    bool hasBack = false;
    Edge back = Edge::Up;
    // By swipe id ("swipe-up", …).
    std::map<std::string, Action> swipes;

    const std::string& neighbour(Edge e) const { return neighbours[static_cast<int>(e)]; }
};

// Whether the swipe towards `side` leads to the neighbour there: its action
// is goto-neighbour that way, goto-screen naming it, or - both screens in
// the main row - next-screen towards the right, previous-screen towards the
// left. The pull tab stands, and the screen follows the finger, exactly
// where this holds.
bool swipeLeadsTo(const GraphScreen& screen, Edge side, const std::vector<std::string>& mainRow);

// The edges with a pull tab, in the order up, down, left, right.
std::vector<Edge> tabSides(const GraphScreen& screen, const std::vector<std::string>& mainRow);

// Next (delta 1) or previous (-1) over the main row, wrapping: from the
// screen itself when it is in the row, else from `pageId`, the screen it is
// docked on. Empty for an empty row.
std::string rowStep(const std::vector<std::string>& mainRow, const std::string& currentId, const std::string& pageId, int delta);

struct Rect {
    int x = 0;
    int y = 0;
    int width = 0;
    int height = 0;
};

// The tab's numbers (lib/pull-tabs.ts PULL_TAB).
constexpr int TAB_LENGTH = 32;
constexpr int TAB_THICKNESS = 4;
constexpr int TAB_RADIUS = 2;
constexpr int TAB_INSET = 6;
constexpr int TAB_NUDGE = 6;
constexpr long TAB_PERIOD_MS = 5000;
constexpr long TAB_NUDGE_MS = 800;

// What a swipe moves: the screen less a navigator's strip along `edge`
// ("left", "right", "top", "bottom"), `thickness` deep; the whole screen
// for any other edge.
Rect contentArea(int width, int height, const std::string& edge, int thickness);

// The tab on one edge of `area`, `nudge` pixels towards the middle: whole
// pixels, centred (rounded down).
Rect tabRect(Edge edge, const Rect& area, int nudge);

// How far the tabs stand nudged `ms` into the animation, in whole pixels.
int nudgeAt(long ms);

}  // namespace screengraph
