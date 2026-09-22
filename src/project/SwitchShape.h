#pragma once

// The shape and the colours of a Switch, as whole pixels.
//
// A line-for-line mirror of lib/switch-shape.ts in the designer repo. The two
// have to produce the same integers or conformance finds the difference, so
// this file is deliberately boring: the same names, the same order, the same
// arithmetic. When one changes, the other changes in the same commit.
//
// Two forms, from the object's existing `mode`:
//
//   "segmented" -> a connected button group (Material 3): one container with
//                  the states side by side in it, the chosen one as its own
//                  pill. Material has retired the segmented button for it.
//   "single"    -> a switch: a track with a knob standing at one of n
//                  positions, the state's icon on the knob and its label
//                  beside it. n is normally 2.
//
// Why it looks like this: docs/2026-09-20-switch-look.md in the designer repo.

#include <Arduino.h>

#include <vector>

#include "LevelShape.h"
#include "MaterialColors.h"
#include "ProjectTypes.h"

// The inset between the container and the buttons in it.
//
// Its comment used to read "Material's 2 dp between the buttons of a connected
// group", which is a different measurement this was not being used for - the
// buttons tiled edge to edge until the designer's 2026-09-21 change, and here
// until it was brought across. That one is switchshape::kButtonGap.
#define SWITCH_PAD 2

struct SwitchRect {
  int x = 0;
  int y = 0;
  int w = 0;
  int h = 0;
  int r = 0;
  // The radius of the right-hand end, where it differs from [r] - a button in a
  // connected group is round on the outside and barely rounded where it faces
  // its neighbour. Negative means both ends are [r], which is the designer's
  // absent `rRight`.
  int rRight = -1;

  bool valid() const { return w > 0 && h > 0; }
  int rightRadius() const { return rRight >= 0 ? rRight : r; }
};

// What a Switch is painted with. Everything follows from its one colour.
struct SwitchLook {
  String surface;         // the container, and the track of a switch that is off
  bool surfaceFramed = false;  // that surface is the background: draw an outline instead
  String chosen;          // the chosen state's pill, and the track of a switch that is on
  String onChosen;        // ink on it
  String onSurface;       // ink on the container
  String ring;            // "asked for, not confirmed"
};

// The track and the knob of the switch form, in the state it is in.
struct SwitchKnobLook {
  String track;
  String trackOutline;  // empty for none
  String knob;
  String onKnob;
};

// Where a state's icon and label go inside a rectangle.
struct SwitchContent {
  bool hasIcon = false;
  SwitchRect icon;
  int textX = 0;
  int baseline = 0;
  bool beside = true;
};

namespace switchshape {

// Icon to label, and track to label.
constexpr int kGap = 8;
// Material 3's connected button group: 2 between the buttons, and 8 at the
// corners where two of them face each other. Both are the spec's own numbers
// (m3.material.io/components/button-groups/specs) rather than anything chosen
// here. Mirrors SWITCH_BUTTON_GAP and SWITCH_INNER_CORNER in the designer's
// lib/switch-shape.ts.
constexpr int kButtonGap = 2;
constexpr int kInnerCorner = 8;
// The knob has three sizes since 2026-09-22, as sixteenths of the track's
// height: quiet when the switch is off, bigger when it is on, bigger still
// while a finger is on it. Material does the same. A knob that only changed
// colour read as a recoloured dot rather than something that had moved.
// Mirrors SWITCH_KNOB_*_16THS in the designer's lib/switch-shape.ts.
constexpr int kKnobQuiet16ths = 8;
constexpr int kKnobOn16ths = 12;
constexpr int kKnobPressed16ths = 14;

// The outline an off track carries, flat, in the author's own colour.
constexpr int kTrackOutline = 2;

// The form is the type since 2026-09-20: "switch" is the knob in a
// track, "button-group" the connected strip. It was a `mode` property
// beside them until then (docs/2026-09-20-control-split.md).
inline bool isKnobForm(const ScreenObject& obj) { return obj.type == "switch"; }

inline bool isTonal(const ScreenObject& obj) { return obj.properties.switchStyle == "tonal"; }

// The one colour the author sets. The palette's own default is the designer's
// side of this; a project always carries a colour once it has been placed.
inline String colorOf(const ScreenObject& obj) {
  String c = obj.properties.switchColor;
  c.trim();
  return c.isEmpty() ? String("#6750A4") : c;
}

// Whether this state counts as "switched on", which is what makes the knob form
// draw in colour. Renamed from showMarker on 2026-09-20; ProjectLoader reads
// both names into the same field.
inline bool stateIsOn(const SwitchState& state) { return state.showMarker; }

// The corner radius of a container or a button in it: a pill when it is clearly
// wider than tall, a rounded square otherwise - a segment as tall as it is wide
// would come out an oval, or a circle.
inline int corner(int w, int h) {
  if (w <= 0 || h <= 0) return 0;
  if (w >= (h * 3) / 2) return h / 2;
  const int third = (w < h ? w : h) / 3;
  return third < 28 ? third : 28;
}

inline SwitchLook look(const ScreenObject& obj, const String& background) {
  SwitchLook out;
  const String color = materialcolors::applyDepth(colorOf(obj));
  const String ground = materialcolors::applyDepth(background.isEmpty() ? String("#ffffff") : background);
  // The same tint the slider's empty track and a tonal button take, so the
  // three read as one family.
  const LevelTrackLook tint = levelshape::trackLook(color, ground);
  const String quiet = tint.framed ? color : tint.track;
  out.surface = materialcolors::applyDepth(materialcolors::blend(ground, color, 25));
  const materialcolors::Rgb s = materialcolors::channels(out.surface);
  const materialcolors::Rgb g = materialcolors::channels(ground);
  out.surfaceFramed = s.valid && g.valid && s.r == g.r && s.g == g.g && s.b == g.b;
  out.chosen = isTonal(obj) ? quiet : color;
  out.onChosen = materialcolors::onColorFor(out.chosen);
  out.onSurface = materialcolors::onColorFor(out.surface);
  out.ring = color;
  return out;
}

inline SwitchKnobLook knobLook(const ScreenObject& obj, const String& background, bool on) {
  const SwitchLook base = look(obj, background);
  const String color = materialcolors::applyDepth(colorOf(obj));
  const String ground = materialcolors::applyDepth(background.isEmpty() ? String("#ffffff") : background);
  const LevelTrackLook tint = levelshape::trackLook(color, ground);
  // Where the tint lands back on the background - every 1-bit panel - the
  // quiet colour IS the background and the outline carries the shape. Note
  // this differs from look()'s own `quiet`, which falls back to the colour.
  const String quiet = tint.framed ? ground : tint.track;
  SwitchKnobLook out;
  if (on) {
    out.track = base.chosen;
    out.trackOutline = "";
    out.knob = base.onChosen;
    out.onKnob = base.chosen;
    return out;
  }
  // Off, since 2026-09-22: the track is the half-strength tint, and the
  // outline and the knob carry the author's colour at FULL strength. The
  // older reading - a pale 25 % track with everything else in the tint - made
  // an off switch look switched off *and* disabled.
  out.track = quiet;
  out.trackOutline = color;
  out.knob = color;
  out.onKnob = materialcolors::onColorFor(color);
  return out;
}

// The container of a connected button group: the object's own rectangle.
inline SwitchRect container(const ScreenObject& obj) {
  SwitchRect box;
  box.x = obj.x;
  box.y = obj.y;
  box.w = obj.width > 0 ? obj.width : 0;
  box.h = obj.height > 0 ? obj.height : 0;
  box.r = corner(box.w, box.h);
  return box;
}

// The buttons in it, tiling it exactly: each starts where the last one ended.
//
// Material 3's connected button group, which is what this is: the buttons stand
// kButtonGap apart, the ends of the group are fully round, and where two
// buttons face each other the corner is small. Selecting one changes nothing
// about its neighbours. A lone button is round at both ends.
//
// Each button deciding its own radius from its own box - corner(seg.w, innerH),
// which is what stood here - was the fault reported to the designer on
// 2026-09-21: a container 92 wide and 48 tall is a pill (radius 24) while each
// of its two buttons is about 43 by 43 and so, by the same rule, a rounded
// square (radius 14) - a rectangle sitting inside a pill, visibly squashed.
// Handing the button the container's radius instead would be worse: at 43 by 43
// it clamps to a circle. It is the small inner corner that makes the round
// outer one safe.
inline void segments(const ScreenObject& obj, int count, std::vector<SwitchRect>& out) {
  out.clear();
  const SwitchRect box = container(obj);
  const int innerX = box.x + SWITCH_PAD;
  const int innerY = box.y + SWITCH_PAD;
  const int innerW = box.w - 2 * SWITCH_PAD > 0 ? box.w - 2 * SWITCH_PAD : 0;
  const int innerH = box.h - 2 * SWITCH_PAD > 0 ? box.h - 2 * SWITCH_PAD : 0;
  const int n = count > 0 ? count : 1;
  const int outer = box.r - SWITCH_PAD > 0 ? box.r - SWITCH_PAD : 0;
  const int half = innerH / 2;
  const int inner = kInnerCorner < half ? kInnerCorner : half;
  for (int i = 0; i < n; i++) {
    const int from = innerX + (innerW * i) / n + (i == 0 ? 0 : kButtonGap);
    const int to = innerX + (innerW * (i + 1)) / n;
    SwitchRect seg;
    seg.x = from;
    seg.y = innerY;
    seg.w = to - from > 0 ? to - from : 0;
    seg.h = innerH;
    seg.r = i == 0 ? outer : inner;
    seg.rRight = i == n - 1 ? outer : inner;
    out.push_back(seg);
  }
}

// Which segment a finger at `x` is on.
inline int segmentAt(const ScreenObject& obj, int count, int x) {
  std::vector<SwitchRect> segs;
  segments(obj, count, segs);
  for (size_t i = 0; i < segs.size(); i++) {
    if (x < segs[i].x + segs[i].w || i + 1 == segs.size()) return (int)i;
  }
  return 0;
}

// The track of the switch form: two thirds of the object's height, at its left
// edge, one knob's width per state.
inline SwitchRect track(const ScreenObject& obj, int count) {
  const int h = obj.height > 0 ? obj.height : 0;
  int trackH = (h * 2) / 3;
  if (trackH < 6) trackH = 6;
  int pad = trackH / 8;
  if (pad < 1) pad = 1;
  int knob = trackH - 2 * pad;
  if (knob < 2) knob = 2;
  const int n = count > 0 ? count : 1;
  const int wanted = 2 * pad + knob + (n - 1) * knob;
  SwitchRect out;
  out.x = obj.x;
  out.y = obj.y + (h - trackH) / 2;
  out.w = wanted < obj.width ? wanted : (obj.width > 0 ? obj.width : 0);
  out.h = trackH;
  out.r = trackH / 2;
  return out;
}

struct SwitchKnob {
  int cx = 0;
  int cy = 0;
  int r = 0;
};

inline SwitchKnob knob(const ScreenObject& obj, int count, int index, bool pressed, bool on) {
  const SwitchRect t = track(obj, count);
  int pad = t.h / 8;
  if (pad < 1) pad = 1;
  // The slot's WIDTH is unchanged by state - a knob that grows must not also
  // move - so the step stays the old full-size knob.
  int step = t.h - 2 * pad;
  if (step < 2) step = 2;
  const int last = (count > 0 ? count : 1) - 1;
  int slot = index < 0 ? 0 : index;
  if (slot > last) slot = last;
  const int sixteenths = pressed ? kKnobPressed16ths : (on ? kKnobOn16ths : kKnobQuiet16ths);
  int d = t.h * sixteenths / 16;
  if (d < 2) d = 2;
  SwitchKnob out;
  out.cx = t.x + pad + slot * step + step / 2;
  out.cy = t.y + t.h / 2;
  out.r = d / 2;
  return out;
}

// Which slot a finger at `x` means; -1 where it is past the track, on the label.
inline int slotAt(const ScreenObject& obj, int count, int x) {
  const SwitchRect t = track(obj, count);
  if (x < t.x || x > t.x + t.w) return -1;
  int pad = t.h / 8;
  if (pad < 1) pad = 1;
  int knobD = t.h - 2 * pad;
  if (knobD < 2) knobD = 2;
  const int first = t.x + pad + knobD / 2;
  // Math.round's half-up on the designer's side; the same here, on a value
  // that is never negative in practice.
  const int slot = (int)((float)(x - first) / (float)knobD + 0.5f);
  const int last = (count > 0 ? count : 1) - 1;
  if (slot < 0) return 0;
  return slot > last ? last : slot;
}

// What is left of the object beside the track: where the state's label goes.
inline SwitchRect labelBox(const ScreenObject& obj, int count) {
  const SwitchRect t = track(obj, count);
  SwitchRect out;
  out.x = t.x + t.w + kGap;
  out.y = obj.y;
  const int right = obj.x + obj.width;
  out.w = right - out.x > 0 ? right - out.x : 0;
  out.h = obj.height > 0 ? obj.height : 0;
  out.r = 0;
  return out;
}

// Where a state's icon and label go inside a rectangle: side by side when it is
// wider than tall (a strip), the icon above the label when it is not (a tile).
inline SwitchContent content(const SwitchRect& rect, const LevelFontMetrics& m, bool hasIcon, int textWidth) {
  const int line = m.ascent + m.descent;
  const int iconSize = hasIcon ? (m.capHeight > 1 ? m.capHeight : 1) : 0;
  SwitchContent out;
  out.hasIcon = hasIcon;
  if (rect.w >= rect.h) {
    const int group = (hasIcon ? iconSize + kGap : 0) + textWidth;
    int left = rect.x + (rect.w - group) / 2;
    if (left < rect.x) left = rect.x;
    out.baseline = rect.y + (rect.h - line) / 2 + m.ascent;
    if (hasIcon) {
      out.icon.x = left;
      out.icon.y = out.baseline - iconSize;
      out.icon.w = iconSize;
      out.icon.h = iconSize;
    }
    out.textX = hasIcon ? left + iconSize + kGap : left;
    out.beside = true;
    return out;
  }
  const int block = (hasIcon ? iconSize + 6 : 0) + line;
  int top = rect.y + (rect.h - block) / 2;
  if (top < rect.y) top = rect.y;
  out.baseline = top + (hasIcon ? iconSize + 6 : 0) + m.ascent;
  if (hasIcon) {
    out.icon.x = rect.x + (rect.w - iconSize) / 2;
    out.icon.y = top;
    out.icon.w = iconSize;
    out.icon.h = iconSize;
  }
  out.textX = rect.x + (rect.w - textWidth) / 2;
  if (out.textX < rect.x) out.textX = rect.x;
  out.beside = false;
  return out;
}

// Which state a tap selects. In the switch form with two states a tap anywhere
// toggles; with more, a tap on the track picks the slot under it and a tap
// beside the track advances by one, so a finger on the label still does
// something predictable.
inline int stateIndexForTap(const ScreenObject& obj, int x, int activeIndex) {
  const int count = (int)obj.properties.states.size();
  if (count <= 0) return -1;
  if (isKnobForm(obj)) {
    if (count == 2) return activeIndex == 0 ? 1 : 0;
    const int slot = slotAt(obj, count, x);
    if (slot >= 0) return slot;
    return activeIndex < 0 ? 0 : (activeIndex + 1) % count;
  }
  return segmentAt(obj, count, x);
}

}  // namespace switchshape
