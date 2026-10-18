#include "project/NavigatorLayout.h"

#include <algorithm>
#include <cmath>
#include <cstdint>

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
    l.bar = s;
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

bool markerGlides(int from, int to, int scrollFrom, int scrollTo) {
    return from >= 0 && to >= 0 && from != to && scrollFrom == scrollTo;
}

int markerOffset(const Layout& l, int from, int to, int travel, int span) {
    if (span <= 0) return 0;
    const int t = std::min(std::abs(travel), span);
    return static_cast<int>(static_cast<int64_t>(to - from) * l.entryLength * t / span);
}

int markerSettle(int start, int end, long elapsedMs) {
    const int64_t t = std::max<int64_t>(0, std::min<int64_t>(1000, static_cast<int64_t>(elapsedMs) * 1000 / kMarkerSettleMs));
    const int64_t u = t - 1000;
    const int64_t f = 1000 + 27 * u * u * u / 10000000 + 17 * u * u / 10000;
    return start + static_cast<int>(static_cast<int64_t>(end - start) * f / 1000);
}

Rect markerRect(const Layout& l, int from, int offset, int scroll, const Rect& box) {
    const Rect entry = entryRect(l, from, scroll - offset);
    return {entry.x + box.x, entry.y + box.y, box.width, box.height};
}

int cornerInset(int radius, int width, int height, int row) {
    const int r = std::max(0, std::min(radius, std::min(width / 2, height / 2)));
    const int d = std::min(row, height - 1 - row);
    if (d < 0 || d >= r) return 0;
    const int k = 2 * r - 2 * d - 1;
    return r - (static_cast<int>(std::sqrt(static_cast<double>(4 * r * r - k * k))) + 1) / 2;
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

Rect stripOf(Edge edge, int thickness, int screenWidth, int screenHeight) {
    if (edge == Edge::Top) return {0, 0, screenWidth, thickness};
    if (edge == Edge::Bottom) return {0, screenHeight - thickness, screenWidth, thickness};
    if (edge == Edge::Left) return {0, 0, thickness, screenHeight};
    return {screenWidth - thickness, 0, thickness, screenHeight};
}

Layout barLayout(Edge edge, const Rect& bar, int count, int entryLength, const std::string& at, int length) {
    if (at != "near" && at != "far") return layoutInStrip(edge, bar, count, entryLength);
    const bool horizontal = edge == Edge::Top || edge == Edge::Bottom;
    const int l = std::min(length, horizontal ? bar.width : bar.height);
    const bool near = at == "near";
    Rect sw, entries;
    if (horizontal) {
        sw = {near ? bar.x : bar.x + bar.width - l, bar.y, l, bar.height};
        entries = {near ? bar.x + l : bar.x, bar.y, bar.width - l, bar.height};
    } else {
        sw = {bar.x, near ? bar.y : bar.y + bar.height - l, bar.width, l};
        entries = {bar.x, near ? bar.y + l : bar.y, bar.width, bar.height - l};
    }
    Layout out = layoutInStrip(edge, entries, count, entryLength);
    out.bar = bar;
    out.hasThemeSwitch = true;
    out.themeSwitch = sw;
    return out;
}

Rect dividerRect(Edge edge, const Rect& strip) {
    const int d = kDivider;
    if (edge == Edge::Left) return {strip.x + strip.width - d, strip.y, d, strip.height};
    if (edge == Edge::Right) return {strip.x, strip.y, d, strip.height};
    if (edge == Edge::Top) return {strip.x, strip.y + strip.height - d, strip.width, d};
    return {strip.x, strip.y, strip.width, d};
}

int pagedIndex(const std::vector<bool>& hidden, int current, int delta) {
    const int count = static_cast<int>(hidden.size());
    if (count == 0) return current;
    int index = current;
    for (int step = 0; step < count; step++) {
        index = ((index + delta) % count + count) % count;
        if (!hidden[index]) return index;
    }
    return current;
}

int firstShown(const std::vector<bool>& hidden) {
    for (size_t i = 0; i < hidden.size(); i++)
        if (!hidden[i]) return static_cast<int>(i);
    return 0;
}

}  // namespace navigatorlayout
