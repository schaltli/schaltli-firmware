#pragma once
#include <utility>
#include <vector>
#include "ProjectTypes.h"

/**
 * Light or dark, by swapping the loaded project in place.
 *
 * A 24-bit export carries, beside every field X that depends on the theme,
 * an optional XDark (the designer's docs/device-contract.md §2.3): colours
 * in an object's properties, a screen's backgroundColor, the baked bitmaps'
 * paths. While schaltli/state/theme says dark (§4), a device draws XDark
 * where there is one and X where there is not - an empty XDark meaning X,
 * a lone XDark (X absent) meaning the dark value of X.
 *
 * Done here by swapping each pair rather than by teaching the renderer a
 * second name for every field (docs/2026-09-26-device-switch.md in the
 * designer repo). The renderer reads a colour or a path in some twenty
 * places, and derives tracks, surfaces and ink from the screen background;
 * with the pairs swapped every one of them draws dark without a change, and
 * none can be missed. Swapping again restores light.
 *
 * Which pairs were swapped is remembered, in the fixed order they are
 * visited, and only those are swapped back. Deciding again on the way back
 * would get one case wrong: a lone XDark over an empty X leaves the light
 * slot filled and the dark slot empty - exactly what a pair that was never
 * swapped looks like.
 *
 * No hardware in here, only String and std::vector, so tools/theme-variant
 * can check it on a laptop.
 */
namespace themevariant {

// Every (X, XDark) pair of a project, in one fixed order.
template <typename F>
void forEachObjectPair(ScreenObject& obj, F& f) {
  ObjectProperties& p = obj.properties;
  f(p.color, p.colorDark);
  f(p.backgroundColor, p.backgroundColorDark);
  f(p.borderColor, p.borderColorDark);
  f(p.textColor, p.textColorDark);
  f(p.fillColor, p.fillColorDark);
  f(p.fillEndColor, p.fillEndColorDark);
  f(p.trackColor, p.trackColorDark);
  f(p.trackEdgeColor, p.trackEdgeColorDark);
  f(p.strokeColor, p.strokeColorDark);
  f(p.switchColor, p.switchColorDark);
  f(p.fieldColor, p.fieldColorDark);
  f(p.lineColor, p.lineColorDark);
  f(obj.path, obj.pathDark);
  // A button's faces and its icon (contract «button»): colours only, the
  // radius and the layout are one for both.
  if (p.button) {
    ButtonLookProps& b = *p.button;
    f(b.normal.fill, b.normalDark.fill);
    f(b.normal.outline, b.normalDark.outline);
    f(b.normal.ink, b.normalDark.ink);
    f(b.pressed.fill, b.pressedDark.fill);
    f(b.pressed.outline, b.pressedDark.outline);
    f(b.pressed.ink, b.pressedDark.ink);
    f(b.iconPath, b.iconPathDark);
    f(b.iconPathPressed, b.iconPathPressedDark);
  }
  for (SwitchState& state : p.states) {
    f(state.iconPath, state.iconPathDark);
    f(state.iconPathActive, state.iconPathActiveDark);
    f(state.iconPathPressed, state.iconPathPressedDark);
  }
  // A Toggle Button's looks and its ring (contract «toggle-button»): colours
  // only, the layout is one for both.
  if (p.toggle) {
    ToggleButtonProps& t = *p.toggle;
    auto look = [&f](ToggleLook& l) {
      f(l.normal.fill, l.normalDark.fill);
      f(l.normal.outline, l.normalDark.outline);
      f(l.normal.ink, l.normalDark.ink);
      f(l.pressed.fill, l.pressedDark.fill);
      f(l.pressed.outline, l.pressedDark.outline);
      f(l.pressed.ink, l.pressedDark.ink);
      f(l.discNormal, l.discNormalDark);
      f(l.discPressed, l.discPressedDark);
      f(l.discEdgeNormal, l.discEdgeNormalDark);
      f(l.discEdgePressed, l.discEdgePressedDark);
    };
    for (ToggleLook& l : t.looks) look(l);
    look(t.none);
    f(t.ring, t.ringDark);
  }
  for (ValueIconPair& pair : p.valueIconPairs) f(pair.path, pair.pathDark);
  // The energy diagram's colours, its nodes' and their icons' looks (2.8).
  if (p.energy) {
    EnergyDiagramProps& e = *p.energy;
    f(e.textColor, e.textColorDark);
    f(e.mutedColor, e.mutedColorDark);
    f(e.busColor, e.busColorDark);
    f(e.idleColor, e.idleColorDark);
    f(e.chargeColor, e.chargeColorDark);
    f(e.dischargeColor, e.dischargeColorDark);
    for (EnergyNodeProps& n : e.nodes) {
      f(n.color, n.colorDark);
      f(n.icon.live, n.icon.liveDark);
      f(n.icon.charge, n.icon.chargeDark);
      f(n.icon.discharge, n.icon.dischargeDark);
      f(n.icon.muted, n.icon.mutedDark);
    }
  }
  // A chart's quiet colour, and each series' and zone's (2.13).
  if (p.chart) {
    ChartProps& c = *p.chart;
    f(c.mutedColor, c.mutedColorDark);
    f(c.nowColor, c.nowColorDark);
    for (size_t i = 0; i < c.seriesColor.size(); i++) f(c.seriesColor[i], c.seriesColorDark[i]);
    for (size_t i = 0; i < c.zoneColor.size(); i++) f(c.zoneColor[i], c.zoneColorDark[i]);
  }
  for (ScreenObject& child : obj.children) forEachObjectPair(child, f);
}

template <typename F>
void forEachPair(ProjectConfig& project, F& f) {
  for (Screen& screen : project.screens) {
    f(screen.backgroundColor, screen.backgroundColorDark);
    f(screen.pullTabColor, screen.pullTabColorDark);
    for (ScreenObject& obj : screen.objects) forEachObjectPair(obj, f);
  }
  // Navigators after the screens (contract 2.7): their ground and every
  // entry object, in a fixed order.
  for (Navigator& nav : project.navigators) {
    f(nav.backgroundColor, nav.backgroundColorDark);
    f(nav.borderColor, nav.borderColorDark);
    for (ScreenObject& obj : nav.themeSwitchObjects) forEachObjectPair(obj, f);
    for (NavigatorEntry& entry : nav.entries) {
      for (ScreenObject& obj : entry.normal) forEachObjectPair(obj, f);
      for (ScreenObject& obj : entry.active) forEachObjectPair(obj, f);
    }
    for (SpiramObjects& arrow : nav.back) {
      for (ScreenObject& obj : arrow) forEachObjectPair(obj, f);
    }
  }
  // Popups after the screens, always in the same order, so that the swaps
  // recorded on the way to dark are undone pair for pair.
  for (Screen& popup : project.popups) {
    f(popup.backgroundColor, popup.backgroundColorDark);
    f(popup.borderColor, popup.borderColorDark);
    for (ScreenObject& obj : popup.objects) forEachObjectPair(obj, f);
  }
}

/**
 * Shows the project in the dark variant (dark = true) or the light one.
 * Asking for the variant it is already in changes nothing.
 */
inline void apply(ProjectConfig& project, bool dark) {
  if (dark == project.themeDark) return;
  if (dark) {
    project.themeSwapped.clear();
    auto toDark = [&project](String& light, String& darkValue) {
      bool swap = !darkValue.isEmpty();
      if (swap) std::swap(light, darkValue);
      project.themeSwapped.push_back(swap);
    };
    forEachPair(project, toDark);
  } else {
    size_t i = 0;
    auto toLight = [&project, &i](String& light, String& darkValue) {
      if (i < project.themeSwapped.size() && project.themeSwapped[i]) std::swap(light, darkValue);
      i++;
    };
    forEachPair(project, toLight);
    project.themeSwapped.clear();
  }
  project.themeDark = dark;
}

/** Whether a payload of schaltli/state/theme means dark. Anything else - empty, unknown - is light. */
inline bool isDarkPayload(String payload) {
  payload.trim();
  payload.toLowerCase();
  return payload == "dark";
}

}  // namespace themevariant
