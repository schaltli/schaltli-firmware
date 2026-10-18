#pragma once
#include "project/NavigatorLayout.h"
#include <Arduino.h>
#include "../../ClippedCanvas16.h"
#include "RgbPanel.h"
#include "../../project/ColorScreenRenderer.h"

// A screen transition that follows the finger, on the board whose numbers say
// it cannot be smooth.
//
// Why it is here again (2026-09-09). This was built once, measured at 72ms a
// frame, and deleted with "the bus is full" written into the header. That
// reasoning was wrong and has since been corrected: the panel's write floor
// is 25ms, not 43. The arithmetic that kills a full-screen follow survived
// the correction anyway, because it is structural rather than a missing
// optimisation - every frame reads 750KB out of PSRAM and writes 750KB into
// the framebuffer, and the two directions interleaved cost 66ms where the
// same bytes in one direction cost 25.
//
//   compose  65.2 ms  15.3 fps  (PanelBenchmark.h, measured on this panel)
//
// So why build it. Because the earlier decision rested on a version nobody
// ever saw. Two variants existed: this one at 15fps, and a half-row one that
// halved the traffic and reached 28fps by throwing away every second row
// during motion. The toggle between them was itself buggy - a corner tap
// registered twice and landed back on half rows - so the verdict "sieht nicht
// so toll aus" was almost certainly passed on the coarse one. The full one
// is cheap to rebuild and the only honest way to close the question is to put
// it on the glass. There is no toggle this time, and nothing to get wrong.
//
// What it deliberately does not do:
//
//   - No half rows, no reduced colour, no cleverness. Every frame is every
//     pixel, so what is judged is the frame rate and nothing else.
//   - No tearing avoidance. The panel scans the framebuffer continuously and
//     this writes straight into it, exactly as the normal present does.
//   - Nothing during the incoming screen's render. That blocks for as long as
//     a screen takes (113ms here), the panel cannot be sampled meanwhile, and
//     a fast flick can be over before the follow ever starts. The knob has
//     the same hole and the same note.
class FollowSwipe {
public:
  // What the last gesture cost, so the feel can be argued about with numbers
  // next to it rather than from memory.
  struct Stats {
    uint32_t frames = 0;
    uint32_t totalUs = 0;
    uint32_t worstUs = 0;
    uint32_t startRenderUs = 0;
    uint32_t durationMs = 0;
    int travel = 0;
    int pxPerSecond = 0;
    bool committed = false;
    bool flicked = false;
    bool valid = false;
  };

  void attach(RgbPanel *panel, ClippedCanvas16 **current, ClippedCanvas16 *spare,
              ColorScreenRenderer *renderer, int *currentScreen, int screenCount,
              int16_t w, int16_t h)
  {
    panel_ = panel;
    current_ = current;
    spare_ = spare;
    renderer_ = renderer;
    currentScreen_ = currentScreen;
    screenCount_ = screenCount;
    w_ = w;
    h_ = h;
  }

  bool ready() const { return panel_ && current_ && spare_ && renderer_ && screenCount_ >= 2; }
  bool active() const { return active_; }
  const Stats &stats() const { return stats_; }

  // `target` is the screen to arrive at; `motion` says which way the picture
  // travels: +1 when the incoming screen enters from the right - or, with
  // `vertical`, from below - which is what a finger moving left (up)
  // produces. `from` is where the finger was along that axis.
  //
  // Up and down since the screen graph (designer device contract 2.10): a
  // history below its page comes up from below, beside a navigator that
  // stands still, as sideways.
  //
  // The two are given separately on purpose. Which screen comes next is the
  // project's business - it may bind a leftward swipe to the PREVIOUS screen
  // - while which way the picture slides is the hand's, and it always
  // follows the hand. Tying the two together would mean a project with
  // reversed bindings sliding its screens backwards under the finger.
  bool start(int target, int motion, int16_t from, bool vertical = false)
  {
    if (!ready() || active_) return false;
    if (target < 0 || target >= screenCount_ || target == *currentScreen_) return false;

    direction_ = motion >= 0 ? 1 : -1;
    vertical_ = vertical;
    target_ = target;
    setWindow(target);

    stats_ = Stats();
    stats_.valid = true;

    // The incoming screen is drawn once, into the spare canvas, while the
    // outgoing one is still on the panel. Everything after this is memory
    // movement.
    const uint32_t t0 = micros();
    renderer_->setCanvas(spare_);
    renderer_->renderScreen(target_);
    renderer_->setCanvas(*current_);
    stats_.startRenderUs = micros() - t0;

    startPos_ = from;
    offset_ = 0;
    // Started after the render, not before it: the finger travels during
    // those milliseconds and the panel cannot be read, so counting them
    // would report a throw as a crawl.
    startMs_ = millis();
    recoveries_ = 0;
    active_ = true;
    return true;
  }

  // Where the finger is now; the axis the gesture started on counts.
  void follow(int16_t x, int16_t y)
  {
    if (!active_) return;
    compose(clamp((int)(vertical_ ? y : x) - (int)startPos_));
  }

  // An empty touch read that turned out to be a dropout rather than a
  // lifted finger. Counted per gesture, because "it jams" and "the driver
  // loses samples" look identical from the outside.
  void noteRecovery()
  {
    if (recoveries_ < 65535) recoveries_++;
  }

  // Every gesture, not just the last one. One swipe says nothing about a
  // gesture that works four times out of five, and polling from outside
  // cannot help: a debug request is served between gestures, never during
  // one, because a transition owns the loop while it runs.
  String log() const
  {
    if (logCount_ == 0) return String("  no gestures recorded\n");
    String out = "  n  frames  ms/frame  duration   travel    speed     idle  drops  verdict\n";
    const uint32_t first = logCount_ > LOG_SIZE ? logCount_ - LOG_SIZE : 0;
    for (uint32_t i = first; i < logCount_; i++)
    {
      const Record &r = log_[i % LOG_SIZE];
      // Everything the gesture spent that was not composing. If this is
      // large, something outside this class is holding the loop.
      const uint32_t composedMs = r.composeUs / 1000;
      const uint32_t idleMs = r.durationMs > composedMs ? r.durationMs - composedMs : 0;
      out += "  " + pad(String(i + 1), 3) + pad(String(r.frames), 8) +
             pad(r.frames ? String(r.composeUs / r.frames / 1000.0f, 1) : String("-"), 10) +
             pad(String(r.durationMs) + "ms", 10) + pad(String(r.travel) + "px", 9) +
             pad(String(r.pxPerSecond) + "px/s", 10) + pad(String(idleMs) + "ms", 9) +
             pad(String(r.recoveries), 7) +
             String(r.committed ? (r.flicked ? "paged (flick)" : "paged") : "sprang back") + "\n";
    }
    return out;
  }

  void clearLog()
  {
    logCount_ = 0;
  }

  // Decides, finishes the movement so the screen does not stop halfway, and
  // hands the contact back.
  void release()
  {
    if (!active_) return;

    const int travelled = offset_;
    const uint32_t elapsed = millis() - startMs_;
    // A flick is recognised by speed, because distance is exactly what gets
    // lost here. One frame is 66ms and the panel cannot be sampled during
    // it, so a throw across the screen arrives as two or three samples and
    // whatever travel happened to be visible at the last one. Judged on
    // distance it reads as a nudge and springs back, which is precisely the
    // "sometimes it does nothing" that this gesture was reported for.
    const int speed = elapsed > 0 ? (int)((int64_t)abs(travelled) * 1000 / (int64_t)elapsed) : 0;
    const bool flick = speed >= FLICK_PX_PER_SECOND && abs(travelled) >= FLICK_MIN_PX;
    const bool commit = abs(travelled) >= span_ / 3 || flick;
    const int finalOffset = commit ? direction_ * -span_ : 0;

    // Glide the rest of the way rather than jumping it. At 15fps this is a
    // handful of frames, and leaving it out would hide exactly the part of
    // the movement being judged.
    while (offset_ != finalOffset)
    {
      int step = finalOffset - offset_;
      if (step > GLIDE_STEP_PX) step = GLIDE_STEP_PX;
      if (step < -GLIDE_STEP_PX) step = -GLIDE_STEP_PX;
      compose(offset_ + step);
    }

    if (commit)
    {
      // The framebuffer already shows the incoming screen in full, so there
      // is nothing left to present - only the bookkeeping, so that the next
      // MQTT value renders into the canvas that is actually on the panel.
      ClippedCanvas16 *previous = *current_;
      *current_ = spare_;
      spare_ = previous;
      renderer_->setCanvas(*current_);
      *currentScreen_ = target_;
    }

    stats_.travel = travelled;
    stats_.durationMs = elapsed;
    stats_.pxPerSecond = speed;
    stats_.committed = commit;
    stats_.flicked = flick;
    active_ = false;

    Record &r = log_[logCount_ % LOG_SIZE];
    r.frames = stats_.frames;
    r.composeUs = stats_.totalUs;
    r.durationMs = elapsed;
    r.travel = travelled;
    r.pxPerSecond = speed;
    r.recoveries = recoveries_;
    r.committed = commit;
    r.flicked = flick;
    logCount_++;
  }

  String report() const
  {
    // Said out loud rather than left as silence, because "nothing happens
    // when I swipe" has two very different causes and one of them is a
    // project with a single screen.
    if (!ready()) return String("  swipe off: needs a spare canvas and two screens\n");
    if (!stats_.valid) return String("  swipe ready, none since boot\n");
    String out = "  last swipe: " + String(stats_.frames) + " frames, " +
                 String(stats_.frames ? stats_.totalUs / stats_.frames / 1000.0f : 0.0f, 1) + " ms/frame, " +
                 String(stats_.frames && stats_.totalUs ? 1000000.0f * stats_.frames / stats_.totalUs : 0.0f, 1) +
                 " fps\n";
    out += "  worst frame " + String(stats_.worstUs / 1000.0f, 1) + " ms, incoming render " +
           String(stats_.startRenderUs / 1000.0f, 1) + " ms\n";
    out += "  travelled " + String(stats_.travel) + " px in " + String(stats_.durationMs) + " ms, " +
           String(stats_.pxPerSecond) + " px/s, " + (stats_.committed ? "committed" : "sprang back") +
           (stats_.flicked ? " (flick)" : "") + "\n";
    return out;
  }

private:
  struct Record {
    uint32_t frames = 0;
    uint32_t composeUs = 0;
    uint32_t durationMs = 0;
    int travel = 0;
    int pxPerSecond = 0;
    uint16_t recoveries = 0;
    bool committed = false;
    bool flicked = false;
  };
  static const int LOG_SIZE = 24;

  static String pad(const String &s, int width)
  {
    String out = s;
    while ((int)out.length() < width) out += " ";
    return out;
  }

  // Past a jittery contact, still immediate to the eye.
  static const int START_PX = 12;
  static const int GLIDE_STEP_PX = 120;
  // 400 px/s is half a screen a second, which no one produces by accident
  // while resting a finger. The minimum distance keeps a jittery contact
  // that moved 3px in one frame from counting as a throw.
  static const int FLICK_PX_PER_SECOND = 400;
  static const int FLICK_MIN_PX = 30;

  int clamp(int v) const
  {
    // Between "not moved" and "the incoming screen exactly in place", so
    // dragging backwards cannot pull in a third screen that was never
    // rendered.
    if (direction_ > 0) return v > 0 ? 0 : (v < -span_ ? -span_ : v);
    return v < 0 ? 0 : (v > span_ ? span_ : v);
  }

  // The part of the screen that slides (designer device contract 2.7): all
  // of it, or - when both screens show the same navigator - what lies
  // beside its strip; the strip stands still. Columns [winX0_, winX1_) of
  // rows [winY0_, winY1_); span_ is how far a screen travels.
  void setWindow(int target)
  {
    winX0_ = 0;
    winX1_ = w_;
    winY0_ = 0;
    winY1_ = h_;
    navigatorlayout::Layout here, there;
    const Navigator *navHere = nullptr, *navThere = nullptr;
    if (renderer_->navigatorLayout(*currentScreen_, here, &navHere) && renderer_->navigatorLayout(target, there, &navThere) &&
        navHere == navThere)
    {
      const navigatorlayout::Rect &st = here.strip;
      switch (here.edge)
      {
        case navigatorlayout::Edge::Left: winX0_ = st.x + st.width; break;
        case navigatorlayout::Edge::Right: winX1_ = st.x; break;
        case navigatorlayout::Edge::Top: winY0_ = st.y + st.height; break;
        case navigatorlayout::Edge::Bottom: winY1_ = st.y; break;
      }
    }
    span_ = vertical_ ? winY1_ - winY0_ : winX1_ - winX0_;
  }

  // One frame, straight into the panel's framebuffer. Per row: the strip's
  // columns copied as they are, two runs for the sliding part - this is the
  // whole cost of the gesture.
  void compose(int offset)
  {
    const uint32_t t0 = micros();
    // Into whichever buffer is not on the glass, and handed over whole at
    // the end. With one framebuffer this is the same pointer every time and
    // the frame tears; with two it cannot.
    uint16_t *fb_ = panel_->back();
    const uint16_t *out = (*current_)->getBuffer();
    const uint16_t *in = spare_->getBuffer();
    const size_t stride = (size_t)w_;
    const int span = span_;

    if (vertical_)
    {
      composeVertical(fb_, out, in, stride, span, offset);
      panel_->flip();
      offset_ = offset;
      const uint32_t d = micros() - t0;
      stats_.frames++;
      stats_.totalUs += d;
      if (d > stats_.worstUs) stats_.worstUs = d;
      return;
    }

    for (int16_t y = 0; y < h_; y++)
    {
      uint16_t *dst = fb_ + (size_t)y * stride;
      const uint16_t *outRow = out + (size_t)y * stride;
      const uint16_t *inRow = in + (size_t)y * stride;
      // A row of a top or bottom strip: as it is.
      if (y < winY0_ || y >= winY1_)
      {
        memcpy(dst, outRow, stride * 2);
        continue;
      }
      // The columns of a left or right strip: as they are.
      if (winX0_ > 0) memcpy(dst, outRow, (size_t)winX0_ * 2);
      if (winX1_ < w_) memcpy(dst + winX1_, outRow + winX1_, (size_t)(w_ - winX1_) * 2);
      uint16_t *win = dst + winX0_;
      const uint16_t *outWin = outRow + winX0_;
      const uint16_t *inWin = inRow + winX0_;
      if (offset <= 0)
      {
        const int keep = span + offset;  // outgoing pixels still on screen, on the left
        if (keep > 0) memcpy(win, outWin + (-offset), (size_t)keep * 2);
        if (keep < span) memcpy(win + keep, inWin, (size_t)(span - keep) * 2);
      }
      else
      {
        const int show = offset;  // incoming pixels, on the left
        memcpy(win, inWin + (span - show), (size_t)show * 2);
        if (show < span) memcpy(win + show, outWin, (size_t)(span - show) * 2);
      }
    }

    panel_->flip();

    offset_ = offset;
    const uint32_t d = micros() - t0;
    stats_.frames++;
    stats_.totalUs += d;
    if (d > stats_.worstUs) stats_.worstUs = d;
  }

  // The same up and down: each row of the window shows a row of the
  // outgoing or the incoming screen further along, the strip's columns and
  // rows stay as they are. offset < 0 moves the picture up (the incoming
  // screen from below).
  void composeVertical(uint16_t *fb, const uint16_t *out, const uint16_t *in, size_t stride, int span, int offset)
  {
    const int cols = winX1_ - winX0_;
    for (int16_t y = 0; y < h_; y++)
    {
      uint16_t *dst = fb + (size_t)y * stride;
      const uint16_t *outRow = out + (size_t)y * stride;
      if (y < winY0_ || y >= winY1_)
      {
        memcpy(dst, outRow, stride * 2);
        continue;
      }
      if (winX0_ > 0) memcpy(dst, outRow, (size_t)winX0_ * 2);
      if (winX1_ < w_) memcpy(dst + winX1_, outRow + winX1_, (size_t)(w_ - winX1_) * 2);
      const int rel = y - winY0_;
      const uint16_t *src;
      if (offset <= 0)
      {
        const int at = rel - offset;
        src = at < span ? out + (size_t)(winY0_ + at) * stride : in + (size_t)(winY0_ + at - span) * stride;
      }
      else
      {
        src = rel < offset ? in + (size_t)(winY0_ + span - offset + rel) * stride : out + (size_t)(winY0_ + rel - offset) * stride;
      }
      memcpy(dst + winX0_, src + winX0_, (size_t)cols * 2);
    }
  }

  RgbPanel *panel_ = nullptr;
  ClippedCanvas16 **current_ = nullptr;
  ClippedCanvas16 *spare_ = nullptr;
  ColorScreenRenderer *renderer_ = nullptr;
  int *currentScreen_ = nullptr;
  int screenCount_ = 0;
  int16_t w_ = 0, h_ = 0;
  int winX0_ = 0, winX1_ = 0, winY0_ = 0, winY1_ = 0, span_ = 0;

  bool active_ = false;
  int direction_ = 1;
  int target_ = 0;
  bool vertical_ = false;
  int16_t startPos_ = 0;
  uint32_t startMs_ = 0;
  int offset_ = 0;
  uint16_t recoveries_ = 0;
  Stats stats_;
  Record log_[LOG_SIZE];
  uint32_t logCount_ = 0;

public:
  static int startThresholdPx() { return START_PX; }
};
