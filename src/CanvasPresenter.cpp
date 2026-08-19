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

  image_ = lv_img_create(lv_scr_act());
  lv_img_set_src(image_, &descriptor_);
  lv_obj_align(image_, LV_ALIGN_CENTER, 0, 0);
  return true;
}

void CanvasPresenter::present() {
  if (image_ == nullptr) return;
  // The descriptor still points at the same buffer, so LVGL only needs to
  // be told the pixels changed. Invalidating the object (rather than
  // re-setting the source) keeps LVGL's own dirty-area tracking intact, so
  // a later composed overlay isn't forced to redraw with it.
  lv_obj_invalidate(image_);
}
