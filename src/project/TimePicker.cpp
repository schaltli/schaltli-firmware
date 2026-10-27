#include "TimePicker.h"

#include <algorithm>
#include <cmath>

namespace timepicker {

namespace {
int mod(int x, int n) { return ((x % n) + n) % n; }
int rnd(double v) { return static_cast<int>(std::lround(v)); }
}  // namespace

Frame frameFor(int width, int height, bool round, Style style) {
  Frame f;
  f.style = style;
  if (round) {
    // At 360, as tried on 2026-10-27: wheels of 100 x 170 from 76 down, a
    // colon of 24 between, OK and Cancel 104 x 46 at 260 - inside the bezel.
    auto s = [width](int v) { return rnd(v * width / 360.0); };
    f.panel = Rect{0, 0, width, height};
    const int wheelW = s(100), wheelH = s(170), colon = s(24);
    const int left = (width - (2 * wheelW + colon)) / 2;
    f.hour = Rect{left, s(76), wheelW, wheelH};
    f.minute = Rect{left + wheelW + colon, s(76), wheelW, wheelH};
    f.itemH = s(50);
    f.colonX = left + wheelW + colon / 2;
    f.colonY = f.hour.y + wheelH / 2;
    const int bw = s(104), bh = s(46), gap = s(12);
    const int bx = (width - (2 * bw + gap)) / 2;
    f.cancel = Rect{bx, s(260), bw, bh};
    f.ok = Rect{bx + bw + gap, s(260), bw, bh};
    return f;
  }
  const int panelY = rnd(height * 0.4);
  const int panelH = height - panelY;
  f.panel = Rect{0, panelY, width, panelH};
  const int margin = rnd(panelH * 0.07);
  const int colW = rnd(width * (style == Style::Wheels ? 0.15 : 0.14));
  const int colon = rnd(colW * 0.27);
  const int buttonW = rnd(width * (style == Style::Wheels ? 0.2 : 0.19));
  const int buttonH = rnd(panelH * 0.22);
  const int groupW = 2 * colW + colon;
  const int between = rnd(width * 0.05);
  const int left = (width - (groupW + between + buttonW)) / 2;
  const int midY = panelY + panelH / 2;
  if (style == Style::Wheels) {
    const int wheelH = panelH - 2 * margin;
    f.hour = Rect{left, panelY + margin, colW, wheelH};
    f.minute = Rect{left + colW + colon, panelY + margin, colW, wheelH};
    f.itemH = rnd(wheelH / 4.5);
  } else {
    const int valueH = rnd(panelH * 0.29);
    const int gap = rnd(panelH * 0.03);
    const int top = midY - valueH / 2;
    f.hour = Rect{left, top, colW, valueH};
    f.minute = Rect{left + colW + colon, top, colW, valueH};
    f.hourUp = Rect{left, top - gap - buttonH, colW, buttonH};
    f.hourDown = Rect{left, top + valueH + gap, colW, buttonH};
    f.minuteUp = Rect{f.minute.x, f.hourUp.y, colW, buttonH};
    f.minuteDown = Rect{f.minute.x, f.hourDown.y, colW, buttonH};
    f.itemH = valueH;
  }
  f.colonX = left + colW + colon / 2;
  f.colonY = midY;
  const int bx = left + groupW + between;
  const int gap = rnd(panelH * 0.05);
  f.cancel = Rect{bx, midY - gap / 2 - buttonH, buttonW, buttonH};
  f.ok = Rect{bx, midY + (gap + 1) / 2, buttonW, buttonH};
  return f;
}

State open(const timefield::TimeOfDay* shown, int minuteStep) {
  State s;
  s.step = timefield::step(minuteStep);
  const timefield::TimeOfDay t = timefield::onStep(shown ? *shown : timefield::TimeOfDay{}, s.step);
  s.hour.count = 24;
  s.hour.index = t.hour;
  s.minute.count = 60 / s.step;
  s.minute.index = t.minute / s.step;
  return s;
}

timefield::TimeOfDay value(const State& state) {
  timefield::TimeOfDay t;
  t.hour = mod(state.hour.index, 24);
  t.minute = mod(state.minute.index, state.minute.count) * state.step;
  return t;
}

void drag(Wheel& wheel, float dy, int itemH) {
  wheel.settling = false;
  wheel.offset += dy;
  const float half = itemH / 2.0f;
  while (wheel.offset > half) {
    wheel.index = mod(wheel.index - 1, wheel.count);
    wheel.offset -= itemH;
  }
  while (wheel.offset < -half) {
    wheel.index = mod(wheel.index + 1, wheel.count);
    wheel.offset += itemH;
  }
}

void release(Wheel& wheel, float velocity, int itemH, uint32_t nowMs) {
  // A flick: as far as 160 ms at its speed would carry it, at most eight.
  const int extra = std::max(-8, std::min(8, rnd(velocity * 160.0f / itemH)));
  wheel.index = mod(wheel.index - extra, wheel.count);
  wheel.offset += extra * itemH;
  wheel.settling = true;
  wheel.from = wheel.offset;
  wheel.startMs = nowMs;
}

void nudge(Wheel& wheel, int k, int itemH, uint32_t nowMs) {
  wheel.index = mod(wheel.index + k, wheel.count);
  wheel.offset += k * itemH;
  wheel.settling = true;
  wheel.from = wheel.offset;
  wheel.startMs = nowMs;
}

bool animate(Wheel& wheel, uint32_t nowMs) {
  if (!wheel.settling) return false;
  const uint32_t t = nowMs - wheel.startMs;
  if (t >= SETTLE_MS) {
    wheel.offset = 0;
    wheel.settling = false;
    return true;
  }
  // Ease-out cubic: fast first, gently onto the value.
  const float p = 1.0f - static_cast<float>(t) / SETTLE_MS;
  wheel.offset = wheel.from * p * p * p;
  return true;
}

Part partAt(const Frame& f, int x, int y) {
  if (f.ok.contains(x, y)) return Part::Ok;
  if (f.cancel.contains(x, y)) return Part::Cancel;
  if (f.style == Style::Buttons) {
    if (f.hourUp.contains(x, y)) return Part::HourUp;
    if (f.hourDown.contains(x, y)) return Part::HourDown;
    if (f.minuteUp.contains(x, y)) return Part::MinuteUp;
    if (f.minuteDown.contains(x, y)) return Part::MinuteDown;
  }
  if (f.hour.contains(x, y)) return Part::Hour;
  if (f.minute.contains(x, y)) return Part::Minute;
  return Part::None;
}

int neighbourAt(const Rect& wheel, int itemH, int y) {
  const double d = (y - (wheel.y + wheel.h / 2.0)) / itemH;
  return static_cast<int>(std::lround(d));
}

namespace {
std::string rect(const Rect& r) {
  return "{\"x\":" + std::to_string(r.x) + ",\"y\":" + std::to_string(r.y) + ",\"w\":" + std::to_string(r.w) +
         ",\"h\":" + std::to_string(r.h) + "}";
}
}  // namespace

std::string describe(const State& state, const Frame& f) {
  const bool settling = state.hour.settling || state.minute.settling || state.hour.offset != 0 || state.minute.offset != 0;
  std::string out = "{\"open\":true,\"value\":\"" + timefield::format(value(state)) + "\",\"step\":" + std::to_string(state.step) +
                    ",\"style\":\"" + (f.style == Style::Wheels ? "wheels" : "buttons") + "\",\"settling\":" +
                    (settling ? "true" : "false") + ",\"itemH\":" + std::to_string(f.itemH) + ",\"hour\":" + rect(f.hour) +
                    ",\"minute\":" + rect(f.minute) + ",\"ok\":" + rect(f.ok) + ",\"cancel\":" + rect(f.cancel);
  if (f.style == Style::Buttons) {
    out += ",\"hourUp\":" + rect(f.hourUp) + ",\"hourDown\":" + rect(f.hourDown) + ",\"minuteUp\":" + rect(f.minuteUp) +
           ",\"minuteDown\":" + rect(f.minuteDown);
  }
  return out + "}";
}

}  // namespace timepicker
