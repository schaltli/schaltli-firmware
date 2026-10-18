#include "project/GraphMap.h"

#include <algorithm>
#include <cmath>

namespace graphmap {

std::vector<Tile> layout(const std::vector<Column>& columns) {
    std::vector<Tile> tiles;
    for (size_t i = 0; i < columns.size(); i++) tiles.push_back({columns[i].page, (int)i, 0});
    for (size_t i = 0; i < columns.size(); i++) {
        if (!columns[i].up.empty()) tiles.push_back({columns[i].up, (int)i, -1});
        if (!columns[i].down.empty()) tiles.push_back({columns[i].down, (int)i, 1});
    }
    return tiles;
}

int indexOf(const std::vector<Tile>& tiles, const std::string& id) {
    for (size_t i = 0; i < tiles.size(); i++) {
        if (tiles[i].id == id) return (int)i;
    }
    return -1;
}

Point centreOf(const Tile& tile, const Tile& current, int size, Point pan) {
    return {size / 2 + (tile.col - current.col) * COL_STEP + pan.x, size / 2 + (tile.level - current.level) * ROW_STEP + pan.y};
}

int tileAt(const std::vector<Tile>& tiles, int current, int size, Point pan, Point at) {
    if (current < 0 || current >= (int)tiles.size()) return -1;
    for (size_t i = 0; i < tiles.size(); i++) {
        const Point c = centreOf(tiles[i], tiles[current], size, pan);
        if (std::abs(at.x - c.x) <= TILE_W / 2 && std::abs(at.y - c.y) <= TILE_H / 2) return (int)i;
    }
    return -1;
}

Point panned(const std::vector<Tile>& tiles, int current, int size, Point pan, Point at, long ms) {
    if (current < 0 || current >= (int)tiles.size() || ms <= 0) return pan;
    const double cx = size / 2.0, cy = size / 2.0;
    const double dx = at.x - cx, dy = at.y - cy;
    const double r = std::sqrt(dx * dx + dy * dy);
    if (r < size / 2.0 - EDGE_BAND || r == 0) return pan;
    // Towards the finger's side: the map moves the other way, so what lies
    // beyond the edge there comes in.
    const double step = PAN_PER_SECOND * ms / 1000.0;
    Point next{pan.x - (int)std::lround(dx / r * step), pan.y - (int)std::lround(dy / r * step)};
    // No further than brings the farthest tile on each side to the centre.
    int minCol = 0, maxCol = 0, minLevel = 0, maxLevel = 0;
    for (const Tile& t : tiles) {
        minCol = std::min(minCol, t.col - tiles[current].col);
        maxCol = std::max(maxCol, t.col - tiles[current].col);
        minLevel = std::min(minLevel, t.level - tiles[current].level);
        maxLevel = std::max(maxLevel, t.level - tiles[current].level);
    }
    next.x = std::max(-maxCol * COL_STEP, std::min(-minCol * COL_STEP, next.x));
    next.y = std::max(-maxLevel * ROW_STEP, std::min(-minLevel * ROW_STEP, next.y));
    return next;
}

}  // namespace graphmap
