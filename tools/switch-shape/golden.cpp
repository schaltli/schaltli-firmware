// Prints everything src/project/SwitchShape.h makes of one Switch, so
// tools/switch-shape/run.js can hold it to the designer's own recording.
//
// One case per run, read from the file named on the command line:
//
//   line 1: type x y width height stateCount activeIndex askedIndex
//           pressedIndex textWidth hasIcon
//   line 2: switchColor switchStyle background      ("-" for empty)
//   line 3: ascent descent capHeight
//   line 4: stateCount flags, 1 where that state counts as "on"
//
// The font metrics are an INPUT, not an answer: on the device they come from
// the BDF the project ships, in the browser from the font the designer loaded.
// What is compared is the layout rule, not two font engines - the same split
// the designer's own recorder makes (build-switch-golden.js).
#include <cstdio>
#include <fstream>
#include <string>
#include <vector>

#include "LevelShape.h"
#include "ProjectTypes.h"
#include "SwitchShape.h"

static String readWord(std::ifstream& in) {
  std::string w;
  in >> w;
  return String(w == "-" ? "" : w.c_str());
}

static void printRect(const char* label, const SwitchRect& r) {
  printf("%s %d %d %d %d %d %d\n", label, r.x, r.y, r.w, r.h, r.r, r.rightRadius());
}

static const char* hexOf(const String& s) { return s.isEmpty() ? "-" : s.c_str(); }

int main(int argc, char** argv) {
  if (argc < 2) {
    fprintf(stderr, "usage: golden <case file>\n");
    return 2;
  }
  std::ifstream in(argv[1]);
  if (!in) {
    fprintf(stderr, "cannot read %s\n", argv[1]);
    return 2;
  }

  std::string type;
  int x = 0, y = 0, w = 0, h = 0, count = 0, activeIndex = -1, askedIndex = -1, pressedIndex = -1;
  int textWidth = 0, hasIconFlag = 0;
  in >> type >> x >> y >> w >> h >> count >> activeIndex >> askedIndex >> pressedIndex >> textWidth >>
      hasIconFlag;

  const String switchColor = readWord(in);
  const String switchStyle = readWord(in);
  const String background = readWord(in);

  LevelFontMetrics metrics;
  in >> metrics.ascent >> metrics.descent >> metrics.capHeight;

  ScreenObject obj;
  obj.type = String(type.c_str());
  obj.id = "probe";
  obj.x = x;
  obj.y = y;
  obj.width = w;
  obj.height = h;
  obj.zIndex = 1;
  obj.properties.switchColor = switchColor;
  obj.properties.switchStyle = switchStyle;
  for (int i = 0; i < count; i++) {
    int on = 0;
    in >> on;
    SwitchState state;
    state.showMarker = on != 0;
    obj.properties.states.push_back(state);
  }

  const bool on = activeIndex >= 0 && activeIndex < count &&
                  switchshape::stateIsOn(obj.properties.states[activeIndex]);
  const int shown = askedIndex >= 0 ? askedIndex : activeIndex;
  const bool knobForm = switchshape::isKnobForm(obj);

  std::vector<SwitchRect> segs;
  switchshape::segments(obj, count, segs);
  const SwitchRect track = switchshape::track(obj, count);
  const SwitchRect labelBox = switchshape::labelBox(obj, count);

  printf("form %s\n", knobForm ? "knob" : "group");
  printf("corner %d\n", switchshape::corner(w, h));
  printRect("container", switchshape::container(obj));
  printf("segments %d\n", (int)segs.size());
  for (size_t i = 0; i < segs.size(); i++) printRect("seg", segs[i]);
  printRect("track", track);
  printRect("labelbox", labelBox);

  // What a ring on each button would enclose. Printed per button rather than
  // only for the one ringed now, so the rule is pinned whichever button a
  // finger asks for - and because only a button that is at once the reported
  // state AND the one asked for tells the right answer (its own colour) from
  // the wrong one (the container's). "-" is the designer's null: nothing lies
  // behind a container that is only an outline.
  {
    const SwitchLook ringLook = switchshape::look(obj, background);
    for (int i = 0; i < count; i++) {
      printf("ringfill %s\n", hexOf(switchshape::ringFill(ringLook, i == activeIndex)));
    }
  }

  printf("knobs %d\n", count);
  for (int i = 0; i < count; i++) {
    const switchshape::SwitchKnob k = switchshape::knob(obj, count, i, false, on);
    printf("knob %d %d %d\n", k.cx, k.cy, k.r);
  }
  for (int i = 0; i < count; i++) {
    const switchshape::SwitchKnob k = switchshape::knob(obj, count, i, false, false);
    printf("quietknob %d %d %d\n", k.cx, k.cy, k.r);
  }
  {
    const switchshape::SwitchKnob k =
        switchshape::knob(obj, count, shown > 0 ? shown : 0, true, on);
    printf("pressedknob %d %d %d\n", k.cx, k.cy, k.r);
  }
  printf("shown %d\n", shown);

  // Laid out in whichever box this form puts its content in - the same choice
  // the designer's recorder makes.
  const SwitchRect& contentBox = knobForm ? labelBox : segs[0];
  const SwitchContent content = switchshape::content(contentBox, metrics, hasIconFlag != 0, textWidth);
  printf("content %d %d %d %d %d %d %d %d\n", content.hasIcon ? 1 : 0, content.icon.x, content.icon.y,
         content.icon.w, content.icon.h, content.textX, content.baseline, content.beside ? 1 : 0);

  const SwitchLook look = switchshape::look(obj, background);
  // surfaceFramed is this side's way of saying the designer's
  // `surfaceOutline: string | null` - and where it is framed, the outline is
  // drawn in the author's own colour (ColorScreenRenderer's group path).
  printf("look %s %d %s %s %s %s %s\n", hexOf(look.surface), look.surfaceFramed ? 1 : 0,
         hexOf(materialcolors::applyDepth(switchshape::colorOf(obj))), hexOf(look.chosen),
         hexOf(look.onChosen), hexOf(look.onSurface), hexOf(look.ring));

  const SwitchKnobLook knobLook = switchshape::knobLook(obj, background, on);
  printf("knoblook %s %s %s %s\n", hexOf(knobLook.track), hexOf(knobLook.trackOutline),
         hexOf(knobLook.knob), hexOf(knobLook.onKnob));
  const SwitchKnobLook knobLookOff = switchshape::knobLook(obj, background, false);
  printf("knoblookoff %s %s %s %s\n", hexOf(knobLookOff.track), hexOf(knobLookOff.trackOutline),
         hexOf(knobLookOff.knob), hexOf(knobLookOff.onKnob));
  printf("on %d\n", on ? 1 : 0);
  return 0;
}
