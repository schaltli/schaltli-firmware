// Prints everything src/project/LevelShape.h makes of one bar, so
// tools/level-shape/run.js can hold it to the designer's own recording.
//
// One case per run, read from the file named on the command line:
//
//   line 1: type x y width height
//   line 2: percent setpointPercent          (setpoint < 0 means "none")
//   line 3: fillColor background displayValue barDirection barThickness
//           writeTopic setpointTopic fontSize glow   ("-" for empty)
//   line 4: ascent descent capHeight
//
// The font metrics are an INPUT, not an answer: on the device they come from
// the BdfFont the project ships, in the browser from the font the designer
// loaded. What is compared is the layout rule, not two font engines - the same
// split the designer's own recorder makes (build-level-golden.js), which is why
// its `fonts` array never reaches this program.
#include <cstdio>
#include <fstream>
#include <string>
#include <vector>

#include "LevelShape.h"
#include "ProjectTypes.h"

static String readWord(std::ifstream& in) {
  std::string w;
  in >> w;
  return String(w == "-" ? "" : w.c_str());
}

static void printRect(const char* label, const LevelRect& r) {
  printf("%s %d %d %d %d %d\n", label, r.x, r.y, r.w, r.h, r.r);
}

static void printMaybeRect(const char* label, bool has, const LevelRect& r) {
  if (!has) {
    printf("%s 0 0 0 0 0 0\n", label);
    return;
  }
  printf("%s 1 %d %d %d %d %d\n", label, r.x, r.y, r.w, r.h, r.r);
}

static void printSegment(const char* label, const LevelSegment& s) {
  printf("%s %d %d %d %d %d %s %d %d\n", label, s.x, s.y, s.w, s.h, s.r,
         s.isFill ? "fill" : "track", s.roundStart ? 1 : 0, s.roundEnd ? 1 : 0);
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
  int x = 0, y = 0, w = 0, h = 0;
  in >> type >> x >> y >> w >> h;

  float percent = 0.0f, setpoint = -1.0f;
  in >> percent >> setpoint;

  const String fillColor = readWord(in);
  const String background = readWord(in);
  const String displayValue = readWord(in);
  const String barDirection = readWord(in);
  int barThickness = 0, fontSizeProp = 0, glowProp = 0;
  in >> barThickness;
  const String writeTopic = readWord(in);
  const String setpointTopic = readWord(in);
  in >> fontSizeProp >> glowProp;

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
  obj.properties.fillColor = fillColor;
  obj.properties.displayValue = displayValue;
  obj.properties.barDirection = barDirection.isEmpty() ? String("left-to-right") : barDirection;
  obj.properties.barThickness = barThickness;
  obj.properties.writeTopic = writeTopic;
  obj.properties.setpointTopic = setpointTopic;
  obj.properties.glow = glowProp;
  if (fontSizeProp > 0) obj.properties.fontSize = fontSizeProp;

  const LevelLayout lay = levelshape::layout(obj, metrics);
  const bool vertical = levelshape::isVertical(obj);
  // As the renderer decides it since 2026-09-28: only a slider has a handle
  // (objecttypes::isSettable, spelled out here to keep this program free of
  // the rest of the project); a bar has a pointer beside its track instead.
  const bool settable = obj.type == "slider" || obj.type == "dial";
  const bool handled = levelshape::hasHandle(obj) && settable;

  printf("flags %d %d %d %d %d %d\n", vertical ? 1 : 0,
         levelshape::fillsFromEnd(obj) ? 1 : 0, levelshape::thickness(obj), levelshape::hasHandle(obj) ? 1 : 0,
         levelshape::showsNumber(obj) ? 1 : 0, levelshape::valueWidth(obj, metrics));
  printf("metrics %d %d %d\n", metrics.ascent, metrics.descent, metrics.capHeight);

  printf("baseline %d\n", lay.baseline);
  printMaybeRect("value", lay.hasValue, lay.value);
  printRect("bar", lay.bar);
  printRect("slot", lay.slot);
  printRect("laytrack", lay.track);
  printRect("track", lay.track);
  printSegment("empty", levelshape::emptyTrack(lay));

  // The handle stands where the setpoint is when there is one, and on the
  // reported value when there is not - the same choice the recorder makes.
  LevelRect handle;
  if (handled) {
    handle = levelshape::handleRect(obj, lay, setpoint >= 0.0f ? setpoint : percent);
  }
  printMaybeRect("handle", handled, handle);

  LevelRect pointer;
  bool pointsLeft = false;
  const bool pointed = levelshape::hasHandle(obj) && !settable && setpoint >= 0.0f &&
                       levelshape::pointerRect(obj, lay, setpoint, pointer, pointsLeft);
  if (pointed) {
    printf("pointer 1 %d %d %d %d %s\n", pointer.x, pointer.y, pointer.w, pointer.h, pointsLeft ? "left" : "up");
  } else {
    printf("pointer 0\n");
  }

  std::vector<LevelSegment> segs;
  levelshape::segments(obj, lay, percent, handled ? &handle : nullptr, segs);
  printf("segments %d\n", (int)segs.size());
  for (size_t i = 0; i < segs.size(); i++) printSegment("seg", segs[i]);

  // The frame a track that cannot be told from its background is drawn with.
  // Not in the recording - the designer keeps levelFrameInner to itself - but
  // printed so that a case added there later has something to compare against
  // rather than silently checking nothing.
  for (size_t i = 0; i < segs.size(); i++) {
    LevelSegment inner;
    if (levelshape::frameInner(segs[i], vertical, inner)) printSegment("frame", inner);
    else printf("frame - - - - - - - -\n");
  }

  const LevelTrackLook look = levelshape::trackLook(fillColor, background);
  printf("look %s %d\n", hexOf(look.track), look.framed ? 1 : 0);
  return 0;
}
