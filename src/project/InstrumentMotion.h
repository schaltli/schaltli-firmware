#pragma once
// A styled needle's swing on the device (device contract §2.15 «Motion»,
// designer lib/instrument-spring.ts): a damped spring from where the needle
// stands to where the value puts it, stopped past either end at the pins,
// coming to rest exactly on the value. Board-independent: a board asks
// update() which objects moved and redraws their boxes; the renderer asks
// positionOf() where a needle stands meanwhile.

#include <stdint.h>

#include <math.h>

#include <vector>

#include "ProjectTypes.h"

// The needle's pose for a position 0..1 (device contract §2.15): a gauge's
// angle, floor(deg x 64 + 0.5); a bar's shift, the designer's Math.round.
inline needleraster::Pose instrumentPose(const InstrumentGeom& g, double at) {
  needleraster::Pose pose;
  if (g.gauge) {
    pose.angle64 = needleraster::angle64Of(g.sweepFrom + (g.sweepTo - g.sweepFrom) * at);
  } else {
    pose.shiftX = (int32_t)floor(at * g.travelX + 0.5);
    pose.shiftY = (int32_t)floor(at * g.travelY + 0.5);
  }
  // Segments light by the position itself, gauge and bar alike.
  pose.level = needleraster::levelOf(at);
  return pose;
}

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

  // Counts a frame drawn; notes when a swing starts and ends, so a board can
  // say what its last swing cost (frames, milliseconds) without anyone
  // polling it meanwhile.
  void frameDrawn(uint32_t nowMs) {
    if (!inSwing_) {
      inSwing_ = true;
      swingStartMs_ = nowMs;
      swingFrames_ = 0;
    }
    swingFrames_++;
    swingEndMs_ = nowMs;
  }
  void settleCheck() {
    if (inSwing_ && !moving()) {
      inSwing_ = false;
      lastSwingFrames_ = swingFrames_;
      lastSwingMs_ = swingEndMs_ - swingStartMs_;
    }
  }
  uint32_t lastSwingFrames() const { return lastSwingFrames_; }
  uint32_t lastSwingMs() const { return lastSwingMs_; }

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
      // Only where the needle was and where it is now: the whole face
      // costs a 400 px gauge five times the frame (the spike; I9's first
      // run on the 4.3B drew one frame a second that way).
      int32_t ax0, ay0, ax1, ay1, bx0, by0, bx1, by1;
      const bool hadBox = needleraster::bounds(o.instrument.needle, instrumentPose(o.instrument, s->at), &ax0, &ay0, &ax1, &ay1);
      step(*s, nowMs, o.instrument.frequency, o.instrument.damping);
      const bool hasBox = needleraster::bounds(o.instrument.needle, instrumentPose(o.instrument, s->at), &bx0, &by0, &bx1, &by1);
      if (!hadBox || !hasBox) {
        out.push_back(boxOf(o));
        continue;
      }
      const Box face = boxOf(o);
      int32_t x0 = (ax0 < bx0 ? ax0 : bx0), y0 = (ay0 < by0 ? ay0 : by0);
      int32_t x1 = (ax1 > bx1 ? ax1 : bx1), y1 = (ay1 > by1 ? ay1 : by1);
      if (x0 < 0) x0 = 0;
      if (y0 < 0) y0 = 0;
      if (x1 > face.w - 1) x1 = face.w - 1;
      if (y1 > face.h - 1) y1 = face.h - 1;
      if (x1 < x0 || y1 < y0) continue;
      out.push_back(Box{(int16_t)(face.x + x0), (int16_t)(face.y + y0), (int16_t)(x1 - x0 + 1), (int16_t)(y1 - y0 + 1)});
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
  bool inSwing_ = false;
  uint32_t swingStartMs_ = 0, swingEndMs_ = 0, swingFrames_ = 0;
  uint32_t lastSwingFrames_ = 0, lastSwingMs_ = 0;

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
