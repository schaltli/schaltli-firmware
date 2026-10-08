#include "project/NavigatorLayout.h"

#include <algorithm>

// The designer's lib/navigator.ts, line by line.

namespace navigatorlayout {

Edge edgeFrom(const std::string& name) {
    if (name == "top") return Edge::Top;
    if (name == "bottom") return Edge::Bottom;
    if (name == "right") return Edge::Right;
    return Edge::Left;
}

std::vector<int> listedScreens(const std::vector<ListedScreen>& screens) {
    std::vector<int> out;
    for (size_t i = 0; i < screens.size(); i++)
        if (!screens[i].isMaster && !screens[i].popup && !screens[i].hidden) out.push_back(static_cast<int>(i));
    return out;
}

int thicknessFor(bool withText) { return withText ? 80 : 64; }
int minEntryFor(bool withText) { return withText ? 88 : 64; }

Rect strip(Edge edge, bool withText, int screenWidth, int screenHeight) {
    const int t = thicknessFor(withText);
    if (edge == Edge::Top) return {0, 0, screenWidth, t};
    if (edge == Edge::Bottom) return {0, screenHeight - t, screenWidth, t};
    if (edge == Edge::Left) return {0, 0, t, screenHeight};
    return {screenWidth - t, 0, t, screenHeight};
}

Layout layout(Edge edge, bool withText, int screenWidth, int screenHeight, int count) {
    const Rect s = strip(edge, withText, screenWidth, screenHeight);
    const bool horizontal = edge == Edge::Top || edge == Edge::Bottom;
    const int length = horizontal ? s.width : s.height;
    const int min = minEntryFor(withText);
    const int entryLength = count > 0 && count * min <= length ? length / count : min;
    return layoutInStrip(edge, s, count, entryLength);
}

Layout layoutInStrip(Edge edge, const Rect& s, int count, int entryLength) {
    Layout l;
    l.edge = edge;
    l.strip = s;
    l.horizontal = edge == Edge::Top || edge == Edge::Bottom;
    l.length = l.horizontal ? s.width : s.height;
    l.count = count;
    l.entryLength = entryLength;
    return l;
}

int maxScroll(const Layout& l) { return std::max(0, l.count * l.entryLength - l.length); }

int clampScroll(const Layout& l, int scroll) { return std::min(std::max(0, scroll), maxScroll(l)); }

Rect entryRect(const Layout& l, int index, int scroll) {
    const int along = index * l.entryLength - scroll;
    if (l.horizontal) return {l.strip.x + along, l.strip.y, l.entryLength, l.strip.height};
    return {l.strip.x, l.strip.y + along, l.strip.width, l.entryLength};
}

int scrollToShow(const Layout& l, int index, int scroll) {
    const int start = index * l.entryLength;
    const int end = start + l.entryLength;
    int next = scroll;
    if (start < next) next = start;
    else if (end > next + l.length) next = end - l.length;
    return clampScroll(l, next);
}

int entryAt(const Layout& l, int x, int y, int scroll) {
    const Rect& s = l.strip;
    if (x < s.x || y < s.y || x >= s.x + s.width || y >= s.y + s.height) return -1;
    if (l.entryLength <= 0) return -1;
    const int along = (l.horizontal ? x - s.x : y - s.y) + scroll;
    const int index = along / l.entryLength;
    return index < l.count ? index : -1;
}

int pageScroll(const Layout& l, int scroll, int direction) {
    const int page = std::max(1, l.entryLength > 0 ? l.length / l.entryLength : 1) * l.entryLength;
    return clampScroll(l, scroll + direction * page);
}

}  // namespace navigatorlayout
