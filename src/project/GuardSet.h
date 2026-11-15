#pragma once

// The guarded controls of the screen showing, for a board's touch code
// (device contract §2.14, docs/2026-11-15-guard.md in the designer): one
// cover per control that has one, the finger pushing one, the hint. The
// gesture itself is guard::Cover (Guard.h); this keeps the covers, hands the
// renderer what to draw and asks the board to redraw where a cover moved.
//
// Shared by the three touch loops (rgbpanel, the Knob's, the PaperS3's): each
// calls down/move/up from its own contact handling, tick from its loop and
// clear wherever the screen changes or the panel goes dark.

#include <Arduino.h>
#include <functional>
#include <vector>

#include "ColorScreenRenderer.h"
#include "Guard.h"

class GuardSet {
 public:
  using Redraw = std::function<void(int16_t x, int16_t y, int16_t w, int16_t h, const char* reason)>;

  // `motion` false: the PaperS3 (E7) - the cover does not follow the finger,
  // no bob, no fade; it is drawn where it lies, waiting, or gone.
  void begin(ColorScreenRenderer* renderer, Redraw redraw, bool motion = true) {
    renderer_ = renderer;
    redraw_ = redraw;
    motion_ = motion;
    if (renderer_) renderer_->setGuardOverlays(&overlays_);
  }

  // A finger down on `obj` (its position on the glass objX/objY). True when
  // the cover takes the contact: the control is guarded and not unlocked -
  // nothing is switched, no pressed look, no swipe (decisions 5, 8, E6, E8).
  bool down(const ScreenObject& obj, int screen, int16_t objX, int16_t objY, int x, int y, uint32_t now) {
    if (!obj.guard.on || unlocked(obj.id)) return false;
    Entry& e = entryFor(obj.id);
    e.screen = screen;
    e.x = objX + obj.guard.coverX;
    e.y = objY + obj.guard.coverY;
    e.w = obj.guard.coverW;
    e.h = obj.guard.coverH;
    e.r = obj.guard.coverR;
    e.travel = obj.guard.travel;
    e.cover.down(now, x, y);
    finger_ = obj.id;
    refresh(e, now, "guard down");
    return true;
  }

  bool fingerActive() const { return !finger_.isEmpty(); }

  void move(int x, int y, uint32_t now) {
    Entry* e = find(finger_);
    if (e == nullptr) return;
    const int before = e->cover.offset;
    const bool lockedIn = e->cover.move(now, x, y, e->travel);
    // Where nothing moves (E7) only locking in is worth a refresh.
    if (lockedIn || (motion_ && e->cover.offset != before)) refresh(*e, now, lockedIn ? "guard locked in" : "guard push");
    tick(now);
  }

  // The finger lifted. True for a plain tap: show the hint (E5).
  bool up(uint32_t now) {
    Entry* e = find(finger_);
    finger_ = "";
    if (e == nullptr) return false;
    const bool hint = e->cover.up(now);
    if (hint) showHint(now);
    // Paper (E7): gone in one refresh, which the next tick makes.
    if (!motion_) e->cover.reset();
    else refresh(*e, now, "guard up");
    return hint;
  }

  bool unlocked(const String& id) const {
    for (const Entry& e : entries_)
      if (e.id == id) return e.cover.unlocked();
    return false;
  }

  // A tap changed the control while its cover waits: top up (decision 13).
  void changed(const String& id, uint32_t now) {
    Entry* e = find(id);
    if (e != nullptr) e->cover.changed(now);
  }

  // Time passing: covers open, wait, drop; the arrows bob; the hint ends. A
  // control no longer showing - its panel closed - locks at once (E4).
  void tick(uint32_t now) {
    if (entries_.empty() && hintUntil_ == 0) return;
    for (size_t i = 0; i < entries_.size();) {
      Entry& e = entries_[i];
      int16_t ox = 0, oy = 0;
      const bool visible = renderer_ != nullptr && renderer_->findObjectById(e.screen, e.id, ox, oy) != nullptr;
      if (!visible) {
        if (finger_ == e.id) finger_ = "";
        const Entry gone = e;
        entries_.erase(entries_.begin() + i);
        rebuild(now);
        redrawReach(gone, "guard hidden");
        continue;
      }
      const bool changed = e.cover.tick(now);
      // Paper (E7): no fall, no fade - the drop is the cover gone, one refresh.
      if (!motion_ && e.cover.phase == guard::Phase::Dropping) e.cover.reset();
      if (!e.cover.showing()) {
        const Entry gone = e;
        entries_.erase(entries_.begin() + i);
        rebuild(now);
        redrawReach(gone, "guard gone");
        continue;
      }
      // Moving parts: the bob while it can be pushed, the open and drop slides.
      const bool moving = motion_ && (e.cover.phase == guard::Phase::Pushing || e.cover.phase == guard::Phase::Opening ||
                                      e.cover.phase == guard::Phase::Dropping);
      if (changed || (moving && now - e.lastFrame >= FRAME_MS)) refresh(e, now, "guard tick");
      i++;
    }
    if (hintUntil_ != 0 && (int32_t)(now - hintUntil_) >= 0) {
      hintUntil_ = 0;
      if (renderer_) renderer_->setGuardHint(false);
      redrawHint("guard hint gone");
    }
  }

  // Every cover gone at once, without animation (E2): a screen change, the
  // panel going dark, a project reload. The caller redraws the screen.
  void clear() {
    entries_.clear();
    overlays_.clear();
    finger_ = "";
    hintUntil_ = 0;
    if (renderer_) renderer_->setGuardHint(false);
  }

  bool any() const { return !entries_.empty() || hintUntil_ != 0; }

 private:
  static constexpr uint32_t FRAME_MS = 33;

  struct Entry {
    String id;
    int screen = 0;
    int16_t x = 0, y = 0, w = 0, h = 0, r = 0;
    int travel = 20;
    guard::Cover cover;
    uint32_t lastFrame = 0;
    int16_t drawnX = 0, drawnY = 0, drawnW = 0, drawnH = 0;
    bool drawn = false;
  };

  Entry& entryFor(const String& id) {
    Entry* e = find(id);
    if (e != nullptr) return *e;
    entries_.push_back(Entry());
    entries_.back().id = id;
    return entries_.back();
  }

  Entry* find(const String& id) {
    if (id.isEmpty()) return nullptr;
    for (Entry& e : entries_)
      if (e.id == id) return &e;
    return nullptr;
  }

  void rebuild(uint32_t now) {
    overlays_.clear();
    for (const Entry& e : entries_) {
      const guard::Drawn d = guard::drawn(e.cover, now, e.h, motion_);
      ColorScreenRenderer::GuardOverlay o;
      o.x = e.x;
      o.y = e.y;
      o.w = e.w;
      o.h = e.h;
      o.r = e.r;
      o.lift = d.lift;
      o.alpha = d.alpha;
      o.bob = d.bob;
      overlays_.push_back(o);
    }
  }

  // The cover drawn anew: the overlays rebuilt, then the union of where it
  // was and where it is redrawn.
  void refresh(Entry& e, uint32_t now, const char* reason) {
    rebuild(now);
    e.lastFrame = now;
    const guard::Drawn d = guard::drawn(e.cover, now, e.h, motion_);
    const int16_t top = (int16_t)(e.y - d.lift - (d.bob > 0 ? d.bob : 0));
    int16_t x0 = e.x, y0 = top, x1 = e.x + e.w, y1 = (int16_t)(e.y - d.lift + e.h);
    if (e.drawn) {
      if (e.drawnX < x0) x0 = e.drawnX;
      if (e.drawnY < y0) y0 = e.drawnY;
      if (e.drawnX + e.drawnW > x1) x1 = e.drawnX + e.drawnW;
      if (e.drawnY + e.drawnH > y1) y1 = e.drawnY + e.drawnH;
    }
    e.drawn = true;
    e.drawnX = e.x;
    e.drawnY = top;
    e.drawnW = e.w;
    e.drawnH = (int16_t)(e.h + (d.bob > 0 ? d.bob : 0));
    redrawClamped(x0, y0, x1 - x0, y1 - y0, reason);
  }

  void redrawReach(const Entry& e, const char* reason) {
    const int wait = guard::waitOffset(e.h);
    redrawClamped(e.x, (int16_t)(e.y - wait - guard::BOB_PX), e.w, (int16_t)(e.h + wait + guard::BOB_PX), reason);
  }

  void redrawClamped(int x, int y, int w, int h, const char* reason) {
    if (!redraw_ || renderer_ == nullptr || renderer_->canvas() == nullptr) return;
    const int W = renderer_->canvas()->width(), H = renderer_->canvas()->height();
    if (x < 0) {
      w += x;
      x = 0;
    }
    if (y < 0) {
      h += y;
      y = 0;
    }
    if (x + w > W) w = W - x;
    if (y + h > H) h = H - y;
    if (w > 0 && h > 0) redraw_((int16_t)x, (int16_t)y, (int16_t)w, (int16_t)h, reason);
  }

  void showHint(uint32_t now) {
    hintUntil_ = now + guard::HINT_MS;
    if (hintUntil_ == 0) hintUntil_ = 1;
    if (renderer_) renderer_->setGuardHint(true);
    redrawHint("guard hint");
  }

  void redrawHint(const char* reason) {
    if (renderer_ == nullptr) return;
    int16_t x, y, w, h;
    if (renderer_->guardHintRect(x, y, w, h)) redrawClamped(x, y, w, h, reason);
  }

  ColorScreenRenderer* renderer_ = nullptr;
  Redraw redraw_;
  bool motion_ = true;
  std::vector<Entry> entries_;
  std::vector<ColorScreenRenderer::GuardOverlay> overlays_;
  String finger_;
  uint32_t hintUntil_ = 0;
};
