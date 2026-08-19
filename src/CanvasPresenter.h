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

private:
  uint16_t width_;
  uint16_t height_;
  ClippedCanvas16* canvas_ = nullptr;
  lv_img_dsc_t descriptor_ = {};
  lv_obj_t* image_ = nullptr;
};
