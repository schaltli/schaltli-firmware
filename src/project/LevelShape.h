#pragma once

// The geometry of a level indicator, as whole pixels.
//
// A line-for-line mirror of lib/level-shape.ts in the designer repo. The two
// have to produce the same integers or conformance finds the difference, so
// this file is deliberately boring: the same names, the same order, the same
// arithmetic. When one changes, the other changes in the same commit.
//
// Integer division throughout, and the TypeScript side uses Math.trunc for the
// same reason - C++ integer division truncates toward zero and that is the side
// that cannot be changed.
//
// Why the shape is what it is: docs/2026-09-19-slider-look.md in the designer
// repo. In short: one colour, a track whose unfilled part is mixed from it and
// the screen behind it (or an outline where the two cannot be told apart), a
// thickness the author sets, one handle with a gap cut around it wherever it
// stands, and the number in a column of its own at the bar's far end.
//
// There used to be a header line above the bar carrying a name and an icon. A
// Bar and a Slider have neither since 2026-09-29 (designer commit 84f3fb7): a
// label beside one is a Text object, an icon an Icon object, and the export
// never sends either. What is left here is exactly what was drawn without one.

#include <Arduino.h>

#include <vector>

#include "MaterialColors.h"
#include "ProjectTypes.h"

struct LevelRect {
  int x = 0;
  int y = 0;
  int w = 0;
  int h = 0;
  int r = 0;  // corner radius; fillRoundRect clamps it to half the short side

  bool valid() const { return w > 0 && h > 0; }
};

// One run of track: whether it is the filled part, and which of its two ends is
// the track's own outer end. An inner end - where the fill meets the unfilled
// part, or where the handle's gap cuts the run - is square, which is Material's
// 2 dp inner corner taken to its limit.
struct LevelSegment : LevelRect {
  bool isFill = false;
  bool roundStart = false;
  bool roundEnd = false;
};

// The vertical measure of the object's font: everything the number's column is
// built from. The renderer fills this from the BdfFont the object names - never from
// properties.fontSize, which nothing keeps in step with the font.
struct LevelFontMetrics {
  int ascent = 11;
  int descent = 3;
  int capHeight = 10;
};

// What the unfilled track looks like: a colour, or - when that colour cannot be
// told from the background - an outline in the bar's own colour.
struct LevelTrackLook {
  String track;
  bool framed = false;
};

struct LevelLayout {
  int baseline = 0;  // the row the number stands on, in the middle of its column
  bool hasValue = false;
  LevelRect value;  // the number's own column beside the bar
  LevelRect bar;    // what is left over for the bar
  LevelRect slot;   // the band of it the bar takes, across
  LevelRect track;  // the track inside that
};

namespace levelshape {

// The padding along the bar's own direction. Deliberately still the 4 this bar
// has always had: it is what percentFromPoint inverts, so widening it would
// move every value's position and silently change what every existing
// calibration means.
constexpr int kPaddingAlong = 4;
// The slot between the bar and its number.
constexpr int kGap = 6;
// Material's own track, and what an unset barThickness means.
constexpr int kDefaultThickness = 16;

inline bool isVertical(const ScreenObject& obj) {
  const String& dir = obj.properties.barDirection;
  return dir == "bottom-to-top" || dir == "top-to-bottom";
}

inline bool fillsFromEnd(const ScreenObject& obj) {
  const String& dir = obj.properties.barDirection;
  return dir == "right-to-left" || dir == "bottom-to-top";
}

// The object's own fontSize, for an object with no project font to take its
// measure from. Only a fallback: with a font chosen, every measurement comes
// from that font.
inline int fontSize(const ScreenObject& obj) {
  const int size = obj.properties.fontSize;
  return size > 0 ? size : 14;
}

inline bool showsNumber(const ScreenObject& obj) { return obj.properties.displayValue != "none"; }

// How thick the track is, across the bar: the author's barThickness, and
// Material's 16 when there is none.
inline int thickness(const ScreenObject& obj) {
  const int t = obj.properties.barThickness;
  return t > 0 ? t : kDefaultThickness;
}

// Whether the object can ever draw a handle: a finger can set it, or it reports
// a target.
inline bool hasHandle(const ScreenObject& obj) {
  return !obj.properties.writeTopic.isEmpty() || !obj.properties.setpointTopic.isEmpty();
}

// How long the handle is, across the bar: Material's 44 on its 16, as a ratio.
inline int handleLength(int thick) { return (thick * 11) / 4; }

// How long the handle ASKS to be, across the bar - what the author's thickness
// implies, before the object's own size across has had a say.
//
// Everything about the handle ALONG the bar - how wide it is, how much room is
// left either side of it - is measured from this and never from the slot it
// ended up in. An object too short for its handle clamps the handle's length,
// since it cannot stand out of a bar that is not there; its width runs along
// the bar, where nothing is short of room.
//
// Measuring from the clamped slot instead is what made a bar squeezed flat - it
// was a header above it then that left the slot short - draw a 3 px handle
// with a 2 px gap where the designer draws 4 and 6. The 4.3B conformance run saw that
// on 2026-09-22 as 170 differing pixels, all of them the two track runs beside
// the handle and the handle's own outline. The designer's levelHandleSpan is
// this function (lib/level-shape.ts).
inline int handleSpan(const ScreenObject& obj) { return handleLength(thickness(obj)); }

// One line of the font: ascent plus descent.
inline int lineHeight(const LevelFontMetrics& m) { return m.ascent + m.descent; }

// One digit, as all four renderers agree to guess it: 0.62 of the line height.
// A guess, and only where it decides where the bar ends - the number column
// beside the bar.
inline int digitWidth(int line) {
  const int w = (line * 62) / 100;
  return w < 1 ? 1 : w;
}

// How wide the handle is, and how much room is left around it - both from the
// handle's length across the bar: Material's 4 and 6 on its 44.
inline int handleWidth(int across) {
  const int w = across / 11;
  return w < 3 ? 3 : w;
}

inline int handleGap(int across) {
  const int gap = (across * 3) / 22;
  return gap < 2 ? 2 : gap;
}

// The number column beside the bar. Five digits, capped at 40 %.
inline int valueWidth(const ScreenObject& obj, const LevelFontMetrics& m) {
  if (!showsNumber(obj)) return 0;
  const int wanted = digitWidth(lineHeight(m)) * 5;
  const int cap = (obj.width * 2) / 5;
  const int out = wanted < cap ? wanted : cap;
  return out > 0 ? out : 0;
}

// The track's own box: inset by 4 along the bar, and centred in the slot across
// it.
inline LevelRect trackInside(const LevelRect& slot, bool vertical, int thick) {
  const int across = vertical ? slot.w : slot.h;
  const int t = thick < across ? thick : across;
  const int padAcross = (across - t) / 2;
  LevelRect out;
  out.x = slot.x + (vertical ? padAcross : kPaddingAlong);
  out.y = slot.y + (vertical ? kPaddingAlong : padAcross);
  out.w = vertical ? t : slot.w - 2 * kPaddingAlong;
  out.h = vertical ? slot.h - 2 * kPaddingAlong : t;
  const int shorter = out.w < out.h ? out.w : out.h;
  out.r = shorter / 2;
  return out;
}

inline LevelLayout layout(const ScreenObject& obj, const LevelFontMetrics& m) {
  const int x = obj.x;
  const int y = obj.y;
  const int w = obj.width;
  const int h = obj.height;
  const bool vertical = isVertical(obj);
  const int line = lineHeight(m);

  LevelLayout lay;
  lay.baseline = y + m.ascent;
  int barW = w, barH = h;

  if (showsNumber(obj)) {
    // The number goes at the far end of the bar's own axis. Right for a
    // horizontal bar whichever way it fills, so a column of bars lines up
    // regardless of their directions.
    lay.hasValue = true;
    if (vertical) {
      const int cap = (h * 2) / 5;
      const int rowH = line < cap ? line : cap;
      lay.value = LevelRect{x, y + h - rowH, w, rowH, 0};
      barH = h - rowH - kGap;
    } else {
      const int valueW = valueWidth(obj, m);
      lay.value = LevelRect{x + w - valueW, y, valueW, h, 0};
      barW = w - valueW - kGap;
    }
    lay.baseline = lay.value.y + (lay.value.h - line) / 2 + m.ascent;
  }

  lay.bar = LevelRect{x, y, barW > 0 ? barW : 0, barH > 0 ? barH : 0, 0};

  const int thick = thickness(obj);
  const int across = vertical ? lay.bar.w : lay.bar.h;
  // Room for a glow on both sides of the track as well (LevelGlow.h), which
  // a handle's own room already gives where there is a handle.
  const int handleOrTrack = hasHandle(obj) ? handleLength(thick) : thick;
  const int glowRoom = thick + 2 * (obj.properties.glow > 8 ? 8 : (obj.properties.glow > 0 ? obj.properties.glow : 0));
  const int wanted = handleOrTrack > glowRoom ? handleOrTrack : glowRoom;
  int size = wanted < across ? wanted : across;
  if (size < 0) size = 0;
  const int offset = (across - size) / 2;
  lay.slot = vertical ? LevelRect{lay.bar.x + offset, lay.bar.y, size, lay.bar.h, 0}
                      : LevelRect{lay.bar.x, lay.bar.y + offset, lay.bar.w, size, 0};
  lay.track = trackInside(lay.slot, vertical, thick);
  return lay;
}

// Where along the track a percentage falls, in pixels from the track's start -
// the one place the value-to-position mapping lives.
inline int edgeFor(const LevelRect& track, bool vertical, bool fromEnd, float percent) {
  const int span = vertical ? track.h : track.w;
  const int along = (int)((float)span * percent / 100.0f);
  if (vertical) return fromEnd ? track.y + track.h - along : track.y + along;
  return fromEnd ? track.x + track.w - along : track.x + along;
}

inline int clampInt(int v, int lo, int hi) {
  if (hi < lo) return lo;
  if (v < lo) return lo;
  if (v > hi) return hi;
  return v;
}

// The handle: a pill across the slot, standing out of the track on both sides.
// Clamped to stay inside the track's run rather than the track being inset to
// make room, which would move every value's position.
inline LevelRect handleRect(const ScreenObject& obj, const LevelLayout& lay, float percent) {
  const bool vertical = isVertical(obj);
  const int span = vertical ? lay.track.h : lay.track.w;
  int thick = handleWidth(handleSpan(obj));
  const int third = span / 3;
  if (thick > third) thick = third;
  if (thick < 2) thick = 2;
  const int edge = edgeFor(lay.track, vertical, fillsFromEnd(obj), percent);
  LevelRect out;
  out.r = thick / 2;
  if (vertical) {
    out.x = lay.slot.x;
    out.w = lay.slot.w;
    out.h = thick;
    out.y = clampInt(edge - thick / 2, lay.track.y, lay.track.y + lay.track.h - thick);
    return out;
  }
  out.y = lay.slot.y;
  out.h = lay.slot.h;
  out.w = thick;
  out.x = clampInt(edge - thick / 2, lay.track.x, lay.track.x + lay.track.w - thick);
  return out;
}

// Between the track and a pointer's tip.
constexpr int kPointerGap = 2;

// A pointer's size, from the track's thickness: two thirds of it from tip to
// base, and a base as wide as that is long - the proportions the user passed
// on the sketch (2026-09-28). `room` is how much the object has beside the
// track; a pointer that would not fit shrinks, one with no room is not drawn.
// levelPointerSize in the designer's lib/level-shape.ts.
inline bool pointerSize(int thick, int room, int& length, int& half) {
  length = (2 * thick + 1) / 3;
  if (length > room - kPointerGap) length = room - kPointerGap;
  if (length < 1) return false;
  half = (length + 1) / 2;
  return true;
}

// The pointer a bar shows its setpoint with: a triangle beside the track,
// pointing at the value - below a horizontal track, right of a vertical one.
// A bar cannot be moved, so it has no handle. It sits in the room a handle
// would take, and the track is not cut for it. Clamped into the slot along
// the bar, like the handle. `pointsLeft` says which way it points: left for a
// vertical bar, up for a horizontal one. levelPointerBand in the designer.
inline bool pointerRect(const ScreenObject& obj, const LevelLayout& lay, float percent, LevelRect& out,
                        bool& pointsLeft) {
  const bool vertical = isVertical(obj);
  const int room = vertical ? lay.slot.x + lay.slot.w - (lay.track.x + lay.track.w)
                            : lay.slot.y + lay.slot.h - (lay.track.y + lay.track.h);
  int length = 0, half = 0;
  if (!pointerSize(thickness(obj), room, length, half)) return false;
  const int edge = edgeFor(lay.track, vertical, fillsFromEnd(obj), percent);
  const int base = 2 * half;
  out.r = 0;
  pointsLeft = vertical;
  if (vertical) {
    out.x = lay.track.x + lay.track.w + kPointerGap;
    out.y = clampInt(edge - half, lay.slot.y, lay.slot.y + lay.slot.h - base);
    out.w = length;
    out.h = base;
    return true;
  }
  out.x = clampInt(edge - half, lay.slot.x, lay.slot.x + lay.slot.w - base);
  out.y = lay.track.y + lay.track.h + kPointerGap;
  out.w = base;
  out.h = length;
  return true;
}

// The track, cut into the runs that actually get painted: filled up to the
// value, unfilled beyond it, and nothing at all where the handle and its gap
// sit.
inline void segments(const ScreenObject& obj, const LevelLayout& lay, float fillPercent,
                     const LevelRect* handle, std::vector<LevelSegment>& out) {
  out.clear();
  const bool vertical = isVertical(obj);
  const bool fromEnd = fillsFromEnd(obj);
  const LevelRect& track = lay.track;
  const int edge = edgeFor(track, vertical, fromEnd, fillPercent);

  const int start = vertical ? track.y : track.x;
  const int end = start + (vertical ? track.h : track.w);

  struct Run {
    int a;
    int b;
    bool isFill;
  };
  Run runs[2];
  if (fromEnd) {
    runs[0] = Run{start, edge, false};
    runs[1] = Run{edge, end, true};
  } else {
    runs[0] = Run{start, edge, true};
    runs[1] = Run{edge, end, false};
  }

  const int gap = handleGap(handleSpan(obj));
  const int cutA = handle ? (vertical ? handle->y : handle->x) - gap : 0;
  const int cutB = handle ? (vertical ? handle->y + handle->h : handle->x + handle->w) + gap : 0;

  auto push = [&](int a, int b, bool isFill) {
    if (b - a <= 0) return;
    LevelSegment seg;
    seg.isFill = isFill;
    seg.roundStart = a == start;
    seg.roundEnd = b == end;
    seg.r = track.r;
    if (vertical) {
      seg.x = track.x;
      seg.y = a;
      seg.w = track.w;
      seg.h = b - a;
    } else {
      seg.x = a;
      seg.y = track.y;
      seg.w = b - a;
      seg.h = track.h;
    }
    out.push_back(seg);
  };

  for (const Run& run : runs) {
    if (!handle || cutB <= run.a || cutA >= run.b) {
      push(run.a, run.b, run.isFill);
      continue;
    }
    push(run.a, run.b < cutA ? run.b : cutA, run.isFill);
    push(run.a > cutB ? run.a : cutB, run.b, run.isFill);
  }
}

// The whole track as one run, for a bar that has heard no value yet.
inline LevelSegment emptyTrack(const LevelLayout& lay) {
  LevelSegment seg;
  seg.x = lay.track.x;
  seg.y = lay.track.y;
  seg.w = lay.track.w;
  seg.h = lay.track.h;
  seg.r = lay.track.r;
  seg.isFill = false;
  seg.roundStart = true;
  seg.roundEnd = true;
  return seg;
}

// What the unfilled part of the track looks like, worked out from the bar's
// colour and the screen behind it: halfway between them, rounded toward the
// background (C++'s integer division truncates toward zero, which is that
// side), then snapped to what this panel can show. Where the result cannot be
// told from the background - all of 1 bit - the track is drawn as an outline in
// the bar's colour instead of a body.
inline LevelTrackLook trackLook(const String& fillColor, const String& background) {
  LevelTrackLook look;
  const String fill = materialcolors::applyDepth(fillColor);
  const String ground = materialcolors::applyDepth(background);
  const materialcolors::Rgb f = materialcolors::channels(fill);
  const materialcolors::Rgb b = materialcolors::channels(ground);
  if (!f.valid || !b.valid) {
    look.track = ground;
    look.framed = true;
    return look;
  }
  const String mixed =
      materialcolors::toHex(b.r + (f.r - b.r) / 2, b.g + (f.g - b.g) / 2, b.b + (f.b - b.b) / 2);
  look.track = materialcolors::applyDepth(mixed);
  const materialcolors::Rgb t = materialcolors::channels(look.track);
  look.framed = t.valid && t.r == b.r && t.g == b.g && t.b == b.b;
  return look;
}

// What a level's empty track is painted with (designer lib/level-shape.ts
// levelTrackPaint, 2026-09-30): its colour and an edge are the theme's, carried
// in trackColor and trackEdgeColor. An edge frames the track, EDGE_WIDTH px, the
// body in the track's colour. Without one, a track that cannot be told from
// the background - all of 1 bit - is framed in the fill's colour, 1 px, with no
// body, as before. Without a trackColor the track is derived (trackLook).
constexpr int EDGE_WIDTH = 2;

struct LevelTrackPaint {
  String track;
  bool framed = false;
  String edge;
  int edgeWidth = 1;
  bool body = false;
};

inline bool isHexColour(const String& s) { return s.length() == 7 && s.charAt(0) == '#'; }

inline LevelTrackPaint trackPaint(const String& trackColor, const String& trackEdgeColor,
                                  const String& fillHex, const String& background) {
  LevelTrackPaint paint;
  const LevelTrackLook derived = trackLook(fillHex, background);
  paint.track = isHexColour(trackColor) ? materialcolors::applyDepth(trackColor) : derived.track;
  if (isHexColour(trackEdgeColor)) {
    paint.framed = true;
    paint.edge = materialcolors::applyDepth(trackEdgeColor);
    paint.edgeWidth = EDGE_WIDTH;
    paint.body = true;
    return paint;
  }
  const materialcolors::Rgb t = materialcolors::channels(paint.track);
  const materialcolors::Rgb b = materialcolors::channels(materialcolors::applyDepth(background));
  paint.framed = !t.valid || !b.valid || (t.r == b.r && t.g == b.g && t.b == b.b);
  paint.edge = fillHex;
  paint.edgeWidth = 1;
  paint.body = false;
  return paint;
}

// The inside of a framed run: the run itself, taken in by one pixel across, and
// along it only at an end that is the track's own. At an end that was cut the
// frame is left open, so it is one path that stops straight where the handle
// begins rather than a string of small pills each closing with a rounded cap.
inline bool frameInner(const LevelSegment& seg, bool vertical, LevelSegment& out, int width = 1) {
  const int a0 = seg.roundStart ? width : 0;
  const int a1 = seg.roundEnd ? width : 0;
  out = seg;
  if (vertical) {
    out.x = seg.x + width;
    out.y = seg.y + a0;
    out.w = seg.w - 2 * width;
    out.h = seg.h - a0 - a1;
  } else {
    out.x = seg.x + a0;
    out.y = seg.y + width;
    out.w = seg.w - a0 - a1;
    out.h = seg.h - 2 * width;
  }
  out.r = seg.r > width ? seg.r - width : 0;
  return out.w > 0 && out.h > 0;
}

}  // namespace levelshape
