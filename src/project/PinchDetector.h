#pragma once
#include <stdint.h>
#include <math.h>

// Pinch in and pinch out, from the touch samples of one contact (designer
// docs/2026-10-16-charts.md, decision 15). A proof of concept first: the
// question it answers is whether a pinch is recognised reliably on a GT911,
// not whether it can be followed smoothly - a gesture fires its action once,
// at the lift, so a pinch is one step of the time range.
//
// Kept free of Arduino so it is tested on the PC (test/test_pinch) and the
// other boards can take it as it is.
//
// What it decides from:
// - A contact is a pinch candidate from the first sample with two points on;
//   from then on it is never a tap or a swipe, whatever the fingers do -
//   including the one left when the other lifts first.
// - The two fingers' distance at that first sample against their distance at
//   the last sample that still had two. A pinch is a change of distance, not
//   of place: two fingers dragged side by side keep theirs, and are neither.
// - Both a ratio and an absolute travel: 60 px on the 800 px glass is a
//   deliberate move; a ratio alone would call two fingers 40 px apart that
//   twitch to 30 a pinch.
class PinchDetector
{
public:
  enum Result
  {
    None,      // not two fingers, or two that did not pinch
    PinchIn,   // drawn together
    PinchOut,  // drawn apart
  };

  static constexpr float IN_RATIO = 0.75f;
  static constexpr float OUT_RATIO = 1.33f;
  static constexpr int MIN_TRAVEL_PX = 60;

  // A finger came down: a new contact.
  void begin(uint32_t ms)
  {
    *this = PinchDetector();
    contactStartMs_ = ms;
  }

  // One sample of the contact: how many points the panel reported, and the
  // first two of them. Samples with fewer than two only matter for when the
  // contact ends, which the caller says with finish().
  void sample(uint8_t count, int x0, int y0, int x1, int y1, uint32_t ms)
  {
    if (count > maxPoints_) maxPoints_ = count;
    if (count < 2) return;
    const float d = distance(x0, y0, x1, y1);
    if (!multi_)
    {
      multi_ = true;
      startDist_ = d;
      secondFingerAfterMs_ = ms - contactStartMs_;
    }
    endDist_ = d;
    twoFingerSamples_++;
  }

  // Two fingers were on the glass at some point of this contact.
  bool multi() const { return multi_; }

  // The finger is gone: what the contact was.
  Result finish() const
  {
    if (!multi_ || twoFingerSamples_ < 2) return None;
    const float travel = endDist_ - startDist_;
    if (fabsf(travel) < MIN_TRAVEL_PX) return None;
    if (startDist_ <= 0) return travel > 0 ? PinchOut : None;
    const float ratio = endDist_ / startDist_;
    if (ratio <= IN_RATIO) return PinchIn;
    if (ratio >= OUT_RATIO) return PinchOut;
    return None;
  }

  static const char *name(Result r)
  {
    return r == PinchIn ? "pinch-in" : r == PinchOut ? "pinch-out" : "none";
  }

  // For the log: what the decision was made from.
  int startDistance() const { return (int)lroundf(startDist_); }
  int endDistance() const { return (int)lroundf(endDist_); }
  int twoFingerSamples() const { return twoFingerSamples_; }
  uint8_t maxPoints() const { return maxPoints_; }
  // How long after the first finger the second one was seen.
  uint32_t secondFingerAfterMs() const { return secondFingerAfterMs_; }

private:
  static float distance(int x0, int y0, int x1, int y1)
  {
    const float dx = (float)(x1 - x0), dy = (float)(y1 - y0);
    return sqrtf(dx * dx + dy * dy);
  }

  bool multi_ = false;
  float startDist_ = 0;
  float endDist_ = 0;
  int twoFingerSamples_ = 0;
  uint8_t maxPoints_ = 0;
  uint32_t contactStartMs_ = 0;
  uint32_t secondFingerAfterMs_ = 0;
};
