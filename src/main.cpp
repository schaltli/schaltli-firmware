// First LVGL bring-up: WiFi setup screen (SSID dropdown + plaintext
// password field) on the Waveshare ESP32-S3-Knob-Touch-LCD-1.8.
//
// Display: ST77916 over QSPI (Arduino_GFX). Touch: CST816 over I2C, read
// with a minimal hand-rolled driver (protocol is simple enough that no
// library dependency is worth adding). Pin reference: see README.md.
//
// This is deliberately touch-only for now - no rotary-encoder LVGL input
// device yet. The encoder push-button wiring (shared with the quadrature
// signal lines, see the schematic notes in the project memory) needs its
// own investigation before it's worth wiring into LVGL's encoder indev.

#include <Arduino.h>
#include <WiFi.h>
#include <Wire.h>
#include <Arduino_GFX_Library.h>
#include <lvgl.h>
#include "driver/rtc_io.h"

// LCD (ST77916, QSPI)
static const int8_t LCD_CS = 14;
static const int8_t LCD_SCK = 13;
static const int8_t LCD_D0 = 15;
static const int8_t LCD_D1 = 16;
static const int8_t LCD_D2 = 17;
static const int8_t LCD_D3 = 18;
static const int8_t LCD_RST = 21;
static const uint8_t LCD_BL = 47;

// Touch (CST816, I2C)
static const int8_t TOUCH_SDA = 11;
static const int8_t TOUCH_SCL = 12;
static const int8_t TOUCH_RST = 10;
static const int8_t TOUCH_INT = 9;
static const uint8_t CST816_ADDR = 0x15;

static const uint16_t SCREEN_SIZE = 360;

Arduino_DataBus *bus = new Arduino_ESP32QSPI(LCD_CS, LCD_SCK, LCD_D0, LCD_D1, LCD_D2, LCD_D3);
Arduino_GFX *gfx = new Arduino_ST77916(
    bus, LCD_RST, 0 /* rotation */, true /* IPS */, SCREEN_SIZE, SCREEN_SIZE,
    0, 0, 0, 0, st77916_150_init_operations, sizeof(st77916_150_init_operations));

static lv_disp_draw_buf_t draw_buf;
static lv_color_t *lvBuf1;
static lv_obj_t *ssidDropdown;
static lv_obj_t *passwordArea;
static lv_obj_t *keyboard;
static lv_obj_t *statusLabel;

// Matches the official Waveshare demo (cst816.cpp/Touch_Init): no reset
// pin is ever driven - just switch the chip to normal mode over I2C.
static void cst816Init() {
  uint8_t normalMode = 0x00;
  Wire.beginTransmission(CST816_ADDR);
  Wire.write(0x00);
  Wire.write(normalMode);
  Wire.endTransmission(true);
}

// CST816 register map: read 7 bytes from 0x00. Bytes 3-4 = X, bytes 5-6 =
// Y (matches the official Waveshare demo). Byte 2 is documented there as
// "finger count" but stayed 0 during clearly-active drags on this unit -
// byte 1 (0 when idle, nonzero while touched) is what's actually reliable.
static bool cst816Read(int16_t *x, int16_t *y) {
  Wire.beginTransmission(CST816_ADDR);
  Wire.write(0x00);
  if (Wire.endTransmission(false) != 0) return false;
  if (Wire.requestFrom((int)CST816_ADDR, 7) != 7) return false;

  uint8_t buf[7];
  for (uint8_t &b : buf) b = Wire.read();

  // Empirically buf[1] (register 0x01) is 0 when idle and nonzero while
  // touched - more reliable in practice than buf[2] ("finger count" per
  // the official demo, which stayed 0 during clearly-active drags here).
  if (buf[1] == 0) return false;

  *x = ((buf[3] & 0x0F) << 8) | buf[4];
  *y = ((buf[5] & 0x0F) << 8) | buf[6];
  return true;
}

static void touchReadCb(lv_indev_drv_t *drv, lv_indev_data_t *data) {
  int16_t x, y;
  if (cst816Read(&x, &y)) {
    data->state = LV_INDEV_STATE_PRESSED;
    data->point.x = x;
    data->point.y = y;
  } else {
    data->state = LV_INDEV_STATE_RELEASED;
  }
}

static void dispFlushCb(lv_disp_drv_t *disp, const lv_area_t *area, lv_color_t *color_p) {
  uint32_t w = area->x2 - area->x1 + 1;
  uint32_t h = area->y2 - area->y1 + 1;
  gfx->draw16bitRGBBitmap(area->x1, area->y1, (uint16_t *)color_p, w, h);
  lv_disp_flush_ready(disp);
}

static void passwordEventCb(lv_event_t *e) {
  lv_event_code_t code = lv_event_get_code(e);
  if (code == LV_EVENT_FOCUSED) {
    lv_keyboard_set_textarea(keyboard, passwordArea);
    lv_obj_clear_flag(keyboard, LV_OBJ_FLAG_HIDDEN);
  } else if (code == LV_EVENT_DEFOCUSED || code == LV_EVENT_READY || code == LV_EVENT_CANCEL) {
    lv_obj_add_flag(keyboard, LV_OBJ_FLAG_HIDDEN);
  }
}

static void connectBtnEventCb(lv_event_t *e) {
  char ssid[64];
  lv_dropdown_get_selected_str(ssidDropdown, ssid, sizeof(ssid));
  const char *password = lv_textarea_get_text(passwordArea);

  lv_label_set_text_fmt(statusLabel, "Connecting to %s...", ssid);
  WiFi.begin(ssid, password);
}

static void buildUi(int networkCount) {
  lv_obj_t *scr = lv_scr_act();

  lv_obj_t *title = lv_label_create(scr);
  lv_label_set_text(title, "ScreenBee WiFi Setup");
  lv_obj_align(title, LV_ALIGN_TOP_MID, 0, 30);

  ssidDropdown = lv_dropdown_create(scr);
  lv_obj_set_width(ssidDropdown, 260);
  lv_obj_align(ssidDropdown, LV_ALIGN_TOP_MID, 0, 80);

  if (networkCount <= 0) {
    lv_dropdown_set_options(ssidDropdown, "(no networks found)");
  } else {
    String options;
    for (int i = 0; i < networkCount; i++) {
      options += WiFi.SSID(i);
      if (i < networkCount - 1) options += "\n";
    }
    lv_dropdown_set_options(ssidDropdown, options.c_str());
  }

  passwordArea = lv_textarea_create(scr);
  lv_obj_set_width(passwordArea, 260);
  lv_textarea_set_one_line(passwordArea, true);
  lv_textarea_set_password_mode(passwordArea, false);  // visible on purpose
  lv_textarea_set_placeholder_text(passwordArea, "Password");
  lv_obj_align(passwordArea, LV_ALIGN_TOP_MID, 0, 130);
  lv_obj_add_event_cb(passwordArea, passwordEventCb, LV_EVENT_ALL, NULL);

  lv_obj_t *connectBtn = lv_btn_create(scr);
  lv_obj_align(connectBtn, LV_ALIGN_TOP_MID, 0, 180);
  lv_obj_add_event_cb(connectBtn, connectBtnEventCb, LV_EVENT_CLICKED, NULL);
  lv_obj_t *btnLabel = lv_label_create(connectBtn);
  lv_label_set_text(btnLabel, "Connect");

  statusLabel = lv_label_create(scr);
  lv_label_set_text(statusLabel, "");
  lv_obj_align(statusLabel, LV_ALIGN_TOP_MID, 0, 225);

  keyboard = lv_keyboard_create(scr);
  lv_obj_add_flag(keyboard, LV_OBJ_FLAG_HIDDEN);
}

void setup() {
  Serial.begin(115200);
  delay(500);

  // A previous deep-sleep experiment latched LCD_RST/TOUCH_RST low via
  // rtc_gpio_hold_en() - that hold survives normal resets/reflashes, so
  // release it before gfx->begin() tries to pulse the display out of reset.
  rtc_gpio_hold_dis(GPIO_NUM_21);
  rtc_gpio_hold_dis(GPIO_NUM_10);
  gpio_deep_sleep_hold_dis();

  pinMode(LCD_BL, OUTPUT);
  digitalWrite(LCD_BL, HIGH);

  Wire.begin(TOUCH_SDA, TOUCH_SCL);
  cst816Init();

  bool gfxOk = gfx->begin(40000000);
  Serial.printf("gfx->begin() = %s\n", gfxOk ? "true" : "false");
  gfx->fillScreen(RGB565_BLACK);

  Serial.println("Scanning WiFi networks...");
  WiFi.mode(WIFI_STA);
  int networkCount = WiFi.scanNetworks();
  Serial.printf("Found %d network(s)\n", networkCount);

  lv_init();

  size_t bufPixels = SCREEN_SIZE * 60;
  lvBuf1 = (lv_color_t *)heap_caps_malloc(bufPixels * sizeof(lv_color_t), MALLOC_CAP_SPIRAM);
  lv_disp_draw_buf_init(&draw_buf, lvBuf1, NULL, bufPixels);

  static lv_disp_drv_t disp_drv;
  lv_disp_drv_init(&disp_drv);
  disp_drv.hor_res = SCREEN_SIZE;
  disp_drv.ver_res = SCREEN_SIZE;
  disp_drv.flush_cb = dispFlushCb;
  disp_drv.draw_buf = &draw_buf;
  lv_disp_drv_register(&disp_drv);

  static lv_indev_drv_t indev_drv;
  lv_indev_drv_init(&indev_drv);
  indev_drv.type = LV_INDEV_TYPE_POINTER;
  indev_drv.read_cb = touchReadCb;
  lv_indev_drv_register(&indev_drv);

  buildUi(networkCount);

  Serial.println("LVGL UI ready.");
}

void loop() {
  // LV_TICK_CUSTOM (LVGL calling Arduino's millis() from a .c translation
  // unit) never worked in this build - lv_tick_get() stayed at 0 even after
  // a full clean rebuild. Driving the tick manually sidesteps that entirely.
  lv_tick_inc(5);
  lv_timer_handler();
  delay(5);
}
