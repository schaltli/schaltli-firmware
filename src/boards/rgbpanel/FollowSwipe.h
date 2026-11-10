#pragma once
#include "project/NavigatorLayout.h"
#include <Arduino.h>
#include <algorithm>
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
//
// The navigator's marker glides with it (designer #59, 2026-10-18): where
// the swipe leads from one entry's screen to another's, the strip is drawn
// twice at the start - every entry normal into the outgoing canvas, every
// entry active on the marker's colour into a third one - and each frame
// copies the marker's box, row by row inside its corners, from the second
// over the first. No strip is drawn per frame. Once the finger lifts the
// marker settles on its own clock (navigatorlayout::markerSettle), a little
// past the entry and back, with the screen standing; then the true strip.
class FollowSwipe {
public:
  // What the last gesture cost, so the feel can be argued about with numbers
  // next to it rather than from memory.
  struct Stats {
    uint32_t frames = 0;
    uint32_t totalUs = 0;
    uint32_t worstUs = 0;
    uint32_t startRenderUs = 0;
    // Of it, the navigator's strip drawn for the marker (prepareMarker).
    uint32_t stripUs = 0;
    uint32_t durationMs = 0;
    int travel = 0;
    int pxPerSecond = 0;
    bool committed = false;
    bool flicked = false;
    bool valid = false;
    // The marker's frames after the lift, with the screen standing (#59).
    uint32_t markerFrames = 0;
    uint32_t markerUs = 0;
  };

  void attach(RgbPanel *panel, ClippedCanvas16 **current, ClippedCanvas16 *spare,
              ColorScreenRenderer *renderer, int *currentScreen, int screenCount,
              int16_t w, int16_t h, ClippedCanvas16 *accent = nullptr)
  {
    accent_ = accent;
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
    const uint32_t t1 = micros();
    marker_ = prepareMarker(target_);
    stats_.stripUs = micros() - t1;
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
    const int offset = clamp((int)(vertical_ ? y : x) - (int)startPos_);
    if (marker_) markerAt_ = navigatorlayout::markerOffset(markerLayout_, markerFrom_, markerTo_, offset, span_);
    compose(offset);
  }

  // A frame with the marker `offset` px on from this screen's entry towards
  // `target`'s, on the glass only - the canvas keeps the screen as it is.
  // For the HIL (/api/navigator-marker): the frame a swipe would show there,
  // to hold against the designer's. False where the marker would not glide.
  bool showMarker(int target, int offset)
  {
    if (!ready() || active_ || target < 0 || target >= screenCount_) return false;
    if (!prepareMarker(target)) return false;
    markerAt_ = offset;
    composeStill(*current_, *current_);
    renderer_->renderNavigatorStrip(*currentScreen_, ColorScreenRenderer::StripLook::AsShown);
    marker_ = false;
    return true;
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
    // The marker from where the finger left it to the entry arrived at, or
    // back to its own, on its own clock from now.
    const uint32_t releaseMs = millis();
    const int markerStart = markerAt_;
    const int markerEnd = commit ? (markerTo_ - markerFrom_) * markerLayout_.entryLength : 0;

    // Glide the rest of the way rather than jumping it. At 15fps this is a
    // handful of frames, and leaving it out would hide exactly the part of
    // the movement being judged.
    while (offset_ != finalOffset)
    {
      int step = finalOffset - offset_;
      if (step > GLIDE_STEP_PX) step = GLIDE_STEP_PX;
      if (step < -GLIDE_STEP_PX) step = -GLIDE_STEP_PX;
      if (marker_) markerAt_ = navigatorlayout::markerSettle(markerStart, markerEnd, (long)(millis() - releaseMs));
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

    if (marker_)
    {
      // The screen stands; the marker settles over the strip with every
      // entry normal - the outgoing canvas's, which after a page is the
      // spare one.
      ClippedCanvas16 *normal = commit ? spare_ : *current_;
      while (millis() - releaseMs < (uint32_t)navigatorlayout::kMarkerSettleMs)
      {
        markerAt_ = navigatorlayout::markerSettle(markerStart, markerEnd, (long)(millis() - releaseMs));
        composeStill(*current_, normal);
      }
      // At rest: the strip as the screen shows it.
      if (!commit) renderer_->renderNavigatorStrip(*currentScreen_, ColorScreenRenderer::StripLook::AsShown);
      markerAt_ = markerEnd;
      uint16_t *fb = panel_->back();
      memcpy(fb, (*current_)->getBuffer(), (size_t)w_ * h_ * 2);
      panel_->flip();
      marker_ = false;
    }

    stats_.travel = travelled;
    stats_.durationMs = elapsed;
    stats_.pxPerSecond = speed;
    stats_.committed = commit;
    stats_.flicked = flick;
    active_ = false;

    // The frames went straight into the panel's buffers, past its book of
    // what each still owes: only the last one shows the screen at rest, the
    // others hold frames of the slide. Whatever presented a rectangle next
    // brought one of those back around it - with a Camera, ten times a
    // second, so after swiping away and back the picture jittered and a
    // shifted «No live image» from the slide flashed beside it (the user,
    // 2026-11-10). Owed, not copied now: each buffer takes the canvas whole
    // at its next present.
    panel_->oweEverything();

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
           String(stats_.startRenderUs / 1000.0f, 1) + " ms (the strip for the marker " + String(stats_.stripUs / 1000.0f, 1) + " ms)\n";
    if (stats_.markerFrames > 0)
      out += "  the navigator's marker settled in " + String(stats_.markerFrames) + " frames, " +
             String(stats_.markerUs / stats_.markerFrames / 1000.0f, 1) + " ms/frame\n";
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
      if (marker_) overlayMarker(fb_);
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

    if (marker_) overlayMarker(fb_);
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

  // Whether the marker glides with this swipe, and from which entry to
  // which: the navigator laid out, its box, the scroll; both strips drawn.
  bool prepareMarker(int target)
  {
    navigatorlayout::Layout here, there;
    const Navigator *navHere = nullptr, *navThere = nullptr;
    int from = -1, to = -1;
    if (!accent_ || !accent_->getBuffer()) return false;
    if (!renderer_->navigatorLayout(*currentScreen_, here, &navHere, &from) || !renderer_->navigatorLayout(target, there, &navThere, &to) ||
        navHere != navThere)
      return false;
    const int scroll = renderer_->effectiveNavigatorScroll(*currentScreen_);
    if (!navigatorlayout::markerGlides(from, to, scroll, renderer_->effectiveNavigatorScroll(target))) return false;
    if (!renderer_->navigatorMarkerBox(*currentScreen_, markerBox_, markerRadius_)) return false;
    // Both strips are the same for every screen of this navigator at this
    // scroll and theme, so they are drawn once and kept: drawn anew they were
    // 53 ms of every swipe's start, 310 ms of the first (4.3B, 2026-10-30).
    const bool dark = renderer_->themeDark();
    if (!restoreStrips(navHere, dark, scroll, here.bar))
    {
      renderer_->setCanvas(accent_);
      renderer_->renderNavigatorStrip(*currentScreen_, ColorScreenRenderer::StripLook::AllActive);
      renderer_->setCanvas(*current_);
      renderer_->renderNavigatorStrip(*currentScreen_, ColorScreenRenderer::StripLook::AllNormal);
      keepStrips(navHere, dark, scroll, here.bar);
    }
    markerLayout_ = here;
    markerFrom_ = from;
    markerTo_ = to;
    markerScroll_ = scroll;
    markerAt_ = 0;
    return true;
  }

  // The bar's rectangle within the panel, row by row, between a canvas and a
  // kept copy of it.
  void copyBar(const navigatorlayout::Rect &bar, uint16_t *canvas, uint16_t *kept, bool toKept)
  {
    const int x0 = std::max(bar.x, 0), x1 = std::min(bar.x + bar.width, (int)w_);
    const int y0 = std::max(bar.y, 0), y1 = std::min(bar.y + bar.height, (int)h_);
    if (x1 <= x0 || y1 <= y0) return;
    const size_t rowBytes = (size_t)(x1 - x0) * 2;
    for (int y = y0; y < y1; y++)
    {
      uint16_t *c = canvas + (size_t)y * w_ + x0;
      uint16_t *k = kept + (size_t)(y - y0) * (x1 - x0);
      if (toKept) memcpy(k, c, rowBytes);
      else memcpy(c, k, rowBytes);
    }
  }

  // The two strips prepareMarker() draws, kept in PSRAM after the first swipe.
  void keepStrips(const Navigator *nav, bool dark, int scroll, const navigatorlayout::Rect &bar)
  {
    const size_t pixels = (size_t)std::max(bar.width, 0) * (size_t)std::max(bar.height, 0);
    if (pixels == 0) return;
    if (pixels != stripPixels_)
    {
      free(stripActive_);
      free(stripNormal_);
      stripActive_ = (uint16_t *)heap_caps_malloc(pixels * 2, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
      stripNormal_ = (uint16_t *)heap_caps_malloc(pixels * 2, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
      stripPixels_ = pixels;
    }
    stripNav_ = nullptr;
    if (!stripActive_ || !stripNormal_) return;
    copyBar(bar, accent_->getBuffer(), stripActive_, true);
    copyBar(bar, (*current_)->getBuffer(), stripNormal_, true);
    stripNav_ = nav;
    stripDark_ = dark;
    stripScroll_ = scroll;
    stripBar_ = bar;
  }

  // Both strips back where prepareMarker() would draw them; false when what
  // is kept is for another navigator, scroll, theme or place.
  bool restoreStrips(const Navigator *nav, bool dark, int scroll, const navigatorlayout::Rect &bar)
  {
    if (!stripNav_ || stripNav_ != nav || stripDark_ != dark || stripScroll_ != scroll || stripBar_.x != bar.x ||
        stripBar_.y != bar.y || stripBar_.width != bar.width || stripBar_.height != bar.height)
      return false;
    copyBar(bar, accent_->getBuffer(), stripActive_, false);
    copyBar(bar, (*current_)->getBuffer(), stripNormal_, false);
    return true;
  }

  // The marker's box at markerAt_, from the accent canvas over `fb`: row by
  // row inside its corners (navigatorlayout::cornerInset), within the
  // entries' part of the strip.
  void overlayMarker(uint16_t *fb)
  {
    const navigatorlayout::Rect m = navigatorlayout::markerRect(markerLayout_, markerFrom_, markerAt_, markerScroll_, markerBox_);
    const navigatorlayout::Rect &e = markerLayout_.strip;
    const uint16_t *accent = accent_->getBuffer();
    const size_t stride = (size_t)w_;
    for (int row = 0; row < m.height; row++)
    {
      const int y = m.y + row;
      if (y < e.y || y >= e.y + e.height || y < 0 || y >= h_) continue;
      const int inset = navigatorlayout::cornerInset(markerRadius_, m.width, m.height, row);
      const int x0 = std::max(std::max(m.x + inset, e.x), 0);
      const int x1 = std::min(std::min(m.x + m.width - inset, e.x + e.width), (int)w_);
      if (x1 > x0) memcpy(fb + (size_t)y * stride + x0, accent + (size_t)y * stride + x0, (size_t)(x1 - x0) * 2);
    }
  }

  // A frame with the screen standing: `screen` whole, the entries' part of
  // the strip from `normal`, the marker over it.
  void composeStill(ClippedCanvas16 *screen, ClippedCanvas16 *normal)
  {
    const uint32_t t0 = micros();
    uint16_t *fb = panel_->back();
    const size_t stride = (size_t)w_;
    memcpy(fb, screen->getBuffer(), stride * h_ * 2);
    const navigatorlayout::Rect &e = markerLayout_.strip;
    if (normal != screen)
      for (int y = std::max(0, e.y); y < std::min((int)h_, e.y + e.height); y++)
        memcpy(fb + (size_t)y * stride + e.x, normal->getBuffer() + (size_t)y * stride + e.x, (size_t)e.width * 2);
    overlayMarker(fb);
    panel_->flip();
    stats_.markerFrames++;
    stats_.markerUs += micros() - t0;
  }

  RgbPanel *panel_ = nullptr;
  ClippedCanvas16 *accent_ = nullptr;
  bool marker_ = false;
  navigatorlayout::Layout markerLayout_;
  navigatorlayout::Rect markerBox_;
  int markerRadius_ = 0;
  int markerFrom_ = 0, markerTo_ = 0, markerScroll_ = 0, markerAt_ = 0;
  // The strips kept by keepStrips(), and what they were drawn for.
  uint16_t *stripActive_ = nullptr;
  uint16_t *stripNormal_ = nullptr;
  size_t stripPixels_ = 0;
  const Navigator *stripNav_ = nullptr;
  bool stripDark_ = false;
  int stripScroll_ = 0;
  navigatorlayout::Rect stripBar_;
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
  // Where the marker was in the last frame, px from its entry's place, and
  // how many frames it settled in after the last lift (0: it did not glide).
  int markerOffset() const { return markerAt_; }
  uint32_t markerFrames() const { return stats_.markerFrames; }
};
