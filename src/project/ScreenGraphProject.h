#pragma once

// The screen graph of a loaded project (designer device contract 2.10), by
// screen index: what every board asks of it - next and previous over the
// main row, the neighbour on an edge, where the pull tabs stand, what a
// swipe moves, the start screen. The decisions are ScreenGraph's, held to the
// designer's vectors; this only reads them off ProjectConfig.
//
// An export from before the screen graph has no `mainRow` and no
// neighbours: its main row is the screens that are not hidden, as it paged
// before, and each of them has the row's screens beside it as neighbours -
// what the designer would export for it today - so a board that got the new
// firmware but still holds an old project swipes as it did. It has no tab:
// the export carries no colour for one.

#include <string>
#include <vector>

#include "project/ProjectTypes.h"
#include "project/ScreenGraph.h"

namespace screengraphproject {

inline int screenIndexById(const ProjectConfig& project, const String& id) {
  if (id.isEmpty()) return -1;
  for (size_t i = 0; i < project.screens.size(); i++) {
    if (project.screens[i].id == id) return (int)i;
  }
  return -1;
}

inline std::vector<std::string> mainRowIds(const ProjectConfig& project) {
  std::vector<std::string> row;
  if (!project.mainRow.empty()) {
    for (const String& id : project.mainRow) row.push_back(id.c_str());
    return row;
  }
  for (const Screen& s : project.screens) {
    if (!s.hidden) row.push_back(s.id.c_str());
  }
  return row;
}

// The id of the screen on an edge: as exported, or for an export from before
// the screen graph the row's screen beside it.
inline std::string neighbourId(const ProjectConfig& project, int index, screengraph::Edge side) {
  if (index < 0 || index >= (int)project.screens.size()) return "";
  if (!project.mainRow.empty()) return project.screens[index].neighbours[(int)side].c_str();
  if (side != screengraph::Edge::Left && side != screengraph::Edge::Right) return "";
  const std::vector<std::string> row = mainRowIds(project);
  const std::string id = project.screens[index].id.c_str();
  for (size_t i = 0; i < row.size(); i++) {
    if (row[i] != id) continue;
    if (side == screengraph::Edge::Left) return i > 0 ? row[i - 1] : "";
    return i + 1 < row.size() ? row[i + 1] : "";
  }
  return "";
}

inline screengraph::GraphScreen graphScreenOf(const ProjectConfig& project, int index) {
  screengraph::GraphScreen g;
  if (index < 0 || index >= (int)project.screens.size()) return g;
  const Screen& s = project.screens[index];
  g.id = s.id.c_str();
  for (int e = 0; e < screengraph::EDGE_COUNT; e++) g.neighbours[e] = neighbourId(project, index, (screengraph::Edge)e);
  g.hasBack = screengraph::edgeFrom(s.back.c_str(), g.back);
  for (const ScreenButtonAction& b : s.buttonActions) {
    screengraph::Edge side;
    if (!screengraph::sideOfSwipe(b.buttonId.c_str(), side)) continue;
    g.swipes[b.buttonId.c_str()] = {b.action.type.c_str(), b.action.side.c_str(), b.action.targetScreenId.c_str()};
  }
  return g;
}

// The screen on an edge of screen `index`, or -1.
inline int neighbourIndex(const ProjectConfig& project, int index, screengraph::Edge side) {
  return screenIndexById(project, String(neighbourId(project, index, side).c_str()));
}

// Next (delta 1) or previous (-1) over the main row from screen `index`; a
// screen off it goes from the page it is docked on. `index` itself when
// there is no row (a popup's index too: callers step from the screen).
inline int rowStepIndex(const ProjectConfig& project, int index, int delta) {
  if (index < 0 || index >= (int)project.screens.size()) return index;
  const Screen& s = project.screens[index];
  screengraph::Edge back;
  std::string page;
  if (screengraph::edgeFrom(s.back.c_str(), back)) page = s.neighbours[(int)back].c_str();
  const std::string next = screengraph::rowStep(mainRowIds(project), s.id.c_str(), page, delta);
  const int found = screenIndexById(project, String(next.c_str()));
  return found >= 0 ? found : index;
}

// The first screen of the main row, else 0.
inline int startIndex(const ProjectConfig& project) {
  const std::vector<std::string> row = mainRowIds(project);
  const int found = row.empty() ? -1 : screenIndexById(project, String(row.front().c_str()));
  return found >= 0 ? found : 0;
}

// Whether the swipe `swipeId` on screen `index` leads to the neighbour it
// pulls in - where the pull tab stands and a slide follows the finger.
inline bool swipeLeadsToNeighbour(const ProjectConfig& project, int index, const String& swipeId) {
  screengraph::Edge side;
  if (index < 0 || index >= (int)project.screens.size() || !screengraph::sideOfSwipe(swipeId.c_str(), side)) return false;
  return screengraph::swipeLeadsTo(graphScreenOf(project, index), side, mainRowIds(project));
}

inline std::vector<screengraph::Edge> tabSidesAt(const ProjectConfig& project, int index) {
  if (index < 0 || index >= (int)project.screens.size()) return {};
  return screengraph::tabSides(graphScreenOf(project, index), mainRowIds(project));
}

// What a swipe on screen `index` moves: the screen less its navigator's strip.
inline screengraph::Rect contentAreaAt(const ProjectConfig& project, int index, int width, int height) {
  if (index >= 0 && index < (int)project.screens.size()) {
    const Screen& s = project.screens[index];
    for (const Navigator& nav : project.navigators) {
      if (!s.navigatorId.isEmpty() && nav.id == s.navigatorId) return screengraph::contentArea(width, height, nav.edge.c_str(), nav.thickness);
    }
  }
  return screengraph::contentArea(width, height, "", 0);
}

}  // namespace screengraphproject
