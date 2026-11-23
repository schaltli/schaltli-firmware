#pragma once
// A styled needle's swing on the device (device contract §2.15 «Motion»,
// designer lib/instrument-spring.ts): a damped spring from where the needle
// stands to where the value puts it, stopped past either end at the pins,
// coming to rest exactly on the value. Board-independent: a board asks
// update() which objects moved and redraws their boxes; the renderer asks
// positionOf() where a needle stands meanwhile.

#include <stdint.h>

#include <vector>

#include "ProjectTypes.h"

class InstrumentMotion {
 public:
  static constexpr double PIN = 0.025;

  struct Box {
    int16_t x, y, w, h;
  };

  // Where `id`'s needle stands, if this knows; false: draw it at its value.
  bool positionOf(const String& id, double* at) const {
    for (const Swing& s : swings_) {
      if (s.id == id) {
        *at = s.at;
        return true;
      }
    }
    return false;
  }

  // Forget every needle (a screen change, a project load): the next frame
  // draws each at its value, at rest.
  void reset() { swings_.clear(); }

  bool moving() const {
    for (const Swing& s : swings_)
      if (s.moving) return true;
    return false;
  }

  // Looks at every styled object on `objects` (top level), its value's
  // position from `targetOf`, and steps the ones swinging to `nowMs`. Fills
  // `out` with the boxes to draw again. A needle not seen before starts at
  // rest on its value; one whose object has no spring follows it at once.
  template <typename TargetOf>
  void update(const std::vector<ScreenObject>& objects, uint32_t nowMs, TargetOf targetOf, std::vector<Box>& out) {
    out.clear();
    for (const ScreenObject& o : objects) {
      if (!o.instrument.on) continue;
      const double target = targetOf(o);
      Swing* s = find(o.id);
      if (!s) {
        Swing fresh;
        fresh.id = o.id;
        fresh.at = target;
        fresh.target = target;
        swings_.push_back(fresh);
        continue;
      }
      if (!o.instrument.hasSpring) {
        if (s->at != target) {
          s->at = target;
          s->target = target;
          s->moving = false;
          out.push_back(boxOf(o));
        }
        continue;
      }
      if (target != s->target) {
        s->target = target;
        if (!s->moving) {
          s->moving = true;
          s->last = nowMs;
        }
      }
      if (!s->moving) continue;
      step(*s, nowMs, o.instrument.frequency, o.instrument.damping);
      out.push_back(boxOf(o));
    }
  }

 private:
  struct Swing {
    String id;
    double at = 0, velocity = 0, target = 0;
    uint32_t last = 0;
    bool moving = false;
  };
  std::vector<Swing> swings_;

  Swing* find(const String& id) {
    for (Swing& s : swings_)
      if (s.id == id) return &s;
    return nullptr;
  }

  static Box boxOf(const ScreenObject& o) {
    const int16_t side = o.width < o.height ? o.width : o.height;
    return o.instrument.gauge ? Box{(int16_t)o.x, (int16_t)o.y, side, side}
                              : Box{(int16_t)o.x, (int16_t)o.y, (int16_t)o.width, (int16_t)o.height};
  }

  // lib/instrument-spring.ts stepSpring(): four sub-steps, a frame longer
  // than 50 ms taken as 50 ms, the pins keeping 30 % of the speed reversed.
  static void step(Swing& s, uint32_t nowMs, float frequency, float damping) {
    double dt = (nowMs - s.last) / 1000.0;
    if (dt > 0.05) dt = 0.05;
    if (dt < 0) dt = 0;
    s.last = nowMs;
    const double w = 2 * 3.14159265358979 * frequency;
    for (int k = 0; k < 4; k++) {
      const double h = dt / 4;
      s.velocity += (w * w * (s.target - s.at) - 2 * damping * w * s.velocity) * h;
      s.at += s.velocity * h;
      if (s.at < -PIN) {
        s.at = -PIN;
        s.velocity = -s.velocity * 0.3;
      }
      if (s.at > 1 + PIN) {
        s.at = 1 + PIN;
        s.velocity = -s.velocity * 0.3;
      }
    }
    if ((s.target - s.at < 2e-4 && s.at - s.target < 2e-4) && (s.velocity < 2e-3 && s.velocity > -2e-3)) {
      s.at = s.target;
      s.velocity = 0;
      s.moving = false;
    }
  }
};
