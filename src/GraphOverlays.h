#pragma once

// The knob's way through the screen graph, having no navigator (schaltli-
// designer docs/2026-10-17-screen-graph.md, decisions 6 and 15; tasks/
// graph-knob-map-plan.md):
//
//   - GraphHints: while a swipe is being made, at each edge it leads to a
//     neighbour from, that screen's icon and name - 12, 3, 6 and 9 o'clock -
//     fading as the screen follows the finger.
//   - GraphMapView: on a swipe that ends in a pause, the graph as a map, laid
//     out as the designer's «Manage Screens» (src/project/GraphMap); the
//     finger the pointer, lifting it jumps to the tile under it.
//
// Both are LVGL objects on the top layer, above the screen's image: they
// stand still while it slides, and taking them away leaves the picture as it
// was. Icons are the screens' page icons (Screen::pageIconPath, a grayscale
// mask), names LVGL's Montserrat 14.

#include <Arduino.h>
#include <lvgl.h>

#include <vector>

#include "ClippedCanvas16.h"
#include "project/GraphMap.h"
#include "project/ProjectTypes.h"

// A page icon as an LVGL image: the mask drawn in `fg` on `bg`.
struct OverlayIcon {
  ClippedCanvas16* canvas = nullptr;
  lv_img_dsc_t dsc = {};
  String path;
  uint16_t fg = 0;
  bool make(const String& path, uint16_t fg, uint16_t bg);
  // Drawn again on another ground - a tile lit or no longer.
  void repaint(uint16_t bg);
  void release();
};

class GraphHints {
public:
  // The hints for this screen, shown: one per edge a swipe leads to a
  // neighbour from (the pull tab's rule). Nothing where there is none.
  void show(const ProjectConfig& project, int screenIndex);
  // How far the finger has gone: the hints fade over the first part of it.
  void setTravel(int px);
  void hide();
  bool shown() const { return !hints_.empty(); }
  // The edges with a hint, "up down left right" in that order, for the
  // test interface.
  String sides() const;

private:
  struct Hint {
    lv_obj_t* box = nullptr;
    OverlayIcon icon;
    String side;
  };
  std::vector<Hint> hints_;
  int opacity_ = -1;
};

class GraphMapView {
public:
  // The map around this screen; false (nothing shown) when it is not in the
  // graph or the graph has a single screen.
  bool open(const ProjectConfig& project, int screenIndex, int size);
  // The finger at (x, y): the tile under it lit, the map panned near the edge.
  void pointer(int x, int y, unsigned long nowMs);
  // Closes it: the screen index of the lit tile when it is not the current
  // one, else -1 - lifting over the current screen or off every tile cancels.
  int close();
  bool isOpen() const { return backdrop_ != nullptr; }
  // What the test interface reports: the lit tile and every tile's centre.
  String debugJson() const;

private:
  void place();
  // A tile's ground, and its icon's with it.
  void paintTile(int i, bool lit);

  int size_ = 360;
  lv_obj_t* backdrop_ = nullptr;
  std::vector<graphmap::Tile> tiles_;
  std::vector<int> screenOf_;
  std::vector<lv_obj_t*> tileObjs_;
  std::vector<OverlayIcon> icons_;
  // Joins between docked screens and between neighbours in the row: the
  // pair of tile indices each one joins, and its object.
  std::vector<std::pair<int, int>> joins_;
  std::vector<lv_obj_t*> joinObjs_;
  int current_ = -1;
  int lit_ = -1;
  graphmap::Point pan_;
  unsigned long lastMs_ = 0;
};
