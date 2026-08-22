#include "CanvasPresenter.h"
#include <esp_heap_caps.h>
#include <esp_memory_utils.h>

// The zero-copy assumption this whole class rests on. A mismatch here
// would show up as wrong colors on a working-looking display, which is
// exactly the kind of symptom that cost days during this board's first
// bring-up - so fail the build instead.
static_assert(LV_COLOR_DEPTH == 16, "CanvasPresenter hands GFXcanvas16's RGB565 buffer to LVGL unconverted");
static_assert(LV_COLOR_16_SWAP == 0, "GFXcanvas16 stores native-order RGB565; LVGL must read it the same way");
static_assert(sizeof(lv_color_t) == sizeof(uint16_t), "lv_color_t must be a bare uint16_t for the zero-copy path");

CanvasPresenter::CanvasPresenter(uint16_t width, uint16_t height)
    : width_(width), height_(height) {}

bool CanvasPresenter::begin() {
  // GFXcanvas16 mallocs w*h*2 internally. On this board that is 259200
  // bytes, which must not come out of internal SRAM - LVGL's own pool, the
  // WiFi/MQTT stacks and the project loader all need what little of the
  // 512KB is left. Arduino-ESP32 routes large allocations to PSRAM
  // automatically when CONFIG_SPIRAM_USE_MALLOC is on, but that is a
  // build-config assumption rather than a guarantee, so where it actually
  // landed is checked and logged rather than hoped for. (The whole reason
  // this board replaced the M5 Dial was its PSRAM - see the designer
  // repo's memory-constraints notes.)
  canvas_ = new ClippedCanvas16(width_, height_);
  if (canvas_ == nullptr || canvas_->getBuffer() == nullptr) {
    Serial.printf("[CanvasPresenter] FATAL: could not allocate %u x %u canvas (%u bytes)\n",
                  width_, height_, (unsigned)(width_ * height_ * 2));
    return false;
  }

  const void* buffer = canvas_->getBuffer();
  Serial.printf("[CanvasPresenter] %ux%u canvas at %p (%s), %u bytes\n",
                width_, height_, buffer,
                esp_ptr_external_ram(buffer) ? "PSRAM" : "internal SRAM",
                (unsigned)(width_ * height_ * 2));
  if (!esp_ptr_external_ram(buffer)) {
    // Not fatal - it allocated, so it will render - but it is eating the
    // budget everything else needs, and that shows up later as an
    // unrelated-looking out-of-memory somewhere else entirely.
    Serial.println("[CanvasPresenter] WARNING: canvas is in internal SRAM, expected PSRAM");
  }

  descriptor_.header.cf = LV_IMG_CF_TRUE_COLOR;
  descriptor_.header.always_zero = 0;
  descriptor_.header.reserved = 0;
  descriptor_.header.w = width_;
  descriptor_.header.h = height_;
  descriptor_.data_size = (uint32_t)width_ * height_ * sizeof(lv_color_t);
  descriptor_.data = (const uint8_t*)buffer;

  // An LVGL screen is scrollable by default, and the transition parks a
  // full-screen child a whole screen width outside it. That makes the screen
  // scrollable in earnest, and a scrolled screen draws its children shifted -
  // which shows up as a band of bare screen background along the edge that
  // no image covers any more, exactly where a swipe left the pair.
  lv_obj_clear_flag(lv_scr_act(), LV_OBJ_FLAG_SCROLLABLE);

  image_ = lv_img_create(lv_scr_act());
  lv_img_set_src(image_, &descriptor_);
  lv_obj_align(image_, LV_ALIGN_CENTER, 0, 0);

  // The transition canvases. A failure here is not fatal: everything works
  // exactly as before without them, screen changes just cut instead of
  // sliding. Far better than refusing to boot over an animation. Both or
  // neither, so no code below has to ask how many there turned out to be.
  for (int slot = 0; slot < TRANSITION_SLOTS; slot++) {
    transitionCanvas_[slot] = new ClippedCanvas16(width_, height_);
    if (transitionCanvas_[slot] == nullptr || transitionCanvas_[slot]->getBuffer() == nullptr) {
      Serial.println("[CanvasPresenter] no transition canvases - screen changes will cut, not slide");
      for (int i = 0; i <= slot; i++) {
        delete transitionCanvas_[i];
        transitionCanvas_[i] = nullptr;
      }
      return true;
    }
    transitionDescriptor_[slot] = descriptor_;
    transitionDescriptor_[slot].data = (const uint8_t*)transitionCanvas_[slot]->getBuffer();
    transitionImage_[slot] = lv_img_create(lv_scr_act());
    lv_img_set_src(transitionImage_[slot], &transitionDescriptor_[slot]);
    lv_obj_add_flag(transitionImage_[slot], LV_OBJ_FLAG_HIDDEN);
    lv_obj_align(transitionImage_[slot], LV_ALIGN_CENTER, 0, 0);
    Serial.printf("[CanvasPresenter] transition canvas %d at %p (%s)\n", slot,
                  transitionCanvas_[slot]->getBuffer(),
                  esp_ptr_external_ram(transitionCanvas_[slot]->getBuffer()) ? "PSRAM" : "internal SRAM");
  }
  return true;
}

int CanvasPresenter::slotHolding(int screenIndex) const {
  for (int slot = 0; slot < TRANSITION_SLOTS; slot++) {
    if (transitionScreen_[slot] == screenIndex) return slot;
  }
  return -1;
}

int CanvasPresenter::slotToRenderInto(int screenIndex) const {
  for (int slot = 0; slot < TRANSITION_SLOTS; slot++) {
    if (transitionScreen_[slot] < 0) return slot;
  }
  // Both taken: overwrite one that is not the screen being asked for. The
  // other one is very likely the opposite neighbour and worth keeping.
  for (int slot = 0; slot < TRANSITION_SLOTS; slot++) {
    if (transitionScreen_[slot] != screenIndex) return slot;
  }
  return 0;
}

void CanvasPresenter::invalidateSlots() {
  for (int slot = 0; slot < TRANSITION_SLOTS; slot++) transitionScreen_[slot] = -1;
}

int16_t CanvasPresenter::transitionImageX() const {
  const int slot = activeSlot_ >= 0 ? activeSlot_ : 0;
  return transitionImage_[slot] ? lv_obj_get_x(transitionImage_[slot]) : 0;
}

int16_t CanvasPresenter::transitionImageY() const {
  const int slot = activeSlot_ >= 0 ? activeSlot_ : 0;
  return transitionImage_[slot] ? lv_obj_get_y(transitionImage_[slot]) : 0;
}

bool CanvasPresenter::transitionImageHidden() const {
  for (int slot = 0; slot < TRANSITION_SLOTS; slot++) {
    if (transitionImage_[slot] && !lv_obj_has_flag(transitionImage_[slot], LV_OBJ_FLAG_HIDDEN)) return false;
  }
  return true;
}

void CanvasPresenter::beginTransition(int slot, int16_t baseX, int16_t baseY) {
  if (slot < 0 || slot >= TRANSITION_SLOTS || transitionImage_[slot] == nullptr) return;
  activeSlot_ = slot;
  transitionBaseX_ = baseX;
  transitionBaseY_ = baseY;
  transitionActive_ = true;
  // Invalidated explicitly: the descriptor's data pointer never changes, so
  // LVGL has no way of knowing the pixels behind it were just redrawn.
  lv_obj_invalidate(transitionImage_[slot]);
  lv_obj_align(transitionImage_[slot], LV_ALIGN_CENTER, baseX, baseY);
  lv_obj_clear_flag(transitionImage_[slot], LV_OBJ_FLAG_HIDDEN);
}

void CanvasPresenter::setTransitionOffset(int16_t dx, int16_t dy) {
  if (!transitionActive_ || activeSlot_ < 0) return;
  lv_obj_align(image_, LV_ALIGN_CENTER, dx, dy);
  lv_obj_align(transitionImage_[activeSlot_], LV_ALIGN_CENTER, transitionBaseX_ + dx, transitionBaseY_ + dy);
}

bool CanvasPresenter::copyCanvasToSlot(int slot) {
  if (slot < 0 || slot >= TRANSITION_SLOTS || canvas_ == nullptr || transitionCanvas_[slot] == nullptr) return false;
  memcpy(transitionCanvas_[slot]->getBuffer(), canvas_->getBuffer(),
         (size_t)width_ * (size_t)height_ * sizeof(uint16_t));
  return true;
}

bool CanvasPresenter::copySlotToCanvas(int slot) {
  if (slot < 0 || slot >= TRANSITION_SLOTS || canvas_ == nullptr || transitionCanvas_[slot] == nullptr) return false;
  memcpy(canvas_->getBuffer(), transitionCanvas_[slot]->getBuffer(),
         (size_t)width_ * (size_t)height_ * sizeof(uint16_t));
  return true;
}

bool CanvasPresenter::adoptTransitionCanvas() {
  if (canvas_ == nullptr || activeSlot_ < 0 || transitionCanvas_[activeSlot_] == nullptr) return false;
  memcpy(canvas_->getBuffer(), transitionCanvas_[activeSlot_]->getBuffer(),
         (size_t)width_ * (size_t)height_ * sizeof(uint16_t));
  lv_obj_invalidate(image_);
  return true;
}

void CanvasPresenter::endTransition() {
  if (transitionImage_[0] == nullptr) return;
  transitionActive_ = false;
  for (int slot = 0; slot < TRANSITION_SLOTS; slot++) {
    lv_obj_add_flag(transitionImage_[slot], LV_OBJ_FLAG_HIDDEN);
  }
  activeSlot_ = -1;
  lv_obj_align(image_, LV_ALIGN_CENTER, 0, 0);
  // lv_obj_align only marks the layout dirty; the coordinates it implies
  // are not real until a refresh applies them. Everywhere else that is
  // harmless - the next refresh draws the right thing either way - but here
  // it is the difference between the screen being centred and sitting
  // wherever the drag left it, so it is forced now rather than left to
  // whenever LVGL's refresh period next comes round.
  lv_obj_update_layout(image_);
  // And repaint the whole screen, not just what LVGL worked out had moved.
  //
  // Measured on hardware (2026-08-22) with a readback of what actually
  // reached the panel: after a committed drag a 34px stripe down the right
  // edge still held the previous composition, every row, while the rest of
  // the glass was correct. The panel has no framebuffer to fall back on -
  // whatever is not rewritten simply stays - so an invalidation that is
  // even slightly short leaves a permanent seam, and the exact geometry
  // depends on where the settle happened to stop.
  //
  // Working out precisely which areas the two images left uncovered while
  // one of them is being hidden and the other moved a whole screen width is
  // possible, and it is not worth it: this happens once at the end of a
  // gesture and costs one full repaint that was going to happen anyway.
  lv_obj_invalidate(lv_scr_act());
  // And draw it now, synchronously, rather than marking it and hoping.
  //
  // Marking alone was not enough, and the evidence is unambiguous: with
  // every rectangle LVGL handed to the panel logged, the last flush before
  // this point covered the whole screen and *nothing followed it at all* -
  // no repaint was ever issued for the invalidation above. The panel has no
  // framebuffer to fall back on, so the screen kept the composition from
  // mid-swipe: a stripe down the right edge still showing the screen being
  // swiped away, indefinitely, while the canvas, the object positions and
  // every other piece of state read perfectly healthy.
  //
  // lv_refr_now takes the question out of LVGL's invalidation bookkeeping
  // entirely. It costs one full repaint, ~25ms, which is a repaint this
  // transition needs by definition.
  lv_refr_now(NULL);
}

void CanvasPresenter::present() {
  if (image_ == nullptr) return;
  // The descriptor still points at the same buffer, so LVGL only needs to
  // be told the pixels changed. Invalidating the object (rather than
  // re-setting the source) keeps LVGL's own dirty-area tracking intact, so
  // a later composed overlay isn't forced to redraw with it.
  lv_obj_invalidate(image_);
}
