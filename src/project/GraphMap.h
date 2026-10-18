#pragma once

// The knob's map of the screen graph (schaltli-designer
// docs/2026-10-17-screen-graph.md, decision 15): where each screen's tile
// lies, which tile is under a point, and how far the map pans when the
// finger nears the round edge. Laid out as the designer's «Manage Screens»
// lays it out - the main row across, a screen above or below a page above or
// below it - around the current screen at the display's centre.
//
// Pure, no Arduino: `pio test -e native` runs test/test_graph_map.

#include <string>
#include <vector>

namespace graphmap {

// A tile's place in the comb: its column (the main row's index) and level
// (-1 above the row, 0 in it, 1 below).
struct Tile {
    std::string id;
    int col = 0;
    int level = 0;
};

// One screen of the main row and what lies above and below it ("" for none).
struct Column {
    std::string page;
    std::string up;
    std::string down;
};

// The tiles of a comb, row first then what lies above and below each page.
std::vector<Tile> layout(const std::vector<Column>& columns);

// The tile with this id, or -1.
int indexOf(const std::vector<Tile>& tiles, const std::string& id);

struct Point {
    int x = 0;
    int y = 0;
};

// The geometry: a tile's size and the steps between tiles' centres, on a
// display of `size` × `size` with its centre in the middle.
constexpr int TILE_W = 84;
constexpr int TILE_H = 92;
constexpr int COL_STEP = 104;
constexpr int ROW_STEP = 112;

// Where a tile's centre lies with the tile `current` at the centre of a
// display `size` wide, the map panned by `pan`.
Point centreOf(const Tile& tile, const Tile& current, int size, Point pan);

// The tile under a point, or -1.
int tileAt(const std::vector<Tile>& tiles, int current, int size, Point pan, Point at);

// Near the round edge the map pans, towards the side the finger is on so
// what lies beyond comes in: within `EDGE_BAND` of the edge, at
// `PAN_PER_SECOND` units a second, for `ms` milliseconds. The new pan, kept
// so that no tile lies further than the display's half from the centre.
constexpr int EDGE_BAND = 48;
constexpr int PAN_PER_SECOND = 240;
Point panned(const std::vector<Tile>& tiles, int current, int size, Point pan, Point at, long ms);

}  // namespace graphmap
