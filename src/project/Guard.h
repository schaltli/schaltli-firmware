#pragma once

// Guard: a control that needs a deliberate gesture before it changes
// (schaltli-designer docs/2026-11-15-guard.md, device contract §2.14).
//
// The designer's lib/guard.ts, line for line where pixels or times are
// concerned: the lock and the double arrow as integer runs (the lock from the
// same round-rectangle spans Adafruit GFX fills), the stripes, and the gesture
// as a small state machine fed with millis() and finger positions. The
// reference runs are lib/fixtures/guard-spans.json; test/test_guard holds this
// file to them. Pure C++, no Arduino, so it runs natively too.

#include <stdint.h>
#include <math.h>
#include <stdlib.h>
#include <vector>
#include <utility>

namespace guard {

// Times and thresholds: the same on every device.
constexpr float UNLOCK_FRACTION = 0.45f;
constexpr uint32_t ANIM_MS = 200;
constexpr uint32_t RELOCK_MS = 5000;
constexpr uint32_t TOPUP_MS = 2000;
constexpr uint32_t HINT_MS = 1500;
constexpr int TAP_SLOP_PX = 8;
constexpr float WAIT_ALPHA = 0.55f;
constexpr int BOB_PX = 5;
constexpr uint32_t BOB_MS = 1000;
constexpr int STRIPE_PX = 8;
constexpr int LOCK_MIN = 8;
constexpr const char* HINT = "Slide up to unlock";

// The fixed colours, RGB888 (decision 14).
constexpr uint32_t YELLOW = 0xf5c542;
constexpr uint32_t YELLOW_2 = 0xebb526;
constexpr uint32_t INK = 0x3a2d00;

inline bool stripeIsSecond(int x, int y) {
  const int k = (x + y) >= 0 ? (x + y) / STRIPE_PX : ((x + y) - STRIPE_PX + 1) / STRIPE_PX;
  return (k & 1) == 1;
}

using Span = std::pair<int16_t, int16_t>;  // [start, end)
using Runs = std::vector<std::vector<Span>>;

// Adafruit_GFX::fillRoundRect's rows: lib/clip.ts roundRectSpans.
inline std::vector<Span> roundRectSpans(int w, int h, int r) {
  std::vector<Span> out;
  if (w <= 0 || h <= 0) return out;
  const int maxRadius = (w < h ? w : h) / 2;
  if (r > maxRadius) r = maxRadius;
  if (r < 0) r = 0;
  std::vector<int> lo(h, w), hi(h, -1);
  auto column = [&](int x, int y, int height) {
    for (int row = y < 0 ? 0 : y; row < (y + height < h ? y + height : h); row++) {
      if (x < lo[row]) lo[row] = x;
      if (x > hi[row]) hi[row] = x;
    }
  };
  if (w - 2 * r > 0) {
    column(r, 0, h);
    column(w - r - 1, 0, h);
  }
  auto helper = [&](int x0, int y0, int corners, int deltaIn) {
    int f = 1 - r, ddFx = 1, ddFy = -2 * r, x = 0, y = r, px = x, py = y;
    const int delta = deltaIn + 1;
    while (x < y) {
      if (f >= 0) {
        y--;
        ddFy += 2;
        f += ddFy;
      }
      x++;
      ddFx += 2;
      f += ddFx;
      if (x < y + 1) {
        if (corners & 1) column(x0 + x, y0 - y, 2 * y + delta);
        if (corners & 2) column(x0 - x, y0 - y, 2 * y + delta);
      }
      if (y != py) {
        if (corners & 1) column(x0 + py, y0 - px, 2 * px + delta);
        if (corners & 2) column(x0 - py, y0 - px, 2 * px + delta);
        py = y;
      }
      px = x;
    }
  };
  if (r > 0) {
    helper(w - r - 1, r, 1, h - 2 * r - 1);
    helper(r, r, 2, h - 2 * r - 1);
  }
  out.resize(h);
  for (int row = 0; row < h; row++) out[row] = hi[row] < 0 ? Span{0, 0} : Span{(int16_t)lo[row], (int16_t)(hi[row] + 1)};
  return out;
}

template <typename Inside>
inline Runs rowsOf(int w, int h, Inside inside) {
  Runs rows(h);
  for (int y = 0; y < h; y++) {
    int start = -1;
    for (int x = 0; x <= w; x++) {
      const bool on = x < w && inside(x, y);
      if (on && start < 0) start = x;
      if (!on && start >= 0) {
        rows[y].push_back({(int16_t)start, (int16_t)x});
        start = -1;
      }
    }
  }
  return rows;
}

inline bool inSpan(const std::vector<Span>& s, int x, int y) {
  return y >= 0 && y < (int)s.size() && x >= s[y].first && x < s[y].second;
}

// The lock, size x size (lib/guard.ts guardLockRuns).
inline Runs lockRuns(int size) {
  const int s = size < 1 ? 1 : size;
  const int bodyTop = (s * 9) / 20;
  const int bodyR = s / 8 > 1 ? s / 8 : 1;
  const auto body = roundRectSpans(s, s - bodyTop, bodyR);
  const int a = s / 5;
  const int t = s / 7 > 1 ? s / 7 : 1;
  const int ow = s - 2 * a;
  const auto outer = roundRectSpans(ow, 2 * bodyTop, ow / 2);
  const int iw = ow - 2 * t;
  const int ih = 2 * bodyTop - 2 * t;
  const auto inner = iw > 0 ? roundRectSpans(iw, ih > 0 ? ih : 0, iw / 2) : std::vector<Span>();
  return rowsOf(s, s, [&](int x, int y) {
    if (y >= bodyTop) return inSpan(body, x, y - bodyTop);
    if (!inSpan(outer, x - a, y)) return false;
    return !(y >= t && inSpan(inner, x - a - t, y - t));
  });
}

// The double arrow pointing up, size x size (lib/guard.ts guardArrowRuns).
inline Runs arrowRuns(int size) {
  const int s = size < 1 ? 1 : size;
  const int half = (s - 1) / 2;
  const int c = half;
  const int t = s / 7 > 1 ? s / 7 : 1;
  const int chevronRows = (half - 2 + 2 * t) / 2 + 1;
  const int gap = chevronRows + 1;
  const int topRaw = (s - (gap + chevronRows)) / 2;
  const int top = topRaw > 0 ? topRaw : 0;
  auto chevron = [&](int x, int y) {
    if (y < 0 || y >= chevronRows) return false;
    const int d = abs(x - c);
    const int low = 2 * y - 2 * t + 2;
    return d <= half && d >= (low > 0 ? low : 0) && d <= 2 * y + 1;
  };
  return rowsOf(s, s, [&](int x, int y) { return chevron(x, y - top) || chevron(x, y - top - gap); });
}

// Where the cover's lock and arrows stand in it (lib/guard.ts guardCoverMarks).
struct Marks {
  int size, lockX, lockY, arrowsX, arrowsY;
};
inline Marks coverMarks(int x, int y, int w, int h) {
  int size = (h * 9) / 20;
  if (size > 28) size = 28;
  if (size < LOCK_MIN) size = LOCK_MIN;
  const int gap = size / 4 > 3 ? size / 4 : 3;
  const int total = 2 * size + gap;
  const int mx = x + (w - total) / 2;
  const int my = y + (h - size) / 2;
  return {size, mx, my, mx + size + gap, my};
}

// The lock on a Switch's knob (lib/guard.ts guardKnobLock).
struct LockBox {
  int x, y, size;
};
inline LockBox knobLock(int cx, int cy, int r) {
  int size = (r * 2 * 2) / 5;
  if (size < LOCK_MIN) size = LOCK_MIN;
  return {cx - size / 2, cy - size / 2, size};
}

inline int waitOffset(int height) { return height + 2; }

// --- The gesture (lib/guard.ts guardDown/Move/Up/Tick/Changed)

enum class Phase : uint8_t { None, Pushing, Opening, Open, Dropping };

struct Cover {
  Phase phase = Phase::None;
  uint32_t since = 0;
  int offset = 0;
  uint32_t until = 0;
  int startX = 0, startY = 0;
  float moved = 0;
  bool droppedFromWait = false;

  bool showing() const { return phase != Phase::None; }
  bool unlocked() const { return phase == Phase::Opening || phase == Phase::Open; }

  void down(uint32_t now, int x, int y) {
    phase = Phase::Pushing;
    since = now;
    offset = 0;
    until = 0;
    startX = x;
    startY = y;
    moved = 0;
    droppedFromWait = false;
  }

  // Returns true when this move locked it in.
  bool move(uint32_t now, int x, int y, int travel) {
    if (phase != Phase::Pushing) return false;
    const float dx = (float)(x - startX), dy = (float)(y - startY);
    const float d = sqrtf(dx * dx + dy * dy);
    if (d > moved) moved = d;
    offset = startY - y > 0 ? startY - y : 0;
    if (offset > travel) {
      phase = Phase::Opening;
      since = now;
      until = now + RELOCK_MS;
      return true;
    }
    return false;
  }

  // Returns true for a plain tap: the hint shows.
  bool up(uint32_t now) {
    if (phase != Phase::Pushing) return false;
    phase = Phase::Dropping;
    since = now;
    droppedFromWait = false;
    return moved < TAP_SLOP_PX;
  }

  // Time passing; returns true when anything changed.
  bool tick(uint32_t now) {
    if (phase == Phase::Opening && now - since >= ANIM_MS) {
      phase = Phase::Open;
      since += ANIM_MS;
      return true;
    }
    if ((phase == Phase::Open || phase == Phase::Opening) && (int32_t)(now - until) >= 0) {
      phase = Phase::Dropping;
      since = now;
      droppedFromWait = true;
      return true;
    }
    if (phase == Phase::Dropping && now - since >= ANIM_MS) {
      phase = Phase::None;
      return true;
    }
    return false;
  }

  void changed(uint32_t now) {
    if (!unlocked()) return;
    if ((int32_t)(now + TOPUP_MS - until) > 0) until = now + TOPUP_MS;
  }

  void reset() { phase = Phase::None; }
};

struct Drawn {
  int lift;
  float alpha;
  int bob;
};

inline float easeOutBack(float t) {
  const float s = 1.2f, u = t - 1;
  return 1 + (s + 1) * u * u * u + s * u * u;
}

inline Drawn drawn(const Cover& c, uint32_t now, int height, bool motion = true) {
  const int wait = waitOffset(height);
  const int bob = motion ? (int)lroundf(BOB_PX * (1 - cosf(2 * 3.14159265f * (float)(now - c.since) / BOB_MS)) / 2) : 0;
  switch (c.phase) {
    case Phase::Pushing:
      return {motion ? (c.offset < wait ? c.offset : wait) : 0, 1.0f, bob};
    case Phase::Opening: {
      float t = (float)(now - c.since) / ANIM_MS;
      if (t > 1) t = 1;
      const int from = c.offset < wait ? c.offset : wait;
      const float k = motion ? easeOutBack(t) : 1;
      return {(int)lroundf(from + (wait - from) * k), 1 - (1 - WAIT_ALPHA) * t, 0};
    }
    case Phase::Open:
      return {wait, WAIT_ALPHA, 0};
    case Phase::Dropping: {
      if (!motion) return {0, 0, 0};
      float t = (float)(now - c.since) / ANIM_MS;
      if (t > 1) t = 1;
      const int from = c.droppedFromWait ? wait : (c.offset < wait ? c.offset : wait);
      const float start = c.droppedFromWait ? WAIT_ALPHA : 1.0f;
      return {(int)lroundf(from * (1 - t)), start * (1 - t), 0};
    }
    default:
      return {0, 0, 0};
  }
}

}  // namespace guard
