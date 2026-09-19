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
// repo. In short: a pill-ended track, a tinted unfilled part, one handle with a
// gap cut around it wherever it stands, and - when the object carries a name or
// an icon - a header line above the bar.

#include <Arduino.h>

#include "ProjectTypes.h"

struct LevelRect {
  int x = 0;
  int y = 0;
  int w = 0;
  int h = 0;
  int r = 0;  // corner radius; fillRoundRect clamps it to half the short side

  bool valid() const { return w > 0 && h > 0; }
};

// One run of track: which colour it takes, and which of its two ends is the
// track's own outer end. An inner end - where the fill meets the tinted part,
// or where the handle's gap cuts the run - is square, which is Material's 2 dp
// inner corner taken to its limit.
struct LevelSegment : LevelRect {
  bool isFill = false;
  bool roundStart = false;
  bool roundEnd = false;
};

struct LevelLayout {
  bool hasHeader = false;
  LevelRect header;
  bool hasIcon = false;
  LevelRect icon;
  bool hasName = false;
  LevelRect name;
  bool hasValue = false;
  LevelRect value;
  bool hasSub = false;
  LevelRect sub;
  LevelRect bar;    // what is left over for the bar
  LevelRect track;  // the track inside it
};

namespace levelshape {

// The padding along the bar's own direction. Deliberately still the 4 this bar
// has always had: it is what a finger's position is measured against, so
// widening it would silently change what every existing calibration means.
constexpr int kPaddingAlong = 4;

// The slot between the header's parts, and between the bar and its number.
constexpr int kGap = 6;

inline bool isVertical(const ScreenObject& obj) {
  return obj.properties.barDirection == "bottom-to-top" || obj.properties.barDirection == "top-to-bottom";
}

inline bool fillsFromEnd(const ScreenObject& obj) {
  return obj.properties.barDirection == "right-to-left" || obj.properties.barDirection == "bottom-to-top";
}

// Material's own proportion, kept as a proportion: its track is 16 dp inside a
// 44 dp row, so the margin is 7/22 of the row.
inline int padding(int across) {
  const int pad = (across * 7) / 22;
  return pad < 2 ? 2 : pad;
}

// 4 dp on a 44 dp row.
inline int handleWidth(int across) {
  const int w = across / 11;
  return w < 3 ? 3 : w;
}

// 6 dp on a 44 dp row.
inline int handleGap(int across) {
  const int gap = (across * 3) / 22;
  return gap < 2 ? 2 : gap;
}

inline int fontSize(const ScreenObject& obj) {
  return obj.properties.fontSize > 0 ? obj.properties.fontSize : 14;
}

inline bool hasName(const ScreenObject& obj) {
  String label = obj.properties.label;
  label.trim();
  return !label.isEmpty();
}

// An icon is present when the designer baked one and named it in `path`. The
// device never sees the asset id - only the bitmap.
inline bool hasIcon(const ScreenObject& obj) { return !obj.path.isEmpty(); }

inline bool showsNumber(const ScreenObject& obj) { return obj.properties.displayValue != "none"; }

// Whether the object can ever show a second, measured number beside the
// commanded one. Asked of the object rather than of the moment, so the header
// does not re-flow and the name does not jump every time a command goes out.
inline bool showsSub(const ScreenObject& obj) {
  if (!showsNumber(obj)) return false;
  return !obj.properties.writeTopic.isEmpty() || !obj.properties.setpointTopic.isEmpty();
}

// One digit, as all four renderers agree to guess it: 0.62 of the font size.
// A guess rather than a measurement, because the reserve has to be known where
// no font is at hand and has to be the same integer everywhere. Text is clipped
// to its box, so a font wider than the guess is cut off rather than running
// into the bar.
inline int digitWidth(int size) {
  const int w = (size * 62) / 100;
  return w < 1 ? 1 : w;
}

inline int lineHeight(int size) { return (size * 3) / 2; }

inline int subFontSize(const ScreenObject& obj) {
  const int small = (fontSize(obj) * 2) / 3;
  return small < 6 ? 6 : small;
}

inline int headerHeight(const ScreenObject& obj) {
  if (!hasName(obj) && !hasIcon(obj)) return 0;
  const int wanted = lineHeight(fontSize(obj));
  const int half = obj.height / 2;
  const int h = wanted < half ? wanted : half;
  return h < 0 ? 0 : h;
}

inline int valueWidth(const ScreenObject& obj) {
  if (!showsNumber(obj)) return 0;
  const int wanted = digitWidth(fontSize(obj)) * 5;
  const int cap = (obj.width * 2) / 5;
  const int w = wanted < cap ? wanted : cap;
  return w < 0 ? 0 : w;
}

inline int subWidth(const ScreenObject& obj) {
  if (!showsSub(obj)) return 0;
  const int wanted = digitWidth(subFontSize(obj)) * 8;
  const int cap = (obj.width * 3) / 10;
  const int w = wanted < cap ? wanted : cap;
  return w < 0 ? 0 : w;
}

inline LevelRect trackInside(const LevelRect& bar, bool vertical) {
  const int across = vertical ? bar.w : bar.h;
  const int padAcross = padding(across);
  LevelRect t;
  t.x = bar.x + (vertical ? padAcross : kPaddingAlong);
  t.y = bar.y + (vertical ? kPaddingAlong : padAcross);
  t.w = bar.w - 2 * (vertical ? padAcross : kPaddingAlong);
  t.h = bar.h - 2 * (vertical ? kPaddingAlong : padAcross);
  t.r = (t.w < t.h ? t.w : t.h) / 2;
  return t;
}

inline LevelLayout layout(const ScreenObject& obj) {
  LevelLayout out;
  const int x = obj.x;
  const int y = obj.y;
  const int w = obj.width;
  const int h = obj.height;
  const bool vertical = isVertical(obj);
  const int headerH = headerHeight(obj);

  int barX = x, barY = y, barW = w, barH = h;

  if (headerH > 0) {
    out.hasHeader = true;
    out.header = {x, y, w, headerH, 0};
    int left = x;
    if (hasIcon(obj)) {
      // Square, and never more than a quarter of the line: an icon is a mark
      // beside the name, not a picture.
      int size = headerH < w / 4 ? headerH : w / 4;
      if (size < 1) size = 1;
      out.hasIcon = true;
      out.icon = {left, y + (headerH - size) / 2, size, size, 0};
      left = out.icon.x + size + kGap;
    }
    int right = x + w;
    const int valueW = valueWidth(obj);
    if (valueW > 0) {
      out.hasValue = true;
      out.value = {right - valueW, y, valueW, headerH, 0};
      right = out.value.x;
      const int subW = subWidth(obj);
      if (subW > 0) {
        out.hasSub = true;
        out.sub = {right - kGap - subW, y, subW, headerH, 0};
        right = out.sub.x;
      }
    }
    const int nameW = right - kGap - left;
    if (hasName(obj) && nameW > 0) {
      out.hasName = true;
      out.name = {left, y, nameW, headerH, 0};
    }
    barY = y + headerH;
    barH = h - headerH;
  } else if (showsNumber(obj)) {
    // No header: the number goes at the far end of the bar's own axis. Right
    // for a horizontal bar whichever way it fills, so a column of bars lines up
    // regardless of their directions.
    if (vertical) {
      const int wanted = lineHeight(fontSize(obj));
      const int cap = (h * 2) / 5;
      const int lineH = wanted < cap ? wanted : cap;
      out.hasValue = true;
      out.value = {x, y + h - lineH, w, lineH, 0};
      barH = h - lineH - kGap;
    } else {
      const int valueW = valueWidth(obj);
      out.hasValue = true;
      out.value = {x + w - valueW, y, valueW, h, 0};
      barW = w - valueW - kGap;
    }
  }

  out.bar = {barX, barY, barW < 0 ? 0 : barW, barH < 0 ? 0 : barH, 0};
  out.track = trackInside(out.bar, vertical);
  return out;
}

// Where along the track a percentage falls, in pixels - the one place the
// value-to-position mapping lives.
// A float percentage on purpose. The value-to-position mapping is float on
// both sides and only the RESULT is truncated - truncating the percentage
// first loses up to a hundredth of the bar, which on a 700 px track is seven
// pixels. Measured on 2026-09-19: the handle of a heater at 21 degrees on a
// 5..35 scale (53.33 %) sat three pixels away from where the designer drew it,
// because this took an int while the fill next to it did not.
inline int edgeFor(const LevelRect& track, bool vertical, bool fromEnd, float percent) {
  const int span = vertical ? track.h : track.w;
  const int along = (int)((span * percent) / 100.0f);
  if (vertical) return fromEnd ? track.y + track.h - along : track.y + along;
  return fromEnd ? track.x + track.w - along : track.x + along;
}

inline int clampInt(int v, int lo, int hi) {
  if (hi < lo) return lo;
  if (v < lo) return lo;
  if (v > hi) return hi;
  return v;
}

// The handle: a pill across the whole bar box, standing out of the track on
// both sides. The overhang is what says "a thing lying on top" - and on a 1-bit
// panel it is the only thing that can, since there is no third colour.
inline LevelRect handleRect(const ScreenObject& obj, const LevelLayout& lay, float percent) {
  const bool vertical = isVertical(obj);
  const int across = vertical ? lay.bar.w : lay.bar.h;
  const int span = vertical ? lay.track.h : lay.track.w;
  int thickness = handleWidth(across);
  const int third = span / 3;
  if (thickness > third) thickness = third;
  if (thickness < 2) thickness = 2;
  const int edge = edgeFor(lay.track, vertical, fillsFromEnd(obj), percent);
  const int r = thickness / 2;

  if (vertical) {
    const int hy = clampInt(edge - thickness / 2, lay.track.y, lay.track.y + lay.track.h - thickness);
    return {lay.bar.x, hy, lay.bar.w, thickness, r};
  }
  const int hx = clampInt(edge - thickness / 2, lay.track.x, lay.track.x + lay.track.w - thickness);
  return {hx, lay.bar.y, thickness, lay.bar.h, r};
}

// The track, cut into the runs that actually get painted: filled up to the
// value, tinted beyond it, and nothing at all where the handle and its gap sit.
inline void segments(const ScreenObject& obj, const LevelLayout& lay, float fillPercent, const LevelRect* handle,
                     std::vector<LevelSegment>& out) {
  out.clear();
  const bool vertical = isVertical(obj);
  const bool fromEnd = fillsFromEnd(obj);
  const int edge = edgeFor(lay.track, vertical, fromEnd, fillPercent);

  const int start = vertical ? lay.track.y : lay.track.x;
  const int end = start + (vertical ? lay.track.h : lay.track.w);

  struct Run {
    int a;
    int b;
    bool isFill;
  };
  Run runs[2];
  if (fromEnd) {
    runs[0] = {start, edge, false};
    runs[1] = {edge, end, true};
  } else {
    runs[0] = {start, edge, true};
    runs[1] = {edge, end, false};
  }

  const int across = vertical ? lay.bar.w : lay.bar.h;
  const int gap = handleGap(across);
  const int cutA = handle ? (vertical ? handle->y : handle->x) - gap : 0;
  const int cutB = handle ? (vertical ? handle->y + handle->h : handle->x + handle->w) + gap : 0;

  auto push = [&](int a, int b, bool isFill) {
    if (b - a <= 0) return;
    LevelSegment seg;
    if (vertical) {
      seg.x = lay.track.x;
      seg.y = a;
      seg.w = lay.track.w;
      seg.h = b - a;
    } else {
      seg.x = a;
      seg.y = lay.track.y;
      seg.w = b - a;
      seg.h = lay.track.h;
    }
    seg.r = lay.track.r;
    seg.isFill = isFill;
    seg.roundStart = (a == start);
    seg.roundEnd = (b == end);
    out.push_back(seg);
  };

  for (const Run& run : runs) {
    if (handle == nullptr || cutB <= run.a || cutA >= run.b) {
      push(run.a, run.b, run.isFill);
      continue;
    }
    push(run.a, run.b < cutA ? run.b : cutA, run.isFill);
    push(run.a > cutB ? run.a : cutB, run.b, run.isFill);
  }
}

}  // namespace levelshape
