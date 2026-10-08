#pragma once

// The navigator's geometry (schaltli-designer docs/2026-10-08-navigator.md,
// device contract 2.7): which screens it lists, its strip, where each entry
// is at a scroll offset, how far to scroll to show an entry, which entry is
// under a point, a page of scrolling for e-ink.
//
// A line-by-line port of the designer's lib/navigator.ts; the Android app
// has a third. All three are held to the same cases,
// test/test_navigator/navigator-vectors.json - a byte-for-byte copy of the
// designer's lib/navigator/vectors.json. Whole pixels, no Arduino: `pio test
// -e native` runs the vectors on the PC.

#include <string>
#include <vector>

namespace navigatorlayout {

enum class Edge { Top, Bottom, Left, Right };

struct Rect {
    int x = 0;
    int y = 0;
    int width = 0;
    int height = 0;
};

struct Layout {
    Edge edge = Edge::Left;
    Rect strip;
    bool horizontal = false;  // entries run along x (top, bottom)
    int length = 0;           // the strip's length along the entries
    int count = 0;
    int entryLength = 0;
};

struct ListedScreen {
    bool isMaster = false;
    bool popup = false;
    bool hidden = false;
};

Edge edgeFrom(const std::string& name);

// The indices of the screens with an entry: main screens that are not hidden.
std::vector<int> listedScreens(const std::vector<ListedScreen>& screens);

// The thickness and the shortest entry, as the designer's NAVIGATOR_SIZES.
int thicknessFor(bool withText);
int minEntryFor(bool withText);

Rect strip(Edge edge, bool withText, int screenWidth, int screenHeight);
Layout layout(Edge edge, bool withText, int screenWidth, int screenHeight, int count);
// The same for a strip already placed, its thickness given by the export.
Layout layoutInStrip(Edge edge, const Rect& strip, int count, int entryLength);

int maxScroll(const Layout& layout);
int clampScroll(const Layout& layout, int scroll);
Rect entryRect(const Layout& layout, int index, int scroll);
int scrollToShow(const Layout& layout, int index, int scroll);
int entryAt(const Layout& layout, int x, int y, int scroll);
int pageScroll(const Layout& layout, int scroll, int direction);

// The strip of an exported navigator: `thickness` deep along `edge`.
Rect stripOf(Edge edge, int thickness, int screenWidth, int screenHeight);

// Paging past hidden screens (contract 2.7): from `current`, `delta` steps
// (+1 next, -1 previous) over the screens that are not hidden, wrapping.
// `current` itself may be hidden (reached by a goto). With none shown,
// `current`.
int pagedIndex(const std::vector<bool>& hidden, int current, int delta);
// The screen a device starts on: the first that is not hidden, else 0.
int firstShown(const std::vector<bool>& hidden);

}  // namespace navigatorlayout
