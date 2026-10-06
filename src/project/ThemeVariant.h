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
  f(obj.path, obj.pathDark);
  f(obj.pathNormal, obj.pathNormalDark);
  f(obj.pathActive, obj.pathActiveDark);
  for (SwitchState& state : p.states) {
    f(state.iconPath, state.iconPathDark);
    f(state.iconPathActive, state.iconPathActiveDark);
  }
  for (ValueIconPair& pair : p.valueIconPairs) f(pair.path, pair.pathDark);
  for (ScreenObject& child : obj.children) forEachObjectPair(child, f);
}

template <typename F>
void forEachPair(ProjectConfig& project, F& f) {
  for (Screen& screen : project.screens) {
    f(screen.backgroundColor, screen.backgroundColorDark);
    for (ScreenObject& obj : screen.objects) forEachObjectPair(obj, f);
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
