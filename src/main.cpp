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

// Instrumentation for the physical input path, served at GET /api/debug.
// Deliberately permanent, not bring-up scaffolding: a serial monitor is
// not a neutral observer on this board (DTR/RTS changes reset the S3 via
// its native USB), so this endpoint is the only way to diagnose input
// hardware here - and it has already caught two protocol-level bugs the
// old way could not (the knob's pulse encoding, the CST816's byte-2
// criterion). Costs ~10KB of statics and a few reads per loop.
// Lives up here, above everything that tests it: a #if on an undefined
// macro is silently 0, which is how the touch instrumentation spent a
// while compiled out.
#define INPUT_DEBUG 1

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
// Byte 1 is the gesture id, byte 2 the finger count - see sampleTouch()
// for which of them means "touched" on this unit (settled by probe data,
// not guessed). Reads the raw block; interpreting it is the caller's job.
static bool cst816ReadRaw(uint8_t buf[7]) {
  Wire.beginTransmission(CST816_ADDR);
  Wire.write(0x00);
  if (Wire.endTransmission(false) != 0) return false;
  if (Wire.requestFrom((int)CST816_ADDR, 7) != 7) return false;
  for (uint8_t i = 0; i < 7; i++) buf[i] = Wire.read();
  return true;
}

// The CST816 is read exactly ONCE per sample period, into these, and both
// consumers - LVGL's input device and the gesture detector - work from the
// result. They must not each read the chip: reading clears its touch
// state, so whichever ran first consumed the event and the other saw an
// idle panel.
static bool touchIsDown = false;
static int16_t touchX = 0, touchY = 0;

// Runtime-settable via GET /api/debug?set=touchPeriodMs=N, no reflash
// needed. Built to test whether polling faster than the chip's own scan
// cycle returns cleared data - it does not (probe showed solid tracking
// at 7ms); kept as a diagnosis knob since it costs nothing.
static uint32_t touchPeriodMs = 5;

#if INPUT_DEBUG
static uint32_t touchReadCount = 0;
static uint32_t touchI2cErrorCount = 0;

// Raw-byte probe: every read where any of gesture id, finger count or
// coordinates is nonzero is recorded verbatim. One physical swipe then
// shows exactly what the chip reports over time - instead of a fourth
// round of guessing which register means "touched" on this unit.
struct TouchProbe {
  uint32_t ms;
  uint8_t b1, b2;
  uint16_t x, y;
};
static const int TOUCH_PROBE_SIZE = 256;
static TouchProbe touchProbeBuf[TOUCH_PROBE_SIZE];
static uint32_t touchProbeWriteIndex = 0;  // total ever, ring = % TOUCH_PROBE_SIZE
#endif

static void sampleTouch() {
  static unsigned long lastSampleMs = 0;
  unsigned long now = millis();
  // Keeps the previous state between samples rather than resetting it, so
  // a longer period does not fabricate releases.
  if (now - lastSampleMs < touchPeriodMs) return;
  lastSampleMs = now;

  uint8_t buf[7];
  if (!cst816ReadRaw(buf)) {
#if INPUT_DEBUG
    touchI2cErrorCount++;
#endif
    touchIsDown = false;
    return;
  }
#if INPUT_DEBUG
  touchReadCount++;
#endif

  int16_t x = ((buf[3] & 0x0F) << 8) | buf[4];
  int16_t y = ((buf[5] & 0x0F) << 8) | buf[6];

#if INPUT_DEBUG
  if (buf[1] != 0 || buf[2] != 0 || x != 0 || y != 0) {
    TouchProbe &p = touchProbeBuf[touchProbeWriteIndex % TOUCH_PROBE_SIZE];
    p.ms = now;
    p.b1 = buf[1];
    p.b2 = buf[2];
    p.x = (uint16_t)x;
    p.y = (uint16_t)y;
    touchProbeWriteIndex++;
  }
#endif

  // Byte 2 (finger count) ONLY - the official demo's criterion, confirmed
  // by this unit's own probe data (2026-08-20, one recorded swipe at 7ms
  // cadence): b2 holds 1 through the whole drag with live coordinates,
  // while b1 (gesture id) pulses only when the chip's gesture engine
  // fires AND stays latched at 5 (click) for ~450ms after the finger
  // lifts, with frozen coordinates. Keying on b1 is why swipes never
  // worked: mid-drag it is mostly 0 (instant phantom release), and after
  // release its latch fabricates a stationary phantom touch. The earlier
  // note that b2 "stayed 0 during drags" predates this probe and was an
  // artifact of that day's reset/register experiments - the probe is the
  // authoritative record.
  if (buf[2] != 0) {
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

// -------------------------------------------------- frame timing probe
//
// Splits one screen change into the three costs that are otherwise
// indistinguishable from the outside, where the whole thing just measures
// ~100ms (2026-08-21, timed over POST /api/screen):
//
//   render - ColorScreenRenderer filling the 360x360 RGB565 canvas
//   lvgl   - LVGL walking that canvas into its own draw buffer
//   qspi   - draw16bitRGBBitmap actually pushing bytes to the ST77916
//
// Which one dominates decides what is worth fixing. Pure QSPI transfer of
// a full frame is ~13ms at 40MHz x4, so if `qspi` comes back near that,
// the remaining ~85ms is on this side of the bus and no bus-level change
// (faster clock, bigger chunks) will help much. Both buffers living in
// PSRAM is the obvious suspect, and it is a one-line change to test -
// but only worth making if the numbers say so.
//
// Always compiled, not gated behind INPUT_DEBUG: the point is to describe
// the build that actually ships, and the cost is four
// esp_timer_get_time() calls per screen change. Only the JSON rendering
// sits inside the debug block, with the rest of that endpoint.
//
// `chunks` is the honest surprise to watch for: LVGL only refreshes on its
// own period, so a render whose lv_timer_handler() call finds the refresh
// timer not yet due flushes *nothing* (chunks 0) and its frame never
// reaches the panel at all - dropped, not torn. During a fast bezel spin
// that is the difference between "the panel is behind" and "we are
// rendering frames nobody will ever see".
struct FrameTiming {
  uint32_t renderUs;
  uint32_t lvglUs;
  uint32_t qspiUs;
  uint32_t gapMs;  // since the previous screen change - shows how fast input arrives
  uint16_t chunks;
};
static const int FRAME_LOG_SIZE = 16;
static FrameTiming frameLog[FRAME_LOG_SIZE];
static uint32_t frameLogWriteIndex = 0;  // total frames ever, ring = % FRAME_LOG_SIZE
// Accumulated by dispFlushCb across the chunks of one flush, reset per frame.
static volatile uint32_t frameQspiUs = 0;
static volatile uint16_t frameChunks = 0;
static uint32_t lastFrameEndMs = 0;

static void dispFlushCb(lv_disp_drv_t *disp, const lv_area_t *area, lv_color_t *color_p) {
  uint32_t w = area->x2 - area->x1 + 1;
  uint32_t h = area->y2 - area->y1 + 1;
  const int64_t pushStart = esp_timer_get_time();
  gfx->draw16bitRGBBitmap(area->x1, area->y1, (uint16_t *)color_p, w, h);
  frameQspiUs += (uint32_t)(esp_timer_get_time() - pushStart);
  frameChunks++;
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

  const int64_t renderStart = esp_timer_get_time();
  if (!screenRenderer->renderScreen(index)) return false;
  const int64_t renderEnd = esp_timer_get_time();
  currentScreenIndex = index;

  frameQspiUs = 0;
  frameChunks = 0;
  canvasPresenter.present();
  // present() only marks the area dirty; pump LVGL so an HTTP caller that
  // snapshots straight after POST /api/screen sees the new frame rather than
  // the previous one - a race that would look exactly like a rendering bug.
  lv_timer_handler();
  const int64_t flushEnd = esp_timer_get_time();

  // The flush window minus what was spent inside draw16bitRGBBitmap is
  // LVGL's own work: walking the image source into its draw buffer, plus
  // its bookkeeping. Clamped rather than allowed to underflow - these are
  // unsigned, and the two clocks are read at slightly different points.
  const uint32_t flushUs = (uint32_t)(flushEnd - renderEnd);
  const uint32_t qspiUs = frameQspiUs;
  const uint32_t nowMs = millis();
  FrameTiming &slot = frameLog[frameLogWriteIndex % FRAME_LOG_SIZE];
  slot.renderUs = (uint32_t)(renderEnd - renderStart);
  slot.qspiUs = qspiUs;
  slot.lvglUs = flushUs > qspiUs ? flushUs - qspiUs : 0;
  slot.chunks = frameChunks;
  slot.gapMs = lastFrameEndMs == 0 ? 0 : nowMs - lastFrameEndMs;
  lastFrameEndMs = nowMs;
  frameLogWriteIndex++;
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

#if INPUT_DEBUG
// Touch-path diagnosis, exposed via GET /api/debug like the edge recorder
// and for the same reason (a serial monitor resets this board). Answers,
// after one physical swipe, WHERE the path breaks: no samples = driver
// never sees the finger; samples but no release = tracking; a release
// with wrong dx/dy = coordinates; a release classified "none" =
// thresholds; a correct verdict = the path works and the problem is
// downstream of dispatch.
static uint32_t touchSampleCount = 0;
static int16_t lastTouchSampleX = -1, lastTouchSampleY = -1;
static uint32_t touchReleaseCount = 0;
static int lastReleaseDx = 0, lastReleaseDy = 0;
static unsigned long lastReleaseDurationMs = 0;
static const char *lastReleaseVerdict = "none yet";
#endif

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
static unsigned long touchLastDownMs = 0;

// The probe (see sampleTouch) also showed occasional all-zero reads in the
// middle of an active drag - the chip's own scan gaps at our 5ms polling.
// Each one used to end the gesture on the spot, fragmenting one swipe into
// ~9 tiny releases whose fragments mostly classified as nothing. A release
// only counts once the panel has read idle for this long; gesture duration
// is measured to the last down-read, so the debounce tail never inflates
// it.
static const unsigned long RELEASE_DEBOUNCE_MS = 80;

// Works from sampleTouch()'s cached reading, never from cst816Read() - see
// its comment for why a second read would find nothing.
static void pollTouchGestures() {
  if (touchIsDown) {
#if INPUT_DEBUG
    touchSampleCount++;
    lastTouchSampleX = touchX;
    lastTouchSampleY = touchY;
#endif
    if (!touchDown) {
      touchDown = true;
      touchStartX = touchX;
      touchStartY = touchY;
      touchStartMs = millis();
    }
    touchLastX = touchX;
    touchLastY = touchY;
    touchLastDownMs = millis();
    return;
  }
  if (!touchDown) return;
  // Not-down could be a mid-drag scan gap - only a sustained idle reading
  // is a real release (see RELEASE_DEBOUNCE_MS).
  if (millis() - touchLastDownMs < RELEASE_DEBOUNCE_MS) return;
  touchDown = false;

  int dx = touchLastX - touchStartX;
  int dy = touchLastY - touchStartY;
  unsigned long duration = touchLastDownMs - touchStartMs;
#if INPUT_DEBUG
  touchReleaseCount++;
  lastReleaseDx = dx;
  lastReleaseDy = dy;
  lastReleaseDurationMs = duration;
  lastReleaseVerdict = "tap-or-none";
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
#if INPUT_DEBUG
  lastReleaseVerdict = buttonId ? buttonId : "off-axis";
#endif
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
  out += ",\"touchSampleCount\":" + String(touchSampleCount);
  out += ",\"lastTouchX\":" + String(lastTouchSampleX);
  out += ",\"lastTouchY\":" + String(lastTouchSampleY);
  out += ",\"touchReleaseCount\":" + String(touchReleaseCount);
  out += ",\"lastReleaseDx\":" + String(lastReleaseDx);
  out += ",\"lastReleaseDy\":" + String(lastReleaseDy);
  out += ",\"lastReleaseDurationMs\":" + String(lastReleaseDurationMs);
  out += ",\"lastReleaseVerdict\":\"" + String(lastReleaseVerdict) + "\"";
  out += ",\"touchPeriodMs\":" + String(touchPeriodMs);
  out += ",\"touchReadCount\":" + String(touchReadCount);
  out += ",\"touchI2cErrorCount\":" + String(touchI2cErrorCount);
  out += ",\"frameCount\":" + String(frameLogWriteIndex);
  // Newest last. gapMs is the wall-clock distance from the previous screen
  // change, so a fast bezel spin shows up as a run of small gaps - if gapMs
  // is consistently below render+lvgl+qspi, input is arriving faster than
  // the panel can be repainted, which is the tearing everyone sees.
  out += ",\"frames\":[";
  {
    uint32_t total = frameLogWriteIndex;
    uint32_t n = total < (uint32_t)FRAME_LOG_SIZE ? total : (uint32_t)FRAME_LOG_SIZE;
    uint32_t start = total - n;
    for (uint32_t k = 0; k < n; k++) {
      const FrameTiming &f = frameLog[(start + k) % FRAME_LOG_SIZE];
      if (k > 0) out += ",";
      out += "{\"renderUs\":" + String(f.renderUs);
      out += ",\"lvglUs\":" + String(f.lvglUs);
      out += ",\"qspiUs\":" + String(f.qspiUs);
      out += ",\"chunks\":" + String(f.chunks);
      out += ",\"gapMs\":" + String(f.gapMs) + "}";
    }
  }
  out += "]";
  out += ",\"touchProbe\":[";
  {
    uint32_t total = touchProbeWriteIndex;
    uint32_t n = total < (uint32_t)TOUCH_PROBE_SIZE ? total : (uint32_t)TOUCH_PROBE_SIZE;
    uint32_t start = total - n;
    uint32_t prevT = 0;
    for (uint32_t k = 0; k < n; k++) {
      const TouchProbe &p = touchProbeBuf[(start + k) % TOUCH_PROBE_SIZE];
      if (k > 0) out += ",";
      out += "{\"dt\":" + String(k == 0 ? 0 : p.ms - prevT);
      out += ",\"b1\":" + String(p.b1) + ",\"b2\":" + String(p.b2);
      out += ",\"x\":" + String(p.x) + ",\"y\":" + String(p.y) + "}";
      prevT = p.ms;
    }
  }
  out += "]";
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
      // GET /api/debug?set=touchPeriodMs=30 adjusts the touch sample
      // period per request - the polling-rate hypothesis is tested over
      // HTTP rather than one reflash per candidate value.
      testInterface.setDebugHandler([](const String &setArg) {
        if (setArg.startsWith("touchPeriodMs=")) {
          long v = setArg.substring(14).toInt();
          if (v >= 1 && v <= 500) touchPeriodMs = (uint32_t)v;
        }
        return buildDebugJson();
      });
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
