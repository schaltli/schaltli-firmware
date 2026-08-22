#pragma once
#include <Arduino.h>
#include <lvgl.h>
#include "ClippedCanvas16.h"

// The compositor seam - decision 2 of the 2026-08-19 port grilling.
//
// ColorScreenRenderer (ported from screenbee-m5dial@d73d0ff) keeps drawing
// pixel-perfect into its own RGB565 ClippedCanvas16 exactly as it does on
// the M5 Dial, unchanged. The only thing that differs on this board is
// where that buffer goes: instead of a direct pushImage() to the panel, it
// is handed to LVGL as an image, and LVGL flushes it.
//
// Why keep LVGL in the path at all when a direct blit would work: it costs
// almost nothing here (see the zero-copy note below) and it is what makes
// animated backgrounds, screen transitions and other LVGL objects
// composable *around* the rendered screen later, without touching the
// renderer. Rebuilding the designer's controls as native LVGL widgets was
// considered and rejected as overinvestment - it would mean dismantling a
// renderer whose pixel behavior is HIL-verified against the designer.
//
// **Zero-copy, and why that is safe here**: lv_conf.h sets
// LV_COLOR_DEPTH 16 with LV_COLOR_16_SWAP 0, so lv_color_t is a native
// uint16_t RGB565 - byte-identical to what GFXcanvas16 stores. The image
// descriptor therefore points straight at the canvas buffer rather than
// copying 259KB per frame. If either of those lv_conf.h settings ever
// changes, this class breaks silently (wrong colors, not a crash) - hence
// the static_assert in the .cpp.
class CanvasPresenter {
public:
  explicit CanvasPresenter(uint16_t width, uint16_t height);

  // Allocates the canvas and builds the LVGL image object on the active
  // screen. Returns false if the canvas buffer could not be allocated -
  // 360x360x2 is 259200 bytes, which must land in PSRAM (see begin()'s
  // own comment); callers should treat failure as fatal rather than
  // rendering into a null buffer.
  bool begin();

  // The surface ColorScreenRenderer draws into. Only valid after a
  // successful begin().
  ClippedCanvas16& canvas() { return *canvas_; }

  // Tells LVGL the buffer changed, so the next lv_timer_handler() flushes
  // it. Call once after a full screen render completes, mirroring the M5
  // Dial's single pushImage() after ColorScreenRenderer::render().
  void present();

  // The LVGL object showing the rendered screen. Exposed so later work can
  // compose around it - z-order it above an animated background, fade it
  // during a screen transition, overlay LVGL widgets on top.
  lv_obj_t* imageObject() { return image_; }

  // ---------------------------------------------------------- transitions
  //
  // A second canvas of the same size, normally parked off-screen, so a
  // screen change can be *shown* moving instead of appearing. The incoming
  // screen is rendered into it while the outgoing one is still visible, and
  // both are then moved together - which is what lets a swipe drag the
  // boundary under the finger rather than cutting at release.
  //
  // This is the composition the class comment above promised: no change to
  // ColorScreenRenderer, which still draws a flat RGB565 screen into a
  // canvas and knows nothing about any of this. It costs a second 259KB
  // buffer in PSRAM, which is why it is allocated once at begin() rather
  // than per gesture - a mid-gesture 259KB allocation would stall exactly
  // when smoothness matters, and could fail.
  //
  // Deliberately no ownership of *what* the transition means: the caller
  // decides which screen is incoming and from which edge. This class only
  // knows that two images move together.
  // Two of them, one per neighbour, so both directions can be standing
  // ready before a finger ever touches the glass.
  //
  // Rendering the incoming screen at the moment a drag starts blocks the
  // loop for as long as that project's screen takes - 165ms measured on a
  // real one - and the panel is not read at all while it does. A swipe that
  // finishes inside that window is truncated to whatever was sampled before
  // it began, which is a handful of pixels, and everything downstream then
  // judges the gesture on those: it looked like a nudge, sprang back, and
  // whatever the swipe was bound to never fired. Three failures in ten real
  // attempts, recorded on hardware 2026-08-22.
  static const int TRANSITION_SLOTS = 2;

  ClippedCanvas16& slotCanvas(int slot) { return *transitionCanvas_[slot]; }

  // Which screen a slot currently holds, or -1 if nothing usable.
  int slotScreen(int slot) const { return transitionScreen_[slot]; }
  void setSlotScreen(int slot, int screenIndex) { transitionScreen_[slot] = screenIndex; }

  // The slot already holding this screen, or -1.
  int slotHolding(int screenIndex) const;

  // A slot to render this screen into: an empty one if there is one, else
  // the one not holding the screen we are coming from.
  int slotToRenderInto(int screenIndex) const;

  // Forget what the slots hold. Anything that can change what a screen looks
  // like - a new project, a topic value arriving - has to call this, or a
  // drag will slide in a picture of how that screen used to look.
  void invalidateSlots();

  // Straight copies between the live canvas and a spare, for anything that
  // needs to put the screen back the way it was without paying to render it
  // again. 259KB, about 12ms, against 165ms to re-render a screen on a real
  // project - which is the difference between an animation and a slideshow.
  bool copyCanvasToSlot(int slot);
  bool copySlotToCanvas(int slot);

  // Parks the incoming image at (baseX, baseY) relative to centre - the
  // edge it will travel in from - and makes it visible. Call after
  // rendering into transitionCanvas().
  void beginTransition(int slot, int16_t baseX, int16_t baseY);

  // Moves both images: the outgoing one to (dx, dy), the incoming one to
  // its base plus the same delta. So the two stay rigidly joined and the
  // seam between them tracks the finger exactly.
  void setTransitionOffset(int16_t dx, int16_t dy);

  // Hides the incoming image and puts the outgoing one back in the centre.
  //
  // Note what this deliberately does NOT do: swap the two canvases. Several
  // objects captured `&canvas()` at startup (the screen-menu overlay, the
  // test interface), and swapping would leave every one of them drawing
  // into a buffer nobody displays - a fault that would look like random
  // missing overlays much later. A committed transition instead re-renders
  // the destination into the normal canvas while the incoming image is
  // still the one on screen, so the swap never has to happen.
  void endTransition();

  // Copies the incoming screen's pixels into the normal canvas, so the
  // transition can end without rendering that screen a second time.
  //
  // The alternative - re-rendering the destination - was what shipped first,
  // and it was wrong for a reason that only shows on a heavy project. The
  // whole call happens with the display frozen in whatever pose the last
  // animation step left, because nothing pumps LVGL while it runs. At 13ms a
  // screen nobody could see it; at 181ms (measured on a real project, 2026
  // -08-22) the screen sat visibly part-way across for a fifth of a second
  // at the end of every swipe. A 259KB copy is ~10ms and, unlike a re-render,
  // cannot disagree with what the user just watched slide into place.
  bool adoptTransitionCanvas();

  bool transitionActive() const { return transitionActive_; }

  // False when the second canvas could not be allocated. Callers fall back
  // to an instant screen change rather than skipping the change itself.
  bool canTransition() const { return transitionImage_[0] != nullptr; }

  // Where the two images actually sit, straight out of LVGL rather than out
  // of the caller's idea of where it put them. The distinction matters: the
  // whole class of bug here is the display ending up somewhere the state
  // machine believes it is not, and a probe that reports the state machine's
  // own belief back would confirm every such bug as fine.
  int16_t imageX() const { return image_ ? lv_obj_get_x(image_) : 0; }
  int16_t imageY() const { return image_ ? lv_obj_get_y(image_) : 0; }
  int16_t transitionImageX() const;
  int16_t transitionImageY() const;
  // True only when *no* slot is showing - "the transition is over" has to
  // mean all of them, not whichever one was asked about.
  bool transitionImageHidden() const;

private:
  uint16_t width_;
  uint16_t height_;
  ClippedCanvas16* canvas_ = nullptr;
  lv_img_dsc_t descriptor_ = {};
  lv_obj_t* image_ = nullptr;
  ClippedCanvas16* transitionCanvas_[TRANSITION_SLOTS] = {nullptr, nullptr};
  lv_img_dsc_t transitionDescriptor_[TRANSITION_SLOTS] = {};
  lv_obj_t* transitionImage_[TRANSITION_SLOTS] = {nullptr, nullptr};
  int transitionScreen_[TRANSITION_SLOTS] = {-1, -1};
  int activeSlot_ = -1;
  int16_t transitionBaseX_ = 0;
  int16_t transitionBaseY_ = 0;
  bool transitionActive_ = false;
};
