#pragma once
#include <Arduino.h>
#include <esp_lcd_panel_rgb.h>
#include <esp_lcd_panel_ops.h>

// The RGB panel, owned directly rather than through Arduino_GFX, for one
// reason: a second framebuffer.
//
// Why (2026-09-10). The panel scans its framebuffer continuously at 39Hz,
// 25.6ms a pass, while composing one frame of a follow-the-finger transition
// takes 68ms. The scan therefore crosses the write two and a half times, and
// what reaches the glass is part old frame and part new - with the seam
// between the two screens standing about 50px apart on either side of the
// tear line. On a black-to-green boundary that reads as sawteeth marching
// down the screen, which is exactly what it was reported as.
//
// Nothing about the writing can fix that. The only fix is to never show a
// half-written frame: build it in a buffer nobody is looking at, and hand it
// over between two refreshes. The silicon supports up to three framebuffers
// and the driver switches them on VSYNC. Arduino_GFX hardcodes num_fbs = 1
// with no way to ask for more.
//
// Owning the panel here costs little, because this board used Arduino_GFX for
// only four things - start the panel, fill it once, hand out the framebuffer
// pointer, and blit a bitmap. Every pixel that matters is drawn into a
// ClippedCanvas16 by the shared renderer and arrives here as one block. The
// pin map and the timings were already in main.cpp; the configuration below
// is otherwise the same recipe Arduino_GFX builds, kept deliberately close to
// it so a difference in behaviour cannot be blamed on a rewrite.
//
// Three of them, not two, and that is not greed. The driver switches at the
// next VSYNC but does not wait for it: handing a buffer over returned in 2ms
// where a wait would have cost up to 25.6. So the buffer just released is
// still on the glass for up to one refresh, and drawing into it immediately
// would tear the top of the screen - the same artefact in a smaller band.
// With a third buffer the one we draw into was released two flips ago and
// has certainly been replaced, because composing a frame takes 68ms and a
// refresh is 25.6. No waiting, no tearing, 750KB.
//
// Fewer are accepted if the driver refuses - the interaction between bounce
// buffers and multiple framebuffers is documented nowhere this was written
// from - and begin() says which it got. With one, the board behaves exactly
// as it did before, tearing included, rather than not at all.
class RgbPanel {
public:
  struct Pins {
    int de, vsync, hsync, pclk;
    int r[5];
    int g[6];
    int b[5];
  };

  struct Timing {
    uint32_t pclkHz;
    uint16_t hPulse, hBack, hFront;
    uint16_t vPulse, vBack, vFront;
  };

  bool begin(int16_t w, int16_t h, const Pins &pins, const Timing &t, size_t bounceBufferPx)
  {
    w_ = w;
    h_ = h;

    // Most first, fewest last. Every attempt is reported, because "it works"
    // and "it works with the thing we built this for" are different claims
    // and only one of them is worth having.
    for (size_t want = 3; want >= 1; want--)
    {
      if (create(pins, t, bounceBufferPx, want)) return true;
      Serial.printf("[panel] %u framebuffers refused, trying fewer\n", (unsigned)want);
    }
    return false;
  }

  int16_t width() const { return w_; }
  int16_t height() const { return h_; }
  size_t bufferCount() const { return count_; }

  // The buffer to draw into. With one framebuffer this is also the one being
  // displayed, which is the old behaviour and the old tearing.
  uint16_t *back() { return fb_[backIndex_]; }

  // Hands the finished buffer to the panel and moves on to the next one in
  // the ring. The driver makes the switch at the next VSYNC, so nothing
  // half-written is ever scanned out.
  void flip()
  {
    if (!handle_ || count_ < 2) return;  // nothing to switch to
    esp_lcd_panel_draw_bitmap(handle_, 0, 0, w_, h_, fb_[backIndex_]);
    backIndex_ = (backIndex_ + 1) % count_;
  }

  // One finished picture from elsewhere - a rendered canvas - shown whole.
  void blit(const uint16_t *src)
  {
    if (!handle_ || src == nullptr) return;
    memcpy(back(), src, (size_t)w_ * (size_t)h_ * sizeof(uint16_t));
    flip();
  }

  void fill(uint16_t colour)
  {
    if (!handle_) return;
    const size_t pixels = (size_t)w_ * (size_t)h_;
    const uint8_t hi = (uint8_t)(colour >> 8);
    const uint8_t lo = (uint8_t)(colour & 0xFF);
    // Every buffer, so a flip cannot reveal whatever the allocator left in
    // one of the others.
    for (size_t pass = 0; pass < count_; pass++)
    {
      if (hi == lo) memset(back(), lo, pixels * sizeof(uint16_t));
      else { uint16_t *b = back(); for (size_t i = 0; i < pixels; i++) b[i] = colour; }
      flip();
    }
  }

  String report() const
  {
    String out = "  panel " + String(w_) + "x" + String(h_) + ", " + String(count_) +
                 " framebuffer" + (count_ == 1 ? "" : "s");
    out += count_ >= 3 ? " (no tearing, no waiting)"
                       : (count_ == 2 ? " (no tearing while it keeps up)" : " (tearing)");
    out += "\n";
    return out;
  }

private:
  bool create(const Pins &pins, const Timing &t, size_t bounceBufferPx, size_t numFbs)
  {
    // Field by field rather than a designated initializer: the struct carries
    // nested flag words and a deprecated union, and the order rules for those
    // are a compile error waiting to happen on the next IDF bump.
    esp_lcd_rgb_panel_config_t config = {};
    config.clk_src = LCD_CLK_SRC_DEFAULT;
    config.timings.pclk_hz = t.pclkHz;
    config.timings.h_res = (uint32_t)w_;
    config.timings.v_res = (uint32_t)h_;
    config.timings.hsync_pulse_width = t.hPulse;
    config.timings.hsync_back_porch = t.hBack;
    config.timings.hsync_front_porch = t.hFront;
    config.timings.vsync_pulse_width = t.vPulse;
    config.timings.vsync_back_porch = t.vBack;
    config.timings.vsync_front_porch = t.vFront;
    // Both idle low, as Waveshare's own example and Arduino_GFX's translation
    // of a zero polarity argument have it.
    config.timings.flags.hsync_idle_low = 1;
    config.timings.flags.vsync_idle_low = 1;
    config.timings.flags.de_idle_high = 0;
    config.timings.flags.pclk_active_neg = 1;
    config.timings.flags.pclk_idle_high = 0;

    config.data_width = 16;
    config.bits_per_pixel = 16;
    config.num_fbs = numFbs;
    config.bounce_buffer_size_px = bounceBufferPx;
    config.psram_trans_align = 64;
    config.hsync_gpio_num = pins.hsync;
    config.vsync_gpio_num = pins.vsync;
    config.de_gpio_num = pins.de;
    config.pclk_gpio_num = pins.pclk;
    config.disp_gpio_num = GPIO_NUM_NC;

    // DATA0..DATA15 is B, G, R on this board - the low-endian order
    // Arduino_GFX uses when it is not told otherwise.
    config.data_gpio_nums[0] = pins.b[0];
    config.data_gpio_nums[1] = pins.b[1];
    config.data_gpio_nums[2] = pins.b[2];
    config.data_gpio_nums[3] = pins.b[3];
    config.data_gpio_nums[4] = pins.b[4];
    config.data_gpio_nums[5] = pins.g[0];
    config.data_gpio_nums[6] = pins.g[1];
    config.data_gpio_nums[7] = pins.g[2];
    config.data_gpio_nums[8] = pins.g[3];
    config.data_gpio_nums[9] = pins.g[4];
    config.data_gpio_nums[10] = pins.g[5];
    config.data_gpio_nums[11] = pins.r[0];
    config.data_gpio_nums[12] = pins.r[1];
    config.data_gpio_nums[13] = pins.r[2];
    config.data_gpio_nums[14] = pins.r[3];
    config.data_gpio_nums[15] = pins.r[4];

    config.flags.disp_active_low = true;
    config.flags.refresh_on_demand = false;
    config.flags.fb_in_psram = true;

    esp_err_t err = esp_lcd_new_rgb_panel(&config, &handle_);
    if (err != ESP_OK)
    {
      Serial.printf("[panel] esp_lcd_new_rgb_panel(num_fbs=%u) failed: %s\n", (unsigned)numFbs,
                    esp_err_to_name(err));
      handle_ = nullptr;
      return false;
    }

    err = esp_lcd_panel_reset(handle_);
    if (err == ESP_OK) err = esp_lcd_panel_init(handle_);
    if (err != ESP_OK)
    {
      Serial.printf("[panel] init failed: %s\n", esp_err_to_name(err));
      esp_lcd_panel_del(handle_);
      handle_ = nullptr;
      return false;
    }

    void *fb0 = nullptr, *fb1 = nullptr, *fb2 = nullptr;
    if (numFbs >= 3) err = esp_lcd_rgb_panel_get_frame_buffer(handle_, 3, &fb0, &fb1, &fb2);
    else if (numFbs == 2) err = esp_lcd_rgb_panel_get_frame_buffer(handle_, 2, &fb0, &fb1);
    else err = esp_lcd_rgb_panel_get_frame_buffer(handle_, 1, &fb0);
    if (err != ESP_OK || fb0 == nullptr)
    {
      Serial.printf("[panel] get_frame_buffer failed: %s\n", esp_err_to_name(err));
      esp_lcd_panel_del(handle_);
      handle_ = nullptr;
      return false;
    }

    fb_[0] = (uint16_t *)fb0;
    fb_[1] = (uint16_t *)fb1;
    fb_[2] = (uint16_t *)fb2;
    count_ = numFbs;
    // fb0 is the one the driver starts scanning, so the first frame is drawn
    // into the next one along.
    backIndex_ = count_ > 1 ? 1 : 0;

    Serial.printf("[panel] %u framebuffer(s) at %p %p %p, psram free %u KB\n", (unsigned)numFbs,
                  fb_[0], fb_[1], fb_[2], (unsigned)(ESP.getFreePsram() / 1024));
    return true;
  }

  esp_lcd_panel_handle_t handle_ = nullptr;
  uint16_t *fb_[3] = {nullptr, nullptr, nullptr};
  size_t count_ = 0;
  size_t backIndex_ = 0;
  int16_t w_ = 0, h_ = 0;
};
