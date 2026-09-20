#pragma once

#include <Arduino.h>

#include "ProjectTypes.h"

// The sixteen object types, mirroring lib/object-types.ts in the designer
// repo (docs/2026-09-20-control-split.md).
//
// One type is one component. What used to be one object with two faces - a
// Level Indicator a finger could or could not set, a Switch that was a knob
// or a group - is two types, and this board declares in its device.json
// which of them it draws. A type it does not declare is not offered in the
// designer's toolbar at all, and this renderer skips it, the same way it has
// always skipped a type it does not know.
//
// Nothing here accepts the names from before 2026-09-20: the designer
// migrates a project on the way in and never writes them out again.

namespace objecttypes {

// The straight level - to read, or to set with a finger. Same shape.
inline bool isLevel(const String& type) { return type == "bar" || type == "slider"; }

// The round one.
inline bool isArc(const String& type) { return type == "gauge" || type == "dial"; }

// A knob in a track, or a connected button group. The form is the type;
// there is no `mode` property any more.
inline bool isSwitch(const String& type) { return type == "switch" || type == "button-group"; }

// Either shape of level, straight or round.
inline bool isAnyLevel(const String& type) { return isLevel(type) || isArc(type); }

// The two a finger sets: they carry a writeTopic and a step, and they are
// what a hit test has to catch. A bar or a gauge never catches a finger - a
// tank you only read should not swallow a swipe meant to turn the page.
inline bool isSettable(const String& type) { return type == "slider" || type == "dial"; }

inline bool isSettable(const ScreenObject& obj) { return isSettable(obj.type); }

// A line, plain or value-driven.
inline bool isLine(const String& type) { return type == "line" || type == "live-line"; }

}  // namespace objecttypes
