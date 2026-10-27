#pragma once
// The picker a tapped time field opens (designer tasks/ha-time-plan.md,
// decision 4). One piece of code for every board: where its parts lie, what
// a finger does to it, how a wheel moves; drawing and touch are the boards'
// (ColorScreenRenderer::renderTimePicker, each main.cpp).
//
// The 4.3B and the Knob: two wheels, hours and minutes, that follow the
// finger continuously and snap on release, the way a swipe between screens
// settles - a quick flick runs on a few steps; a tap on a neighbour brings
// it to the middle. The PaperS3: a button above and below each, as a wheel
// that follows a finger would cost a refresh per step on e-paper.
//
// Pure C++, tested on the PC (test/test_time_picker).

#include <cstdint>
#include <string>

#include "TimeField.h"

namespace timepicker {

enum class Style { Wheels, Buttons };

struct Rect {
  int x = 0, y = 0, w = 0, h = 0;
  bool contains(int px, int py) const { return px >= x && py >= y && px < x + w && py < y + h; }
};

// What it covers and where its parts lie. `hour` and `minute` are the wheels
// (Wheels) or the value boxes between their buttons (Buttons).
struct Frame {
  Style style = Style::Wheels;
  Rect panel;
  Rect hour, minute;
  Rect hourUp, hourDown, minuteUp, minuteDown;  // Buttons only
  Rect ok, cancel;
  int itemH = 0;   // one value on a wheel
  int colonX = 0;  // the colon's middle
  int colonY = 0;
};

// A rectangular screen: the picker over its lower part, as the keyboard;
// a round one: the whole screen, the wheels in the middle, OK and Cancel
// below them inside the bezel.
Frame frameFor(int width, int height, bool round, Style style);

// One wheel. The value in the middle is `index` (of `count`); while a finger
// moves it or it settles, everything is drawn `offset` px further down.
struct Wheel {
  int index = 0;
  int count = 24;
  float offset = 0;
  bool settling = false;
  float from = 0;
  uint32_t startMs = 0;
};

struct State {
  int step = timefield::DEFAULT_STEP;
  Wheel hour;
  Wheel minute;
};

// Opened on what the field shows, put on the step; no time: 00:00.
State open(const timefield::TimeOfDay* shown, int minuteStep);

// The time in the middle of the wheels.
timefield::TimeOfDay value(const State& state);

// A finger moved the wheel `dy` px (down positive): the values follow it,
// the middle one changes on every half item.
void drag(Wheel& wheel, float dy, int itemH);

// The finger let go at `velocity` px/ms: a flick runs on a few values, then
// it settles on the one it stopped at.
void release(Wheel& wheel, float velocity, int itemH, uint32_t nowMs);

// `k` values on (a tap on a neighbour, a button): that value comes to the
// middle from where it was drawn, settling.
void nudge(Wheel& wheel, int k, int itemH, uint32_t nowMs);

// The next frame of a settling wheel; false once it has come to rest.
constexpr uint32_t SETTLE_MS = 220;
bool animate(Wheel& wheel, uint32_t nowMs);

enum class Part { None, Hour, Minute, HourUp, HourDown, MinuteUp, MinuteDown, Ok, Cancel };

Part partAt(const Frame& frame, int x, int y);

// Which value a tap at `y` on a wheel means, relative to the middle one.
int neighbourAt(const Rect& wheel, int itemH, int y);

// The open picker as JSON, for a test: its value, style, item height and
// each part's rectangle.
std::string describe(const State& state, const Frame& frame);

}  // namespace timepicker
