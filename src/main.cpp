// First LVGL bring-up: WiFi + MQTT setup screen on the Waveshare
// ESP32-S3-Knob-Touch-LCD-1.8.
//
// Display: ST77916 over QSPI (Arduino_GFX). Touch: CST816 over I2C, read
// with a minimal hand-rolled driver (protocol is simple enough that no
// library dependency is worth adding). Pin reference: see README.md.
//
// Flow: scan + pick a WiFi network, enter its password, connect. Once
// connected, WiFi credentials are saved to NVS (Preferences) so the next
// boot auto-reconnects without asking again, the SSID/password UI is
// replaced with a green-checkmark "<ssid> [edit]" status (tap edit to go
// back and pick a different network), and MQTT broker fields appear
// below it - protocol/host/port on one row, then username/password (icon
// prefixes: LVGL ships no literal user/lock glyphs, so LV_SYMBOL_LIST and
// LV_SYMBOL_EYE_OPEN - visible password - are the closest fits) - the
// same fields as screenbee-m5dial's WiFiSetupServerHTML.h AP-setup page,
// saved to their own NVS namespace on "Save".
//
// The default on-screen keyboard's full-width layout clips against the
// round bezel (its rectangular button grid reaches into the square
// framebuffer's corners, which sit outside the visible circle). Tried a
// custom knob-driven character picker (rotate to scroll, tap to insert)
// as an alternative, but tapping individual letters turned out much
// faster to use than rotating through 94 characters one at a time - so
// text entry uses the standard keyboard after all, just sized down
// (260x150) to fit entirely inside the circle's inscribed safe area
// instead of spanning the full square framebuffer. Whichever field is
// being edited temporarily moves to a shared spot above the keyboard
// (see fieldKeyboardCb) so it stays visible instead of being covered.

#include <Arduino.h>
#include <WiFi.h>
#include <Wire.h>
#include <Preferences.h>
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

static Preferences prefs;

static lv_disp_draw_buf_t draw_buf;
static lv_color_t *lvBuf1;
static lv_obj_t *headerRow;
static lv_obj_t *connCheckLabel;
static lv_obj_t *titleLabel;
static lv_obj_t *connEditBtn;
static lv_obj_t *ssidDropdown;
static lv_obj_t *passwordArea;
static lv_obj_t *connectBtn;
static lv_obj_t *statusLabel;
static lv_obj_t *keyboard;
static lv_obj_t *mqttRow;
static lv_obj_t *mqttProtocolDropdown;
static lv_obj_t *mqttHostArea;
static lv_obj_t *mqttPortArea;
static lv_obj_t *mqttUserArea;
static lv_obj_t *mqttPassArea;
static lv_obj_t *mqttSaveBtn;

static String currentSsid;
static String currentPassword;

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

// Shared long-press-to-edit handler for every text field (WiFi password,
// MQTT host/port/username/password). A short tap still just moves the
// cursor via lv_textarea's own click handling; a long-press is the
// explicit "start editing" gesture that opens the keyboard and moves the
// field to a shared spot above it so it stays visible. On close, the
// field returns to its own resting position (passed in as user_data,
// since different fields rest at different y offsets).
static void fieldKeyboardCb(lv_event_t *e) {
  lv_event_code_t code = lv_event_get_code(e);
  lv_obj_t *ta = lv_event_get_target(e);
  lv_coord_t restY = (lv_coord_t)(intptr_t)lv_event_get_user_data(e);

  if (code == LV_EVENT_LONG_PRESSED) {
    lv_keyboard_set_textarea(keyboard, ta);
    lv_obj_clear_flag(keyboard, LV_OBJ_FLAG_HIDDEN);
    lv_obj_move_foreground(ta);
    lv_obj_align(ta, LV_ALIGN_CENTER, 0, -95);
  } else if (code == LV_EVENT_DEFOCUSED || code == LV_EVENT_READY || code == LV_EVENT_CANCEL) {
    lv_obj_add_flag(keyboard, LV_OBJ_FLAG_HIDDEN);
    lv_obj_align(ta, LV_ALIGN_TOP_MID, 0, restY);
  }
}

// Same idea as fieldKeyboardCb, but for mqttHostArea/mqttPortArea, which
// live inside the mqttRow flex container - a manual lv_obj_align there
// would fight the flex layout engine's own positioning and likely get
// clipped to the (small, auto-sized) row. Reparenting to the screen for
// the duration of the edit sidesteps that; flex recomputes its position
// automatically when it's reparented back into mqttRow on close.
static void mqttRowFieldKeyboardCb(lv_event_t *e) {
  lv_event_code_t code = lv_event_get_code(e);
  lv_obj_t *ta = lv_event_get_target(e);

  if (code == LV_EVENT_LONG_PRESSED) {
    lv_keyboard_set_textarea(keyboard, ta);
    lv_obj_clear_flag(keyboard, LV_OBJ_FLAG_HIDDEN);
    lv_obj_set_parent(ta, lv_scr_act());
    lv_obj_move_foreground(ta);
    lv_obj_align(ta, LV_ALIGN_CENTER, 0, -95);
  } else if (code == LV_EVENT_DEFOCUSED || code == LV_EVENT_READY || code == LV_EVENT_CANCEL) {
    lv_obj_add_flag(keyboard, LV_OBJ_FLAG_HIDDEN);
    lv_obj_set_parent(ta, mqttRow);
  }
}

static void saveWifiCredentials(const String &ssid, const String &password) {
  prefs.begin("wifi", false);
  prefs.putString("ssid", ssid);
  prefs.putString("pass", password);
  prefs.end();
  Serial.println("WiFi credentials saved");
}

static bool loadWifiCredentials(String &ssid, String &password) {
  prefs.begin("wifi", true);
  ssid = prefs.getString("ssid", "");
  password = prefs.getString("pass", "");
  prefs.end();
  return ssid.length() > 0;
}

static void loadMqttConfigIntoFields() {
  prefs.begin("mqtt", true);
  String protocol = prefs.getString("protocol", "mqtt");
  String host = prefs.getString("host", "");
  String port = prefs.getString("port", "");
  String user = prefs.getString("user", "");
  String pass = prefs.getString("pass", "");
  prefs.end();

  lv_dropdown_set_selected(mqttProtocolDropdown, protocol == "mqtts" ? 1 : 0);
  lv_textarea_set_text(mqttHostArea, host.c_str());
  lv_textarea_set_text(mqttPortArea, port.c_str());
  lv_textarea_set_text(mqttUserArea, user.c_str());
  lv_textarea_set_text(mqttPassArea, pass.c_str());
}

static void mqttSaveBtnEventCb(lv_event_t *e) {
  char protocol[8];
  lv_dropdown_get_selected_str(mqttProtocolDropdown, protocol, sizeof(protocol));

  prefs.begin("mqtt", false);
  prefs.putString("protocol", protocol);
  prefs.putString("host", lv_textarea_get_text(mqttHostArea));
  prefs.putString("port", lv_textarea_get_text(mqttPortArea));
  prefs.putString("user", lv_textarea_get_text(mqttUserArea));
  prefs.putString("pass", lv_textarea_get_text(mqttPassArea));
  prefs.end();

  lv_label_set_text(statusLabel, "MQTT settings saved");
  Serial.println("MQTT config saved");
}

static void populateSsidDropdown(int networkCount) {
  if (networkCount <= 0) {
    lv_dropdown_set_options(ssidDropdown, "(no networks found)");
    return;
  }
  String options;
  for (int i = 0; i < networkCount; i++) {
    options += WiFi.SSID(i);
    if (i < networkCount - 1) options += "\n";
  }
  lv_dropdown_set_options(ssidDropdown, options.c_str());
}

// Shows the WiFi setup form again (dropdown/password/connect), hides the
// "connected" status and MQTT fields. Reached via the edit button next to
// the green checkmark - lets the user switch to a different network
// without needing to erase saved credentials first.
static void switchToSetupUi() {
  lv_obj_add_flag(connCheckLabel, LV_OBJ_FLAG_HIDDEN);
  lv_obj_add_flag(connEditBtn, LV_OBJ_FLAG_HIDDEN);
  lv_label_set_text(titleLabel, "ScreenBee WiFi Setup");

  lv_obj_add_flag(mqttRow, LV_OBJ_FLAG_HIDDEN);
  lv_obj_add_flag(mqttUserArea, LV_OBJ_FLAG_HIDDEN);
  lv_obj_add_flag(mqttPassArea, LV_OBJ_FLAG_HIDDEN);
  lv_obj_add_flag(mqttSaveBtn, LV_OBJ_FLAG_HIDDEN);

  lv_obj_clear_flag(ssidDropdown, LV_OBJ_FLAG_HIDDEN);
  lv_obj_clear_flag(passwordArea, LV_OBJ_FLAG_HIDDEN);
  lv_obj_clear_flag(connectBtn, LV_OBJ_FLAG_HIDDEN);

  lv_obj_align(statusLabel, LV_ALIGN_TOP_MID, 0, 225);
  lv_label_set_text(statusLabel, "");

  Serial.println("Re-scanning WiFi networks...");
  int networkCount = WiFi.scanNetworks();
  populateSsidDropdown(networkCount);

  // Restore the previously-entered password (and reselect the current
  // network if it's still in range) instead of leaving the fields blank -
  // editing is meant to change to a *different* network, not retype one
  // that's already connected.
  lv_textarea_set_text(passwordArea, currentPassword.c_str());
  for (int i = 0; i < networkCount; i++) {
    if (WiFi.SSID(i) == currentSsid) {
      lv_dropdown_set_selected(ssidDropdown, i);
      break;
    }
  }
}

static void editBtnEventCb(lv_event_t *e) {
  switchToSetupUi();
}

// Hides the SSID/password setup widgets, shows a green checkmark + ssid
// + edit button, and the MQTT broker fields below it (pre-filled from
// NVS if previously saved).
static void switchToConnectedUi(const String &ssid) {
  lv_obj_add_flag(ssidDropdown, LV_OBJ_FLAG_HIDDEN);
  lv_obj_add_flag(passwordArea, LV_OBJ_FLAG_HIDDEN);
  lv_obj_add_flag(connectBtn, LV_OBJ_FLAG_HIDDEN);

  lv_label_set_text(titleLabel, ssid.c_str());
  lv_obj_clear_flag(connCheckLabel, LV_OBJ_FLAG_HIDDEN);
  lv_obj_clear_flag(connEditBtn, LV_OBJ_FLAG_HIDDEN);

  lv_obj_clear_flag(mqttRow, LV_OBJ_FLAG_HIDDEN);
  lv_obj_clear_flag(mqttUserArea, LV_OBJ_FLAG_HIDDEN);
  lv_obj_clear_flag(mqttPassArea, LV_OBJ_FLAG_HIDDEN);
  lv_obj_clear_flag(mqttSaveBtn, LV_OBJ_FLAG_HIDDEN);

  lv_obj_align(statusLabel, LV_ALIGN_TOP_MID, 0, 260);
  lv_label_set_text(statusLabel, "");

  loadMqttConfigIntoFields();
}

static bool wifiConnecting = false;
static uint32_t wifiConnectStart = 0;
static const uint32_t WIFI_CONNECT_TIMEOUT_MS = 15000;

static void connectBtnEventCb(lv_event_t *e) {
  char ssid[64];
  lv_dropdown_get_selected_str(ssidDropdown, ssid, sizeof(ssid));
  currentSsid = ssid;
  currentPassword = lv_textarea_get_text(passwordArea);

  lv_label_set_text_fmt(statusLabel, "Connecting to %s...", ssid);
  WiFi.begin(ssid, currentPassword.c_str());
  wifiConnecting = true;
  wifiConnectStart = millis();
}

// Polled from loop() - WiFi.begin() is async, the actual result (success,
// wrong password, AP out of range, ...) only shows up in WiFi.status()
// some time later.
static void pollWifiConnect() {
  if (!wifiConnecting) return;

  if (WiFi.status() == WL_CONNECTED) {
    Serial.printf("WiFi connected, IP=%s\n", WiFi.localIP().toString().c_str());
    saveWifiCredentials(currentSsid, currentPassword);
    switchToConnectedUi(currentSsid);
    wifiConnecting = false;
  } else if (millis() - wifiConnectStart > WIFI_CONNECT_TIMEOUT_MS) {
    lv_label_set_text(statusLabel, "Connection failed (timeout)");
    Serial.println("WiFi connect timed out");
    wifiConnecting = false;
  }
}

static void buildUi(int networkCount) {
  lv_obj_t *scr = lv_scr_act();

  // Header row: green checkmark + ssid/title + edit button. In setup
  // mode only the title ("ScreenBee WiFi Setup") is visible; once
  // connected the checkmark and edit button appear and the title becomes
  // the ssid (see switchToConnectedUi/switchToSetupUi).
  headerRow = lv_obj_create(scr);
  lv_obj_remove_style_all(headerRow);
  lv_obj_set_size(headerRow, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
  lv_obj_clear_flag(headerRow, LV_OBJ_FLAG_SCROLLABLE);
  lv_obj_set_flex_flow(headerRow, LV_FLEX_FLOW_ROW);
  lv_obj_set_flex_align(headerRow, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
  lv_obj_set_style_pad_column(headerRow, 6, 0);
  lv_obj_align(headerRow, LV_ALIGN_TOP_MID, 0, 30);

  connCheckLabel = lv_label_create(headerRow);
  lv_label_set_text(connCheckLabel, LV_SYMBOL_OK);
  lv_obj_set_style_text_color(connCheckLabel, lv_palette_main(LV_PALETTE_GREEN), 0);
  lv_obj_add_flag(connCheckLabel, LV_OBJ_FLAG_HIDDEN);

  titleLabel = lv_label_create(headerRow);
  lv_label_set_text(titleLabel, "ScreenBee WiFi Setup");

  connEditBtn = lv_btn_create(headerRow);
  lv_obj_set_size(connEditBtn, 34, 34);
  lv_obj_set_style_pad_all(connEditBtn, 2, 0);  // default button padding was clipping the icon
  lv_obj_add_event_cb(connEditBtn, editBtnEventCb, LV_EVENT_CLICKED, NULL);
  lv_obj_t *editIcon = lv_label_create(connEditBtn);
  lv_label_set_text(editIcon, LV_SYMBOL_EDIT);
  lv_obj_center(editIcon);
  lv_obj_add_flag(connEditBtn, LV_OBJ_FLAG_HIDDEN);

  ssidDropdown = lv_dropdown_create(scr);
  lv_obj_set_width(ssidDropdown, 260);
  lv_obj_align(ssidDropdown, LV_ALIGN_TOP_MID, 0, 80);
  populateSsidDropdown(networkCount);

  passwordArea = lv_textarea_create(scr);
  lv_obj_set_width(passwordArea, 260);
  lv_textarea_set_one_line(passwordArea, true);
  lv_textarea_set_password_mode(passwordArea, false);  // visible on purpose
  lv_textarea_set_placeholder_text(passwordArea, "Password");
  lv_obj_align(passwordArea, LV_ALIGN_TOP_MID, 0, 130);
  lv_obj_add_event_cb(passwordArea, fieldKeyboardCb, LV_EVENT_ALL, (void *)(intptr_t)130);

  connectBtn = lv_btn_create(scr);
  lv_obj_align(connectBtn, LV_ALIGN_TOP_MID, 0, 180);
  lv_obj_add_event_cb(connectBtn, connectBtnEventCb, LV_EVENT_CLICKED, NULL);
  lv_obj_t *btnLabel = lv_label_create(connectBtn);
  lv_label_set_text(btnLabel, "Connect");

  statusLabel = lv_label_create(scr);
  lv_label_set_text(statusLabel, "");
  lv_obj_align(statusLabel, LV_ALIGN_TOP_MID, 0, 225);

  // MQTT broker fields (same set as screenbee-m5dial's AP-setup HTML
  // page: protocol/host/port/username/password) - built now but hidden
  // until switchToConnectedUi() reveals them.
  mqttRow = lv_obj_create(scr);
  lv_obj_remove_style_all(mqttRow);
  lv_obj_set_size(mqttRow, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
  lv_obj_clear_flag(mqttRow, LV_OBJ_FLAG_SCROLLABLE);
  lv_obj_set_flex_flow(mqttRow, LV_FLEX_FLOW_ROW);
  lv_obj_set_flex_align(mqttRow, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
  lv_obj_set_style_pad_column(mqttRow, 4, 0);
  lv_obj_align(mqttRow, LV_ALIGN_TOP_MID, 0, 80);
  lv_obj_add_flag(mqttRow, LV_OBJ_FLAG_HIDDEN);

  mqttProtocolDropdown = lv_dropdown_create(mqttRow);
  lv_dropdown_set_options(mqttProtocolDropdown, "mqtt://\nmqtts://");
  lv_obj_set_width(mqttProtocolDropdown, 82);

  mqttHostArea = lv_textarea_create(mqttRow);
  lv_obj_set_width(mqttHostArea, 118);
  lv_textarea_set_one_line(mqttHostArea, true);
  lv_textarea_set_placeholder_text(mqttHostArea, "192.168.1.107");
  lv_obj_add_event_cb(mqttHostArea, mqttRowFieldKeyboardCb, LV_EVENT_ALL, NULL);

  mqttPortArea = lv_textarea_create(mqttRow);
  lv_obj_set_width(mqttPortArea, 55);
  lv_textarea_set_one_line(mqttPortArea, true);
  lv_textarea_set_placeholder_text(mqttPortArea, "1883");
  lv_obj_add_event_cb(mqttPortArea, mqttRowFieldKeyboardCb, LV_EVENT_ALL, NULL);

  mqttUserArea = lv_textarea_create(scr);
  lv_obj_set_width(mqttUserArea, 240);
  lv_textarea_set_one_line(mqttUserArea, true);
  // LVGL's built-in symbol set has no literal "person" icon - LV_SYMBOL_LIST
  // is the closest available stand-in.
  lv_textarea_set_placeholder_text(mqttUserArea, LV_SYMBOL_LIST " Username (optional)");
  lv_obj_align(mqttUserArea, LV_ALIGN_TOP_MID, 0, 130);
  lv_obj_add_event_cb(mqttUserArea, fieldKeyboardCb, LV_EVENT_ALL, (void *)(intptr_t)130);
  lv_obj_add_flag(mqttUserArea, LV_OBJ_FLAG_HIDDEN);

  mqttPassArea = lv_textarea_create(scr);
  lv_obj_set_width(mqttPassArea, 240);
  lv_textarea_set_one_line(mqttPassArea, true);
  lv_textarea_set_password_mode(mqttPassArea, false);  // visible, same as WiFi password
  // No lock icon in LVGL's default set either - EYE_OPEN doubles as a
  // "this password is shown in the open" indicator, which is accurate here.
  lv_textarea_set_placeholder_text(mqttPassArea, LV_SYMBOL_EYE_OPEN " Password (optional)");
  lv_obj_align(mqttPassArea, LV_ALIGN_TOP_MID, 0, 175);
  lv_obj_add_event_cb(mqttPassArea, fieldKeyboardCb, LV_EVENT_ALL, (void *)(intptr_t)175);
  lv_obj_add_flag(mqttPassArea, LV_OBJ_FLAG_HIDDEN);

  mqttSaveBtn = lv_btn_create(scr);
  lv_obj_align(mqttSaveBtn, LV_ALIGN_TOP_MID, 0, 220);
  lv_obj_add_event_cb(mqttSaveBtn, mqttSaveBtnEventCb, LV_EVENT_CLICKED, NULL);
  lv_label_set_text(lv_label_create(mqttSaveBtn), "Save MQTT");
  lv_obj_add_flag(mqttSaveBtn, LV_OBJ_FLAG_HIDDEN);

  // Sized and positioned to fit inside the round bezel's inscribed safe
  // area (260x150, centered 45px below screen center - farthest corner
  // is ~177px from center, just inside the 180px radius) with room left
  // above it, at y=-95, for whichever field is being edited so it stays
  // visible instead of being covered by the keyboard.
  keyboard = lv_keyboard_create(scr);
  lv_obj_set_size(keyboard, 260, 150);
  lv_obj_align(keyboard, LV_ALIGN_CENTER, 0, 45);
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

  WiFi.mode(WIFI_STA);

  String savedSsid, savedPassword;
  bool haveSavedCreds = loadWifiCredentials(savedSsid, savedPassword);
  bool autoConnected = false;
  if (haveSavedCreds) {
    Serial.printf("Trying saved WiFi: %s\n", savedSsid.c_str());
    WiFi.begin(savedSsid.c_str(), savedPassword.c_str());
    uint32_t start = millis();
    while (millis() - start < 8000 && WiFi.status() != WL_CONNECTED) {
      delay(200);
    }
    autoConnected = (WiFi.status() == WL_CONNECTED);
    Serial.printf("Auto-reconnect %s\n", autoConnected ? "succeeded" : "failed");
  }

  int networkCount = 0;
  if (!autoConnected) {
    Serial.println("Scanning WiFi networks...");
    networkCount = WiFi.scanNetworks();
    Serial.printf("Found %d network(s)\n", networkCount);
  }

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
  // Default long-press time is 400ms; real holds measured on this touch
  // panel were released around ~225ms (short, imprecise touch reporting),
  // never crossing that threshold and never firing LV_EVENT_LONG_PRESSED.
  // 200ms is comfortably under a deliberate hold but still distinguishes
  // it from a quick cursor-positioning tap.
  indev_drv.long_press_time = 200;
  lv_indev_drv_register(&indev_drv);

  buildUi(networkCount);

  if (autoConnected) {
    currentSsid = savedSsid;
    switchToConnectedUi(savedSsid);
  }

  Serial.println("LVGL UI ready.");
}

void loop() {
  pollWifiConnect();

  // LV_TICK_CUSTOM (LVGL calling Arduino's millis() from a .c translation
  // unit) never worked in this build - lv_tick_get() stayed at 0 even after
  // a full clean rebuild. Driving the tick manually sidesteps that entirely.
  lv_tick_inc(5);
  lv_timer_handler();
  delay(5);
}
