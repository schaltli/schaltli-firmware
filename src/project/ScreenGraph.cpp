#include "project/ScreenGraph.h"

#include <algorithm>
#include <cmath>

namespace screengraph {

bool edgeFrom(const std::string& name, Edge& out) {
    if (name == "up") out = Edge::Up;
    else if (name == "down") out = Edge::Down;
    else if (name == "left") out = Edge::Left;
    else if (name == "right") out = Edge::Right;
    else return false;
    return true;
}

const char* edgeName(Edge edge) {
    switch (edge) {
        case Edge::Up: return "up";
        case Edge::Down: return "down";
        case Edge::Left: return "left";
        case Edge::Right: return "right";
    }
    return "up";
}

Edge opposite(Edge edge) {
    switch (edge) {
        case Edge::Up: return Edge::Down;
        case Edge::Down: return Edge::Up;
        case Edge::Left: return Edge::Right;
        case Edge::Right: return Edge::Left;
    }
    return Edge::Up;
}

const char* swipeFor(Edge side) {
    switch (side) {
        case Edge::Down: return "swipe-up";
        case Edge::Up: return "swipe-down";
        case Edge::Right: return "swipe-left";
        case Edge::Left: return "swipe-right";
    }
    return "";
}

bool sideOfSwipe(const std::string& swipeId, Edge& out) {
    if (swipeId == "swipe-up") out = Edge::Down;
    else if (swipeId == "swipe-down") out = Edge::Up;
    else if (swipeId == "swipe-left") out = Edge::Right;
    else if (swipeId == "swipe-right") out = Edge::Left;
    else return false;
    return true;
}

static bool inRow(const std::vector<std::string>& row, const std::string& id) {
    return std::find(row.begin(), row.end(), id) != row.end();
}

bool swipeLeadsTo(const GraphScreen& screen, Edge side, const std::vector<std::string>& mainRow) {
    const std::string& neighbour = screen.neighbour(side);
    if (neighbour.empty()) return false;
    auto it = screen.swipes.find(swipeFor(side));
    if (it == screen.swipes.end()) return false;
    const Action& action = it->second;
    if (action.type == "goto-neighbour") return action.side == edgeName(side);
    if (action.type == "goto-screen") return action.targetScreenId == neighbour;
    if (!inRow(mainRow, screen.id) || !inRow(mainRow, neighbour)) return false;
    return (side == Edge::Right && action.type == "next-screen") || (side == Edge::Left && action.type == "previous-screen");
}

std::vector<Edge> tabSides(const GraphScreen& screen, const std::vector<std::string>& mainRow) {
    std::vector<Edge> sides;
    for (Edge e : {Edge::Up, Edge::Down, Edge::Left, Edge::Right}) {
        if (swipeLeadsTo(screen, e, mainRow)) sides.push_back(e);
    }
    return sides;
}

std::string rowStep(const std::vector<std::string>& mainRow, const std::string& currentId, const std::string& pageId, int delta) {
    const int n = static_cast<int>(mainRow.size());
    if (n == 0) return "";
    const std::string& from = inRow(mainRow, currentId) ? currentId : pageId;
    int index = 0;
    for (int i = 0; i < n; i++) {
        if (mainRow[i] == from) {
            index = i;
            break;
        }
    }
    return mainRow[(((index + delta) % n) + n) % n];
}

Rect contentArea(int width, int height, const std::string& edge, int thickness) {
    if (edge == "left") return {thickness, 0, width - thickness, height};
    if (edge == "right") return {0, 0, width - thickness, height};
    if (edge == "top") return {0, thickness, width, height - thickness};
    if (edge == "bottom") return {0, 0, width, height - thickness};
    return {0, 0, width, height};
}

// Rounded down, as JavaScript's Math.floor - also for a negative numerator.
static int floorDiv(int a, int b) {
    int q = a / b;
    return (a % b != 0 && ((a < 0) != (b < 0))) ? q - 1 : q;
}

Rect tabRect(Edge edge, const Rect& area, int nudge) {
    const int cx = area.x + floorDiv(area.width - TAB_LENGTH, 2);
    const int cy = area.y + floorDiv(area.height - TAB_LENGTH, 2);
    const int near = TAB_INSET + nudge;
    switch (edge) {
        case Edge::Up: return {cx, area.y + near, TAB_LENGTH, TAB_THICKNESS};
        case Edge::Down: return {cx, area.y + area.height - near - TAB_THICKNESS, TAB_LENGTH, TAB_THICKNESS};
        case Edge::Left: return {area.x + near, cy, TAB_THICKNESS, TAB_LENGTH};
        case Edge::Right: return {area.x + area.width - near - TAB_THICKNESS, cy, TAB_THICKNESS, TAB_LENGTH};
    }
    return {};
}

int nudgeAt(long ms) {
    const long t = ((ms % TAB_PERIOD_MS) + TAB_PERIOD_MS) % TAB_PERIOD_MS;
    if (t >= TAB_NUDGE_MS) return 0;
    // Math.round: halves away from zero for the positive values here.
    return static_cast<int>(std::floor(TAB_NUDGE * std::sin(M_PI * static_cast<double>(t) / TAB_NUDGE_MS) + 0.5));
}

}  // namespace screengraph
