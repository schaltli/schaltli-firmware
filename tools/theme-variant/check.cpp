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

  ScreenObject button;
  button.pathNormal = "/PROJECT/assets/b.bmp";
  button.pathNormalDark = "/PROJECT/assets/b-dark.bmp";
  button.pathActive = "/PROJECT/assets/b-a.bmp";
  button.pathActiveDark = "/PROJECT/assets/b-a-dark.bmp";
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

  p.screens.push_back(s);
  return p;
}

static bool same(const ProjectConfig& a, const ProjectConfig& b) {
  const Screen& x = a.screens[0];
  const Screen& y = b.screens[0];
  if (!(x.backgroundColor == y.backgroundColor) || !(x.backgroundColorDark == y.backgroundColorDark)) return false;
  if (!(x.objects[0].properties.color == y.objects[0].properties.color)) return false;
  if (!(x.objects[1].path == y.objects[1].path)) return false;
  if (!(x.objects[2].pathNormal == y.objects[2].pathNormal)) return false;
  if (!(x.objects[3].properties.states[1].iconPath == y.objects[3].properties.states[1].iconPath)) return false;
  if (!(x.objects[4].children[0].children[0].properties.fillColor ==
        y.objects[4].children[0].children[0].properties.fillColor)) return false;
  if (!(x.objects[5].properties.strokeColor == y.objects[5].properties.strokeColor)) return false;
  if (!(x.objects[5].properties.strokeColorDark == y.objects[5].properties.strokeColorDark)) return false;
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
  expect(s.objects[2].pathNormal == "/PROJECT/assets/b-dark.bmp", "a button's normal face");
  expect(s.objects[2].pathActive == "/PROJECT/assets/b-a-dark.bmp", "a button's active face");
  expect(s.objects[3].properties.switchColor == "#6aa8d8", "a switch's colour");
  expect(s.objects[3].properties.states[0].iconPath == "/PROJECT/assets/off.bmp", "a state with no dark icon keeps its own");
  expect(s.objects[3].properties.states[1].iconPath == "/PROJECT/assets/on-dark.bmp", "a state's dark icon");
  expect(s.objects[3].properties.states[1].iconPathActive == "/PROJECT/assets/on-a-dark.bmp", "a state's dark active icon");
  expect(s.objects[3].properties.valueIconPairs[0].path == "/PROJECT/assets/p-dark.bmp", "a live icon's pair");
  expect(s.objects[4].children[0].children[0].properties.fillColor == "#2b2b2b", "a tab-control's child");
  expect(s.objects[5].properties.strokeColor == "#445566", "a lone XDark counts as X");

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
