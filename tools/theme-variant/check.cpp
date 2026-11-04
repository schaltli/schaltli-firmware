// Holds src/project/ThemeVariant.h to the reader rule of the dark variant, on
// a laptop - run by tools/theme-variant/run.js.
//
// The rule is the designer's (lib/themes.ts darkVariantOf,
// docs/device-contract.md §2.3), and the cases below are the ones its golden
// records (hil/android/fixtures/build-theme-variant-golden.js), written as the
// structs this firmware holds instead of as JSON: this side never sees the
// JSON again after loading, so what has to be right is the swap on the
// parsed project.
#include <cstdio>
#include "ThemeVariant.h"

static int failures = 0;

static void expect(bool ok, const char* what) {
  if (!ok) {
    std::printf("FAIL %s\n", what);
    failures++;
  }
}

static ProjectConfig sample() {
  ProjectConfig p;
  Screen s;
  s.backgroundColor = "#f4f6f8";
  s.backgroundColorDark = "#15202b";

  ScreenObject label;
  label.properties.color = "#1e2a36";
  label.properties.colorDark = "#e6edf3";
  label.properties.backgroundColor = "transparent";  // no dark twin: stays
  s.objects.push_back(label);

  ScreenObject icon;
  icon.path = "/PROJECT/assets/s_icon.bmp";
  icon.pathDark = "/PROJECT/assets/s_icon-dark.bmp";
  s.objects.push_back(icon);

  // A button's look (since 2026-11-04): faces light and dark, an icon each.
  ScreenObject button;
  button.properties.button.p.reset(new ButtonLookProps());
  ButtonLookProps& look = *button.properties.button;
  look.normal.r = 20;
  look.normal.fill = "#6750a4";
  look.normal.ink = "#ffffff";
  look.normalDark.fill = "#d0bcff";
  look.normalDark.ink = "#381e72";
  look.pressed.r = 8;
  look.pressed.fill = "#7662ad";
  look.pressed.ink = "#ffffff";
  look.pressedDark.fill = "#c4b0f0";
  look.iconPath = "/PROJECT/assets/b-icon.bmp";
  look.iconPathDark = "/PROJECT/assets/b-icon-dark.bmp";
  s.objects.push_back(button);

  ScreenObject sw;
  sw.properties.switchColor = "#2f6f9f";
  sw.properties.switchColorDark = "#6aa8d8";
  SwitchState off;
  off.iconPath = "/PROJECT/assets/off.bmp";
  SwitchState on;
  on.iconPath = "/PROJECT/assets/on.bmp";
  on.iconPathDark = "/PROJECT/assets/on-dark.bmp";
  on.iconPathActive = "/PROJECT/assets/on-a.bmp";
  on.iconPathActiveDark = "/PROJECT/assets/on-a-dark.bmp";
  sw.properties.states.push_back(off);
  sw.properties.states.push_back(on);
  ValueIconPair pair;
  pair.path = "/PROJECT/assets/p.bmp";
  pair.pathDark = "/PROJECT/assets/p-dark.bmp";
  sw.properties.valueIconPairs.push_back(pair);
  s.objects.push_back(sw);

  // A tab-control's child, one panel down.
  ScreenObject tabs;
  ScreenObject panel;
  ScreenObject box;
  box.properties.fillColor = "#e5e5e5";
  box.properties.fillColorDark = "#2b2b2b";
  panel.children.push_back(box);
  tabs.children.push_back(panel);
  s.objects.push_back(tabs);

  // A lone XDark over an empty X - the case a swap without memory gets wrong
  // on the way back.
  ScreenObject lone;
  lone.properties.strokeColor = "";
  lone.properties.strokeColorDark = "#445566";
  s.objects.push_back(lone);

  // A level's second accent, the end of its gradient (2026-09-28).
  ScreenObject level;
  level.properties.fillEndColor = "#1fa37a";
  level.properties.fillEndColorDark = "#3ee08f";
  // Its track and the edge round it, the theme's roles since 2026-09-30.
  level.properties.trackColor = "#b3a8d2";
  level.properties.trackColorDark = "#766b8f";
  level.properties.trackEdgeColor = "transparent";
  level.properties.trackEdgeColorDark = "transparent";
  s.objects.push_back(level);

  // An energy diagram (contract 2.8, 2026-10-12): its colours, a node's
  // colour and its icon's looks.
  ScreenObject energy;
  energy.properties.energy.p.reset(new EnergyDiagramProps());
  energy.properties.energy->busColor = "#6750a4";
  energy.properties.energy->busColorDark = "#d0bcff";
  energy.properties.energy->dischargeColor = "#d0453f";
  energy.properties.energy->dischargeColorDark = "#ff8a80";
  EnergyNodeProps node;
  node.color = "#c99400";
  node.colorDark = "#ffd54f";
  node.icon.live = "/PROJECT/assets/n.bmp";
  node.icon.liveDark = "/PROJECT/assets/n-dark.bmp";
  node.icon.muted = "/PROJECT/assets/m.bmp";
  energy.properties.energy->nodes.push_back(node);
  s.objects.push_back(energy);

  p.screens.push_back(s);
  return p;
}

static bool same(const ProjectConfig& a, const ProjectConfig& b) {
  const Screen& x = a.screens[0];
  const Screen& y = b.screens[0];
  if (!(x.backgroundColor == y.backgroundColor) || !(x.backgroundColorDark == y.backgroundColorDark)) return false;
  if (!(x.objects[0].properties.color == y.objects[0].properties.color)) return false;
  if (!(x.objects[1].path == y.objects[1].path)) return false;
  const ButtonLookProps& bx = *x.objects[2].properties.button;
  const ButtonLookProps& by = *y.objects[2].properties.button;
  if (!(bx.normal.fill == by.normal.fill) || !(bx.pressed.fill == by.pressed.fill) || !(bx.normal.ink == by.normal.ink)) return false;
  if (!(bx.iconPath == by.iconPath)) return false;
  if (!(x.objects[3].properties.states[1].iconPath == y.objects[3].properties.states[1].iconPath)) return false;
  if (!(x.objects[4].children[0].children[0].properties.fillColor ==
        y.objects[4].children[0].children[0].properties.fillColor)) return false;
  if (!(x.objects[5].properties.strokeColor == y.objects[5].properties.strokeColor)) return false;
  if (!(x.objects[5].properties.strokeColorDark == y.objects[5].properties.strokeColorDark)) return false;
  if (!(x.objects[6].properties.fillEndColor == y.objects[6].properties.fillEndColor)) return false;
  if (!(x.objects[6].properties.trackColor == y.objects[6].properties.trackColor)) return false;
  const EnergyDiagramProps& ex = *x.objects[7].properties.energy;
  const EnergyDiagramProps& ey = *y.objects[7].properties.energy;
  if (!(ex.busColor == ey.busColor) || !(ex.nodes[0].color == ey.nodes[0].color) || !(ex.nodes[0].icon.live == ey.nodes[0].icon.live)) return false;
  return true;
}

int main() {
  ProjectConfig p = sample();
  const ProjectConfig light = sample();

  themevariant::apply(p, true);
  const Screen& s = p.screens[0];
  expect(p.themeDark, "reports dark");
  expect(s.backgroundColor == "#15202b", "screen background takes its dark value");
  expect(s.objects[0].properties.color == "#e6edf3", "a label's colour takes its dark value");
  expect(s.objects[0].properties.backgroundColor == "transparent", "transparent without a twin stays");
  expect(s.objects[1].path == "/PROJECT/assets/s_icon-dark.bmp", "an icon's path");
  const ButtonLookProps& darkButton = *s.objects[2].properties.button;
  expect(darkButton.normal.fill == "#d0bcff", "a button's normal fill");
  expect(darkButton.normal.ink == "#381e72", "a button's normal ink");
  expect(darkButton.pressed.fill == "#c4b0f0", "a button's pressed fill");
  expect(darkButton.pressed.ink == "#ffffff", "a pressed ink with no dark twin stays");
  expect(darkButton.normal.r == 20 && darkButton.pressed.r == 8, "a button's radii are one for both");
  expect(darkButton.iconPath == "/PROJECT/assets/b-icon-dark.bmp", "a button's icon");
  expect(s.objects[3].properties.switchColor == "#6aa8d8", "a switch's colour");
  expect(s.objects[3].properties.states[0].iconPath == "/PROJECT/assets/off.bmp", "a state with no dark icon keeps its own");
  expect(s.objects[3].properties.states[1].iconPath == "/PROJECT/assets/on-dark.bmp", "a state's dark icon");
  expect(s.objects[3].properties.states[1].iconPathActive == "/PROJECT/assets/on-a-dark.bmp", "a state's dark active icon");
  expect(s.objects[3].properties.valueIconPairs[0].path == "/PROJECT/assets/p-dark.bmp", "a live icon's pair");
  expect(s.objects[4].children[0].children[0].properties.fillColor == "#2b2b2b", "a tab-control's child");
  expect(s.objects[5].properties.strokeColor == "#445566", "a lone XDark counts as X");
  expect(s.objects[6].properties.fillEndColor == "#3ee08f", "a level's gradient end");
  expect(s.objects[6].properties.trackColor == "#766b8f", "a level's track");
  expect(s.objects[6].properties.trackEdgeColor == "transparent", "a level's track edge, none in both");
  const EnergyDiagramProps& e = *s.objects[7].properties.energy;
  expect(e.busColor == "#d0bcff", "an energy diagram's bus");
  expect(e.dischargeColor == "#ff8a80", "an energy diagram's discharge colour");
  expect(e.nodes[0].color == "#ffd54f", "an energy node's colour");
  expect(e.nodes[0].icon.live == "/PROJECT/assets/n-dark.bmp", "an energy node's live icon");
  expect(e.nodes[0].icon.muted == "/PROJECT/assets/m.bmp", "a look without a dark bake keeps its own");
  // A copied object has a diagram of its own: swapping the copy leaves the original.
  ScreenObject copy = s.objects[7];
  copy.properties.energy->busColor = "#000000";
  expect(e.busColor == "#d0bcff", "a copy's diagram is its own");

  themevariant::apply(p, true);
  expect(s.backgroundColor == "#15202b", "dark twice is still dark, not light again");

  themevariant::apply(p, false);
  expect(!p.themeDark, "reports light");
  expect(same(p, light), "light again is exactly the project as loaded, the lone XDark included");

  themevariant::apply(p, false);
  expect(same(p, light), "light twice changes nothing");

  // Flipping back and forth leaves nothing behind.
  for (int i = 0; i < 5; i++) {
    themevariant::apply(p, true);
    themevariant::apply(p, false);
  }
  expect(same(p, light), "five round trips");

  expect(themevariant::isDarkPayload("dark"), "dark");
  expect(themevariant::isDarkPayload(" DARK\n"), "dark, trimmed, any case");
  expect(!themevariant::isDarkPayload("light"), "light");
  expect(!themevariant::isDarkPayload(""), "empty is light");
  expect(!themevariant::isDarkPayload("dim"), "anything else is light");

  if (failures == 0) std::printf("theme-variant: all cases pass\n");
  return failures == 0 ? 0 : 1;
}
