// ScreenBee firmware for the Waveshare ESP32-S3-Knob-Touch-LCD-1.8.
//
// Boot flow: bring up the panel and LVGL, then try saved WiFi. On success
// the device connects to MQTT, loads whatever project is installed, renders
// it, and serves the test interface. With no usable credentials it raises
// its own access point and serves the setup portal instead.
//
// Setup is a browser captive portal on the device's own AP, NOT an on-screen
// keyboard. This board *did* have a working touch keyboard (built and
// hardware-verified 2026-08-18, see the git history) and it was deliberately
// removed on 2026-08-19: text entry is only ever needed at setup time, and a
// browser form needs no LVGL, no touch and no encoder logic, so the same
// flow works unchanged on every ScreenBee device instead of each board
// growing its own input UI. The cost is one more hop for the user; the gain
// is one implementation instead of N.
//
// LVGL stays, but only as a compositor: ColorScreenRenderer draws
// pixel-perfect into its own RGB565 canvas exactly as on the M5 Dial, and
// LVGL just shows that buffer (see CanvasPresenter). That keeps animated
// backgrounds and screen transitions composable later without touching the
// renderer.
//
// Board quirks that still bite, in case they resurface: the ST77916 needs
// the *150* init table (not the library default), a stale
// rtc_gpio_hold_en() from an old deep-sleep experiment can latch LCD/touch
// reset low across reflashes, the CST816 needs no reset pulse but does need
// one write of 0x00 to register 0x00, and LV_TICK_CUSTOM never worked here
// so the tick is driven manually from loop().

#include <Arduino.h>
#include <WiFi.h>
#include <Wire.h>
#include <LittleFS.h>
#include <Arduino_GFX_Library.h>
#include <lvgl.h>
#include <U8g2_for_Adafruit_GFX.h>
#include "driver/rtc_io.h"

#include "CanvasPresenter.h"
#include "CanvasDisplayAdapter.h"
#include "NvsConfigStorage.h"
#include "TestInterfaceServer.h"
#include "WiFiSetupServer.h"
#include "MqttClient.h"
#include "DeployManager.h"
#include "DeviceInfo.h"
#include "project/ProjectLoader.h"
#include "project/ColorScreenRenderer.h"
#include "project/ColorAssetLoader.h"
#include "ScreenNavigatorOverlay.h"
#include "RotaryEncoderReader.h"

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

static CanvasPresenter canvasPresenter(SCREEN_SIZE, SCREEN_SIZE);
static CanvasDisplayAdapter displayAdapter(canvasPresenter);
static NvsConfigStorage configStorage;
static WiFiSetupServer wifiSetupServer(displayAdapter, configStorage);
static TestInterfaceServer testInterface;
static ProjectLoader projectLoader;
static ColorScreenRenderer *screenRenderer = nullptr;
static MqttClient mqttClient;
static DeployManager deployManager;

// Constructed in setup(), not here: it needs the canvas, which does not
// exist until CanvasPresenter::begin() has allocated it.
static ScreenNavigatorOverlay *screenMenu = nullptr;

static int currentScreenIndex = 0;
static bool topicsSubscribed = false;
static bool setupModeActive = false;
// Deploy work is deferred out of the MQTT callback - see onMqttMessage().
static bool pendingDeploy = false;
static String pendingDeployPayload;

static bool renderScreenIndex(int index);
static void dispatchInput(const char *buttonId);

// ---------------------------------------------------------------- display

// Matches the official Waveshare demo (cst816.cpp/Touch_Init): no reset pin
// is ever driven - just switch the chip to normal mode over I2C. Without
// that one write the chip ACKs and returns an all-zero block forever, which
// looks exactly like a dead sensor.
static void cst816Init() {
  Wire.beginTransmission(CST816_ADDR);
  Wire.write(0x00);
  Wire.write((uint8_t)0x00);
  Wire.endTransmission(true);
}

// CST816 register map: read 7 bytes from 0x00. Bytes 3-4 = X, 5-6 = Y.
// Byte 2 is documented as "finger count" but stayed 0 during clearly-active
// drags on this unit; byte 1 (0 idle, nonzero touched) is what is reliable.
static bool cst816Read(int16_t *x, int16_t *y) {
  Wire.beginTransmission(CST816_ADDR);
  Wire.write(0x00);
  if (Wire.endTransmission(false) != 0) return false;
  if (Wire.requestFrom((int)CST816_ADDR, 7) != 7) return false;

  uint8_t buf[7];
  for (uint8_t &b : buf) b = Wire.read();
  if (buf[1] == 0) return false;

  *x = ((buf[3] & 0x0F) << 8) | buf[4];
  *y = ((buf[5] & 0x0F) << 8) | buf[6];
  return true;
}

// The CST816 is read exactly ONCE per loop, into these, and both consumers -
// LVGL's input device and the gesture detector - work from the result.
//
// They must not both call cst816Read(): reading the chip clears its touch
// state, so whichever ran first consumed the event and the other saw an idle
// panel. That is why gestures produced nothing at all while LVGL's own touch
// kept working - lv_timer_handler() simply ran first every iteration.
static bool touchIsDown = false;
static int16_t touchX = 0, touchY = 0;

static void sampleTouch() {
  int16_t x, y;
  if (cst816Read(&x, &y)) {
    touchIsDown = true;
    touchX = x;
    touchY = y;
  } else {
    touchIsDown = false;
  }
}

static void touchReadCb(lv_indev_drv_t *drv, lv_indev_data_t *data) {
  if (touchIsDown) {
    data->state = LV_INDEV_STATE_PRESSED;
    data->point.x = touchX;
    data->point.y = touchY;
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

// ---------------------------------------------------------------- project

// Always a complete render, never anything depending on what was on screen
// before, so repeated snapshots of one index are byte-stable - the property
// pixel-diffing against the designer rests on.
static bool renderScreenIndex(int index) {
  if (!projectLoader.isLoaded() || screenRenderer == nullptr) return false;
  const ProjectConfig &project = projectLoader.getProject();
  if (index < 0 || index >= (int)project.screens.size()) return false;
  if (!screenRenderer->renderScreen(index)) return false;
  currentScreenIndex = index;
  canvasPresenter.present();
  // present() only marks the area dirty; pump LVGL so an HTTP caller that
  // snapshots straight after POST /api/screen sees the new frame rather than
  // the previous one - a race that would look exactly like a rendering bug.
  lv_timer_handler();
  return true;
}

// ----------------------------------------------------------- input actions

// One dispatcher for every named input - the knob's two directions, the four
// swipes, and (later) SoftwareButton taps. They are all just button ids
// carrying an action, which is the whole point of modelling them that way
// rather than inventing a second binding system: physical or gesture, the
// resolution, master-screen inheritance and export baking are already shared.
//
// An empty type means nothing is configured, which is a normal state, not an
// error - most screens bind only some of their inputs.
static void dispatchButtonAction(const ButtonAction &action) {
  if (action.type.isEmpty() || !projectLoader.isLoaded()) return;
  const ProjectConfig &project = projectLoader.getProject();
  int count = (int)project.screens.size();

  if (action.type == "next-screen" || action.type == "previous-screen") {
    if (count == 0) return;
    int delta = (action.type == "next-screen") ? 1 : -1;
    renderScreenIndex(((currentScreenIndex + delta) % count + count) % count);
  } else if (action.type == "goto-screen") {
    for (int i = 0; i < count; i++) {
      if (project.screens[i].id == action.targetScreenId) {
        renderScreenIndex(i);
        break;
      }
    }
  } else if (action.type == "send-mqtt") {
    // What the knob is actually for on this board: turning adjusts a value
    // (temperature, fan, brightness), bound per screen, rather than paging
    // through screens the way the M5 Dial's encoder does.
    mqttClient.publish(action.mqttTopic, action.mqttMessage, false);
  } else if (action.type == "device-action") {
    // Actions the *device* offers and the designer only names - declared in
    // this board's DDF as deviceActions. Unknown ids are skipped and logged,
    // never an error: a project may legitimately have been built against a
    // newer device that offers more of them.
    if (action.deviceActionId == "showScreenMenu") {
      if (screenMenu) screenMenu->trigger(currentScreenIndex);
    } else {
      Serial.printf("[action] unknown device action \"%s\" - skipping\n", action.deviceActionId.c_str());
    }
  } else if (action.type == "goto-setup-mode") {
    setupModeActive = true;
    wifiSetupServer.startAP();
  }
}

// --- gesture and knob input -------------------------------------------
//
// Bring-up instrumentation for the physical input path - the action
// dispatch behind it is already covered by POST /api/input, so what this
// observes is only whether the hardware produces events. Kept at 1 until
// the bidi-pulse decoder (see RotaryEncoderReader.h) is confirmed on real
// hardware. Must be defined ABOVE every #if that tests it: a #if on an
// undefined macro is silently 0, which is how the touch-release log below
// spent a while compiled out.
#define INPUT_DEBUG 1

// Swipes are read straight from the touch driver rather than through LVGL's
// gesture support: LVGL only reports gestures on the object under the
// finger, and the rendered screen is one flat image with no LVGL objects in
// it, so there is nothing for it to attribute a swipe to.
static const int SWIPE_MIN_DISTANCE = 60;   // px, well past a jittery tap
static const int SWIPE_MAX_OFF_AXIS = 70;   // px, keeps a diagonal drag from counting
static const unsigned long SWIPE_MAX_MS = 900;

static bool touchDown = false;
static int16_t touchStartX = 0, touchStartY = 0;
static unsigned long touchStartMs = 0;
static int16_t touchLastX = 0, touchLastY = 0;

// Works from sampleTouch()'s cached reading, never from cst816Read() - see
// its comment for why a second read would find nothing.
static void pollTouchGestures() {
  if (touchIsDown) {
    if (!touchDown) {
      touchDown = true;
      touchStartX = touchX;
      touchStartY = touchY;
      touchStartMs = millis();
    }
    touchLastX = touchX;
    touchLastY = touchY;
    return;
  }
  if (!touchDown) return;
  touchDown = false;

  int dx = touchLastX - touchStartX;
  int dy = touchLastY - touchStartY;
  unsigned long duration = millis() - touchStartMs;
#if INPUT_DEBUG
  Serial.printf("[input] touch released: start=(%d,%d) end=(%d,%d) dx=%d dy=%d %lums\n",
                touchStartX, touchStartY, touchLastX, touchLastY, dx, dy, duration);
#endif

  bool isSwipe = duration <= SWIPE_MAX_MS &&
                 (abs(dx) >= SWIPE_MIN_DISTANCE || abs(dy) >= SWIPE_MIN_DISTANCE);

  if (!isSwipe) {
    // A tap. While the menu is open it selects a tablet; a miss just keeps
    // the menu up rather than dismissing it, since a near-miss on a round
    // screen is far more likely to be bad aim than a change of mind.
    if (screenMenu && screenMenu->isActive()) {
      int picked = screenMenu->screenAtPoint(touchLastX, touchLastY);
      if (picked >= 0) {
        screenMenu->dismiss();
        renderScreenIndex(picked);
      } else {
        screenMenu->keepOpen();
      }
    }
    return;
  }

  const char *buttonId = nullptr;
  if (abs(dx) > abs(dy)) {
    if (abs(dy) <= SWIPE_MAX_OFF_AXIS) buttonId = dx > 0 ? "swipe-right" : "swipe-left";
  } else {
    if (abs(dx) <= SWIPE_MAX_OFF_AXIS) buttonId = dy > 0 ? "swipe-down" : "swipe-up";
  }
  if (!buttonId) return;

  // A swipe while the menu is open keeps it open rather than re-triggering
  // through the action - repeatedly swiping up is a natural "I'm still here"
  // and should not restart the fly-in.
  if (screenMenu && screenMenu->isActive()) {
    screenMenu->keepOpen();
    return;
  }
  dispatchInput(buttonId);
}

#if INPUT_DEBUG
// Waveform recorder for the encoder pins, retrieved over GET /api/debug -
// deliberately NOT serial, because on this board serial monitoring is not
// a neutral observer: every "crash" observed while turning was rst:0x15,
// USB_UART_CHIP_RESET, a reset issued by the USB peripheral itself, and
// the serial monitor's DTR/RTS handling is the prime suspect. Reading over
// HTTP needs no COM port open at all; if the uptime below never goes
// backwards while nothing touches COM, the "crash" was the observer.
//
// The recorder's original question - why the pin scan showed a ~7:1 edge
// ratio between the channels - was answered by Waveshare's own demo before
// this ever ran: the knob is a direction-pulse encoder, not quadrature,
// so only the turned direction's channel pulses (see
// RotaryEncoderReader.h). It stays armed to verify the new decoder from
// the raw pin side, and coexists with it: the recorder uses edge
// interrupts, the decoder polls from an esp_timer, neither claims the
// pins exclusively.
// Named by GPIO number, not A/B: with the pulse-encoder model the roles
// are directional (gpio8 pulses = right, gpio7 = left), and A/B naming is
// exactly how the quadrature assumption snuck in unquestioned.
struct EdgeEvent {
  uint32_t micros;
  uint8_t gpio7;
  uint8_t gpio8;
};
static const int EDGE_BUF_SIZE = 512;
static volatile EdgeEvent edgeBuf[EDGE_BUF_SIZE];
static volatile uint32_t edgeWriteIndex = 0;  // total events ever, ring = % EDGE_BUF_SIZE
static volatile uint32_t edgeCountGpio7 = 0, edgeCountGpio8 = 0;

static void IRAM_ATTR recordEdge() {
  uint32_t i = edgeWriteIndex % EDGE_BUF_SIZE;
  edgeBuf[i].micros = (uint32_t)esp_timer_get_time();
  edgeBuf[i].gpio7 = gpio_get_level(GPIO_NUM_7);
  edgeBuf[i].gpio8 = gpio_get_level(GPIO_NUM_8);
  edgeWriteIndex = edgeWriteIndex + 1;
}
static void IRAM_ATTR recordEdgeGpio7() { edgeCountGpio7++; recordEdge(); }
static void IRAM_ATTR recordEdgeGpio8() { edgeCountGpio8++; recordEdge(); }

static void beginWaveformRecorder() {
  pinMode(7, INPUT_PULLUP);
  pinMode(8, INPUT_PULLUP);
  attachInterrupt(7, recordEdgeGpio7, CHANGE);
  attachInterrupt(8, recordEdgeGpio8, CHANGE);
}

// Cumulative end-to-end counters (decoder detents that actually reached
// dispatchInput), so the full pin -> decoder -> dispatch path can be
// verified over HTTP by turning the knob and polling GET /api/debug.
static uint32_t dispatchedRight = 0, dispatchedLeft = 0;

// Dumps uptime (a reboot detector - if this goes backwards between polls,
// the device reset), the edge counters, and the most recent events with
// microsecond deltas.
static String buildDebugJson() {
  String out = "{\"uptimeMs\":" + String(millis());
  out += ",\"edgeCountGpio7\":" + String(edgeCountGpio7);
  out += ",\"edgeCountGpio8\":" + String(edgeCountGpio8);
  out += ",\"dispatchedRight\":" + String(dispatchedRight);
  out += ",\"dispatchedLeft\":" + String(dispatchedLeft);
  out += ",\"events\":[";
  uint32_t total = edgeWriteIndex;
  uint32_t n = total < (uint32_t)EDGE_BUF_SIZE ? total : (uint32_t)EDGE_BUF_SIZE;
  uint32_t start = total - n;
  uint32_t prevT = 0;
  for (uint32_t k = 0; k < n; k++) {
    const volatile EdgeEvent &e = edgeBuf[(start + k) % EDGE_BUF_SIZE];
    if (k > 0) out += ",";
    out += "{\"dt\":" + String(k == 0 ? 0 : e.micros - prevT);
    out += ",\"gpio7\":" + String(e.gpio7) + ",\"gpio8\":" + String(e.gpio8) + "}";
    prevT = e.micros;
  }
  out += "]}";
  return out;
}
#endif

static void pollEncoder() {
  int32_t detents = RotaryEncoderReader::readDetents();
  if (detents == 0) return;

#if INPUT_DEBUG
  if (detents > 0) dispatchedRight += detents; else dispatchedLeft += -detents;
#endif

  // One dispatch per detent, so a fast turn still sends one value change
  // per click rather than a burst proportional to speed. Positive = right
  // per the reader's contract; button-1 is "Rotate Right" in the DDF
  // adornment SVG, button-0 "Rotate Left".
  const char *buttonId = detents > 0 ? "button-1" : "button-0";
  for (int32_t i = 0; i < abs(detents); i++) dispatchInput(buttonId);
}

static void dispatchInput(const char *buttonId) {
  if (!projectLoader.isLoaded()) return;
  dispatchButtonAction(projectLoader.getButtonAction(currentScreenIndex, String(buttonId)));
}

static void loadAndRenderProject() {
  if (!LittleFS.exists("/PROJECT/project.json")) {
    Serial.println("[project] none installed yet - deploy one, or POST /api/project");
    return;
  }
  if (!projectLoader.loadProject("/PROJECT/project.json")) {
    Serial.println("[project] failed to load /PROJECT/project.json");
    return;
  }
  const ProjectConfig &project = projectLoader.getProject();
  Serial.printf("[project] loaded \"%s\", %d screen(s)\n", project.name.c_str(), (int)project.screens.size());

  // Icon paths are screen-id-based, not content-hashed, so a redeploy can
  // change what a path's bytes are - the cache must not survive a reload.
  ColorAssetLoader::clearGrayscaleMaskCache();
  if (screenRenderer == nullptr) {
    screenRenderer = new ColorScreenRenderer(projectLoader, &canvasPresenter.canvas());
  }
  topicsSubscribed = false;
  if (!renderScreenIndex(0)) {
    Serial.println("[project] renderScreen(0) failed");
    return;
  }
  Serial.println("[project] rendered screen 0");
}

// ------------------------------------------------------------------- mqtt

// A topic may carry a "#path.into.json" suffix; subscriptions compare bare.
static String stripJsonPath(const String &topicOrPath) {
  int hashIndex = topicOrPath.indexOf('#');
  return hashIndex == -1 ? topicOrPath : topicOrPath.substring(0, hashIndex);
}

// Recurses into children so nested objects (tab-control -> panel) do not
// silently miss their subscriptions.
static void collectTopics(const std::vector<ScreenObject> &objects, std::vector<String> &outTopics) {
  for (const auto &obj : objects) {
    if (!obj.properties.topic.isEmpty()) {
      String realTopic = stripJsonPath(obj.properties.topic);
      bool exists = false;
      for (const auto &existing : outTopics) {
        if (existing == realTopic) { exists = true; break; }
      }
      if (!exists) outTopics.push_back(realTopic);
    }
    if (!obj.children.empty()) collectTopics(obj.children, outTopics);
  }
}

static bool screenUsesTopic(const std::vector<ScreenObject> &objects, const String &topic) {
  for (const auto &obj : objects) {
    if (!obj.properties.topic.isEmpty() && stripJsonPath(obj.properties.topic) == topic) return true;
    if (!obj.children.empty() && screenUsesTopic(obj.children, topic)) return true;
  }
  return false;
}

static String deviceTopic(const char *leaf) {
  return String(TOPIC_PREFIX) + "/" + mqttClient.getClientId() + "/" + leaf;
}

static void subscribeToAllTopics() {
  if (topicsSubscribed || !projectLoader.isLoaded() || !mqttClient.isConnected()) return;
  const ProjectConfig &project = projectLoader.getProject();
  std::vector<String> topics;
  for (const auto &screen : project.screens) collectTopics(screen.objects, topics);
  if (!topics.empty()) mqttClient.subscribeToKeys(topics);
  topicsSubscribed = true;
  Serial.printf("[mqtt] subscribed to %d project topic(s)\n", (int)topics.size());
}

static void publishHello() {
  JsonDocument doc;
  doc["deviceId"] = DEVICE_ID;
  doc["firmwareVersion"] = FIRMWARE_VERSION;
  // ddfVersion+url are what make this device appear under "Announced
  // Devices" at all - without them the designer treats it as older firmware
  // that publishes no DDF and skips it. Still ddfVersion rather than the DDF
  // content hash the designer is moving to, deliberately: its auto-discovery
  // does not understand a hash yet, and no device should run ahead of a
  // designer feature that does not exist (see DeviceInfo.h's DDF_VERSION).
  doc["ddfVersion"] = DDF_VERSION;
  doc["url"] = "http://" + WiFi.localIP().toString() + "/ddf.zip";
  String payload;
  serializeJson(doc, payload);
  mqttClient.publish(deviceTopic("hello"), payload, true);
  Serial.println("[mqtt] published hello");
}

static void publishDeployStatus(const String &deployId, const String &state, const String &error, int percent) {
  JsonDocument doc;
  doc["deployId"] = deployId;
  doc["state"] = state;
  if (!error.isEmpty()) doc["error"] = error;
  // Optional per the contract, but the deploy dialog reads
  // `status.percent ?? 0` - never publishing one leaves its progress bar
  // visibly stuck at 0 for the whole deploy even when it works.
  if (percent >= 0) doc["percent"] = percent;
  String payload;
  serializeJson(doc, payload);
  mqttClient.publish(deviceTopic("deploy-status"), payload, false);
}

// The deploy topic is retained, so the broker redelivers the last trigger on
// every fresh subscribe - including the one after the reboot that ends a
// successful deploy. Without clearing it, a deploy loops forever.
static void clearDeployTrigger() {
  mqttClient.publish(deviceTopic("deploy"), "", true);
}

static void onMqttMessage(const String &topic, const String &payload) {
  // Device-level control topic - routed to DeployManager rather than falling
  // through to setTopicValue(), which would try to display the deploy JSON.
  //
  // Deferred to loop(), not handled here: this callback runs nested inside
  // PubSubClient's own packet-parsing call stack, and a deploy publishes
  // status ~100 times. PubSubClient shares one buffer between publishing and
  // incoming parsing, so those publishes look fine locally while nothing
  // watching the broker ever sees them.
  if (topic == deviceTopic("deploy")) {
    pendingDeployPayload = payload;
    pendingDeploy = true;
    return;
  }

  if (!projectLoader.isLoaded()) return;
  projectLoader.setTopicValue(topic, payload);

  const ProjectConfig &project = projectLoader.getProject();
  if (currentScreenIndex < 0 || currentScreenIndex >= (int)project.screens.size()) return;
  if (!screenUsesTopic(project.screens[currentScreenIndex].objects, topic)) return;
  renderScreenIndex(currentScreenIndex);
}

static void setupMqtt(const WiFiCredentials &creds) {
  if (creds.mqttHost.isEmpty()) {
    Serial.println("[mqtt] no broker configured - skipping");
    return;
  }
  mqttClient.configure(creds.mqttHost, (uint16_t)creds.mqttPort, creds.mqttUsername, creds.mqttPassword);
  deployManager.setPublishStatusHandler(publishDeployStatus);
  deployManager.setClearTriggerHandler(clearDeployTrigger);
  // Subscribed here rather than from the connected callback: connect()
  // replays its own subscription list on every reconnect, so queueing it once
  // up front covers every later connection too.
  mqttClient.subscribe(deviceTopic("deploy"));
  mqttClient.setConnectedCallback([]() { publishHello(); });
  mqttClient.setCallback(onMqttMessage);
  mqttClient.connect();
  Serial.printf("[mqtt] %s:%d as %s\n", creds.mqttHost.c_str(), creds.mqttPort, mqttClient.getClientId().c_str());
}

// ------------------------------------------------------------------- wifi

static void setupWiFi() {
  WiFiCredentials creds;
  bool haveCreds = configStorage.loadWiFiCredentials(creds) && !creds.ssid.isEmpty();

  if (haveCreds) {
    displayAdapter.showLines({"Connecting", "to WiFi...", creds.ssid});
    WiFi.mode(WIFI_STA);
    WiFi.begin(creds.ssid.c_str(), creds.password.c_str());

    unsigned long start = millis();
    while (WiFi.status() != WL_CONNECTED && millis() - start < 30000) {
      delay(250);
    }

    if (WiFi.status() == WL_CONNECTED) {
      creds.lastIP = WiFi.localIP().toString();
      creds.lastRSSI = WiFi.RSSI();
      configStorage.saveWiFiCredentials(creds);
      Serial.printf("[wifi] connected: %s (%d dBm)\n", creds.lastIP.c_str(), creds.lastRSSI);
      setupModeActive = false;

      setupMqtt(creds);
      loadAndRenderProject();
      subscribeToAllTopics();

      testInterface.setScreenSwitchHandler(renderScreenIndex);
      testInterface.setTopicValueHandler([](const String &topic) { return projectLoader.getTopicValue(topic); });
      testInterface.setInputHandler([](const String &buttonId) { dispatchInput(buttonId.c_str()); });
#if INPUT_DEBUG
      testInterface.setDebugHandler(buildDebugJson);
#endif
      if (testInterface.begin(canvasPresenter.canvas())) {
        Serial.printf("[test] http://%s/\n", creds.lastIP.c_str());
      }

      // Nothing installed yet: say so on screen rather than leaving a blank
      // panel that looks like a failure.
      if (!projectLoader.isLoaded()) {
        displayAdapter.showLines({"Ready", creds.lastIP, "no project yet"});
      }
      return;
    }
    Serial.println("[wifi] connect failed - entering setup mode");
  }

  setupModeActive = true;
  wifiSetupServer.start();
}

// ------------------------------------------------------------------ entry

void setup() {
  Serial.begin(115200);
  delay(500);

  // A previous deep-sleep experiment latched LCD_RST/TOUCH_RST low via
  // rtc_gpio_hold_en(); that hold survives normal resets and reflashes, so
  // release it before gfx->begin() tries to pulse the display out of reset.
  rtc_gpio_hold_dis(GPIO_NUM_21);
  rtc_gpio_hold_dis(GPIO_NUM_10);
  // GPIO7 too - it was this board's EXT0 wake source in the earlier
  // deep-sleep experiment, and rtc_gpio_deinit() returns it to normal GPIO
  // control rather than merely unlatching it. Kept as insurance even
  // though the "GPIO7 stuck at 1 while GPIO8 toggled" observation that
  // originally motivated this turned out to be the encoder's actual
  // protocol, not a held pad: only the turned direction's channel pulses
  // (see RotaryEncoderReader.h), and that scan was done turning right.
  rtc_gpio_hold_dis(GPIO_NUM_7);
  rtc_gpio_deinit(GPIO_NUM_7);
  gpio_deep_sleep_hold_dis();

  pinMode(LCD_BL, OUTPUT);
  digitalWrite(LCD_BL, HIGH);

  Wire.begin(TOUCH_SDA, TOUCH_SCL);
  cst816Init();

  bool gfxOk = gfx->begin(40000000);
  Serial.printf("gfx->begin() = %s\n", gfxOk ? "true" : "false");
  gfx->fillScreen(RGB565_BLACK);

  // Formats on first boot; holds /PROJECT and the staged upload zip.
  if (!LittleFS.begin(true)) {
    Serial.println("LittleFS mount failed - project install/load unavailable");
  }
  configStorage.begin();

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

  if (!canvasPresenter.begin()) {
    Serial.println("FATAL: no canvas - nothing can be rendered");
    return;
  }
  screenMenu = new ScreenNavigatorOverlay(&canvasPresenter.canvas(), projectLoader);

#if INPUT_DEBUG
  // Coexists with the decoder: the recorder is edge interrupts, the
  // decoder polls from its own esp_timer - see the recorder's comment.
  beginWaveformRecorder();
#endif
  RotaryEncoderReader::begin();

  setupWiFi();
}

void loop() {
  // LV_TICK_CUSTOM (LVGL calling Arduino's millis() from a .c translation
  // unit) never worked in this build - lv_tick_get() stayed at 0 even after a
  // full clean rebuild, so no periodic timer ever ran past the first paint.
  // Driving the tick manually sidesteps it entirely.
  // Before lv_timer_handler(), so LVGL and the gesture detector both work
  // from the same single reading of a register that clears when read.
  sampleTouch();

  lv_tick_inc(5);
  lv_timer_handler();

  if (setupModeActive) {
    wifiSetupServer.handleClient();

    // Credentials just arrived from the portal - restart so they are applied
    // through the normal setupWiFi() path rather than trying to switch modes
    // in place. Without this the device stays in AP mode forever after a
    // successful setup: the auto-reset countdown below is dead once any
    // client has connected, and connecting is exactly what a user doing the
    // setup did. Found by walking the portal through on real hardware
    // (2026-08-19) - it saved the credentials, said so, and then sat there.
    if (wifiSetupServer.hasWiFiCredentials() && !WiFi.isConnected()) {
      Serial.println("[setup] credentials configured - restarting to apply");
      delay(1000);
      ESP.restart();
    }

    // Reports "nobody ever connected within 2 minutes"; restarting is this
    // file's decision, not the server's.
    if (wifiSetupServer.tick()) {
      Serial.println("[setup] no client connected - restarting");
      delay(100);
      ESP.restart();
    }
    delay(5);
    return;
  }

  testInterface.handleClient();
  mqttClient.loop();
  subscribeToAllTopics();

  pollTouchGestures();
  pollEncoder();

  // No serial reporting here on purpose - the recorder is read over
  // GET /api/debug, so nothing needs a COM port open (see the recorder's
  // own comment for why that matters on this board).

  // The overlay owns the frame while it is up: it re-renders the current
  // screen and composites the tablets on top in one pass, so nothing else
  // may render underneath it. Doing both would show a bare frame between
  // them - the M5 Dial hit exactly that as a visible flicker on every jump.
  if (screenMenu && screenMenu->isActive() && screenMenu->update(screenRenderer, currentScreenIndex)) {
    canvasPresenter.present();
  }

  // Run here, after mqttClient.loop() has returned and PubSubClient's own
  // call stack has unwound - see onMqttMessage() for why this cannot happen
  // inside the callback.
  if (pendingDeploy) {
    pendingDeploy = false;
    String payload = pendingDeployPayload;
    pendingDeployPayload = "";
    deployManager.handleDeployMessage(payload);
  }

  delay(5);
}
