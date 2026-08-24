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
#include "driver/rtc_io.h"

#include "CanvasPresenter.h"
#include "CanvasDisplayAdapter.h"
#include "NvsConfigStorage.h"
#include "TestInterfaceServer.h"
#include "WiFiSetupServer.h"
#include "MqttClient.h"
#include "DeviceSettings.h"
#include <ArduinoJson.h>
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
// Where lvBuf1 actually landed - reported over /api/debug, because every
// flush timing below means something different depending on the answer.
static bool drawBufInternal = false;

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
static void enterSetupMode();
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
#if INPUT_DEBUG
// Filled by sampleTouch for the contact in progress, read out into the
// gesture log on release - see GestureRecord's own comment for why this
// records a range rather than one sample.
static uint8_t gestureMaxFingers = 0;
static uint8_t gestureFingerMask = 0;
#endif

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

#if INPUT_DEBUG
// While this is in the future the panel is not read at all and whatever the
// last injected sample left in touchX/touchY/touchIsDown stands. Without the
// suppression the very next real read - of a panel nobody is touching -
// would overwrite an injected drag with "finger lifted" within 5ms, and the
// injection would look like it had never arrived.
static uint32_t touchInjectUntilMs = 0;
static const uint32_t TOUCH_INJECT_HOLD_MS = 400;
#endif

static void sampleTouch() {
  static unsigned long lastSampleMs = 0;
  unsigned long now = millis();
#if INPUT_DEBUG
  if (now < touchInjectUntilMs) return;
#endif
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
#if INPUT_DEBUG
    if (buf[2] > gestureMaxFingers) gestureMaxFingers = buf[2];
    if (buf[2] < 8) gestureFingerMask |= (uint8_t)(1u << buf[2]);
#endif
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
static uint32_t lastFrameEndMs = 0;

// Monotonic since boot, never reset. The first version of this probe
// accumulated per render instead, and measured a lie: it only saw what was
// flushed inside renderScreenIndex's own lv_timer_handler() call. LVGL
// refreshes on its own period, so when screen changes arrive faster than
// that - which is exactly the case under investigation, a bezel spin - that
// call finds the timer not due, returns in 0.1ms, and the flush happens in
// the *next* pump from loop(). The probe reported chunks 0 for all 16
// frames of a real spin, which reads as "nothing ever reached the panel"
// and is simply false: it was measuring the wrong call site.
//
// So: count globally, attribute by difference between one present() and the
// next. Everything pushed in that window belongs to the frame presented at
// its start, wherever the pump that pushed it happened to run.
#if INPUT_DEBUG
// Mirrors every pixel handed to the panel, so GET /panel.bmp can show what
// is on the glass rather than what the renderer drew. Kept out of the qspi
// and lvgl attributions below - it is measurement scaffolding and must not
// be charged to either.
static uint16_t *panelShadow = nullptr;
// Every rectangle LVGL hands to the panel, newest last. The chunk *count*
// says how much was pushed but not where, and "6 chunks pushed, most of the
// screen still wrong" is a sentence that can only be resolved by knowing
// which six.
struct FlushArea { int16_t x1, y1, x2, y2; uint16_t atMs; };
static const int FLUSH_LOG_SIZE = 12;
static FlushArea flushLog[FLUSH_LOG_SIZE];
static uint32_t flushLogIndex = 0;
static volatile uint32_t totalShadowUs = 0;
static uint32_t markShadowUs = 0;
#endif

static volatile uint32_t totalChunks = 0;
static volatile uint32_t totalQspiUs = 0;
static uint32_t totalLvglUs = 0;
static uint32_t markChunks = 0, markQspiUs = 0, markLvglUs = 0;

static void dispFlushCb(lv_disp_drv_t *disp, const lv_area_t *area, lv_color_t *color_p) {
  uint32_t w = area->x2 - area->x1 + 1;
  uint32_t h = area->y2 - area->y1 + 1;
  const int64_t pushStart = esp_timer_get_time();
  gfx->draw16bitRGBBitmap(area->x1, area->y1, (uint16_t *)color_p, w, h);
  totalQspiUs += (uint32_t)(esp_timer_get_time() - pushStart);
  totalChunks++;
#if INPUT_DEBUG
  {
    FlushArea &fa = flushLog[flushLogIndex % FLUSH_LOG_SIZE];
    fa.x1 = area->x1; fa.y1 = area->y1; fa.x2 = area->x2; fa.y2 = area->y2;
    fa.atMs = (uint16_t)millis();
    flushLogIndex++;
  }
  if (panelShadow != nullptr) {
    const int64_t shadowStart = esp_timer_get_time();
    const uint16_t *src = (const uint16_t *)color_p;
    for (uint32_t row = 0; row < h; row++) {
      memcpy(&panelShadow[(area->y1 + row) * SCREEN_SIZE + area->x1], src + row * w, w * sizeof(uint16_t));
    }
    totalShadowUs += (uint32_t)(esp_timer_get_time() - shadowStart);
  }
#endif
  lv_disp_flush_ready(disp);
}

// Every pump of LVGL goes through here so its cost is counted wherever it
// runs. (CanvasDisplayAdapter has a third call site for setup-mode text;
// deliberately left alone - it runs before any project exists, so there is
// no frame for it to belong to.)
static inline void pumpLvgl() {
  const int64_t pumpStart = esp_timer_get_time();
  lv_timer_handler();
  totalLvglUs += (uint32_t)(esp_timer_get_time() - pumpStart);
}

// Fills in what actually reached the panel for the most recent frame.
// Computed from the marks rather than accumulated, so calling it repeatedly
// is harmless - buildDebugJson() does exactly that, to report the newest
// frame as it stands instead of a placeholder zero.
static void closeOpenFrame() {
  if (frameLogWriteIndex == 0) return;
  FrameTiming &f = frameLog[(frameLogWriteIndex - 1) % FRAME_LOG_SIZE];
  const uint32_t qspi = totalQspiUs - markQspiUs;
  uint32_t lvgl = totalLvglUs - markLvglUs;
#if INPUT_DEBUG
  // The shadow copy runs inside the flush and so inside the LVGL pump that
  // called it; charged to LVGL it would make every timing here a lie about
  // the shipping build.
  const uint32_t shadow = totalShadowUs - markShadowUs;
  lvgl = lvgl > shadow ? lvgl - shadow : 0;
#endif
  f.chunks = (uint16_t)(totalChunks - markChunks);
  f.qspiUs = qspi;
  // LVGL's own share is whatever its pumps cost beyond the bytes they
  // pushed - the walk from the canvas into the draw buffer, plus its
  // bookkeeping.
  f.lvglUs = lvgl > qspi ? lvgl - qspi : 0;
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

  // The previous frame's window ends here: it had from its own present()
  // until now to reach the panel.
  closeOpenFrame();

  const uint32_t nowMs = millis();
  FrameTiming &slot = frameLog[frameLogWriteIndex % FRAME_LOG_SIZE];
  slot.renderUs = (uint32_t)(renderEnd - renderStart);
  slot.gapMs = lastFrameEndMs == 0 ? 0 : nowMs - lastFrameEndMs;
  slot.chunks = 0;
  slot.qspiUs = 0;
  slot.lvglUs = 0;
  lastFrameEndMs = nowMs;
  frameLogWriteIndex++;
  markChunks = totalChunks;
  markQspiUs = totalQspiUs;
  markLvglUs = totalLvglUs;
#if INPUT_DEBUG
  markShadowUs = totalShadowUs;
#endif

  canvasPresenter.present();
  // present() only marks the area dirty; pump LVGL so an HTTP caller that
  // snapshots straight after POST /api/screen sees the new frame rather than
  // the previous one - a race that would look exactly like a rendering bug.
  pumpLvgl();
  return true;
}

// ------------------------------------------------------------- navigation
//
// Paging is separated from drawing (2026-08-21). Every detent used to
// render a whole screen synchronously, and at ~32ms per frame a fast bezel
// spin put 269 of 359 intermediate screens on the panel - measured. That is
// what reads as flashing, and it got *worse* as the pipeline got faster:
// the frames that used to be quietly dropped because LVGL's refresh timer
// was not due now had time to complete. Optimizing the frame cost was
// pushing on the wrong axis; the intermediate screens should not be drawn
// at all.
//
// The obvious fix - draw only where the turn ends - was built and tried on
// hardware, and it was rejected there. With no frames at all during the
// turn the knob feels stuck, and that is subjectively worse than the
// flashing it removes: 89 detents produced 26 frames, every one of them
// displayed, and the verdict was still "klemmt". The intermediate frames
// are not only noise, they are the feedback that the input is being heard.
// Removing them removed the wrong half.
//
// So neither extreme, but a *rate limit*. While a turn is running the
// screen is repainted at most every navFrameMs, showing wherever the
// position has got to by then, and the position the turn ends on is always
// painted. The knob keeps answering at a legible ~8 frames a second
// instead of a 30Hz strobe, and no frame is computed that nobody sees.
//
// A detent still moves the *logical* position immediately, rather than
// accumulating a delta to apply later: the next detent looks up its action
// on the screen it is now on, and screens may bind their buttons
// differently - a burst has to walk them exactly as a slow turn would.
//
// Deliberately only for next/previous-screen. A goto-screen binding, the
// screen menu's own pick and POST /api/screen are all deliberate,
// non-repeating acts that should render immediately - and the HTTP path
// must, since its caller snapshots as soon as it returns.
// Declared up here because navigateToScreen has to know about them; the
// machinery itself lives further down, next to the gesture detector that
// drives it.
static bool dragActive = false;
static bool dragAnimating = false;

static bool pendingScreenRender = false;
static uint32_t lastNavRenderMs = 0;
// Live-adjustable over GET /api/debug?set=navFrameMs=..., so the feel can be
// tuned against a real turn instead of one reflash per candidate value.
// 0 restores a repaint per detent. Values below the cost of a frame itself
// (~32ms) change nothing - that cost is the real floor.
static uint32_t navFrameMs = 120;

static void renderPendingScreen() {
  if (!pendingScreenRender) return;
  pendingScreenRender = false;
  // Timed from the start of the repaint rather than its end, so the cadence
  // is navFrameMs and not navFrameMs plus however long a repaint takes.
  lastNavRenderMs = millis();
  renderScreenIndex(currentScreenIndex);
}

static void navigateToScreen(int index) {
  // A swipe transition owns both canvases while it runs. A detent arriving
  // inside that window would render into the buffer parked off-screen and
  // be lost, so it is dropped rather than queued: turning the knob during a
  // swipe is not a real gesture, and queueing would page twice from it.
  if (dragActive || dragAnimating) return;
  currentScreenIndex = index;
  pendingScreenRender = true;
  // Leading edge: a detent arriving after a quiet spell is drawn at once,
  // so a single click stays instant and a spin starts answering immediately.
  if ((millis() - lastNavRenderMs) >= navFrameMs) renderPendingScreen();
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
    navigateToScreen(((currentScreenIndex + delta) % count + count) % count);
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
    enterSetupMode();
  }
}

// One way in, whether it came from a project's bound action or from the
// reserved hold gesture - so both behave identically and there is one
// place to change what entering setup means.
static void enterSetupMode() {
  if (setupModeActive) return;
  Serial.println("[setup] entering setup mode - starting AP");
  setupModeActive = true;
  // The test interface owns port 80 in normal operation and the setup
  // server wants the same port. Nothing stopped the first one before, so
  // the second bind landed on an occupied socket - harmless-looking until
  // the day something actually enters setup mode, which the reserved hold
  // gesture now makes easy. Stop it first, deliberately: in setup mode the
  // device is not serving snapshots or accepting deploys anyway.
  testInterface.stop();
  wifiSetupServer.startAP();
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
// How straight a swipe has to be, as a ratio rather than a fixed number of
// pixels. This was an absolute 70px cap, and that produced a plain
// contradiction on real gestures (recorded 2026-08-22): a 309px swipe up
// with 90px of drift was rejected as diagonal, while a 127px swipe with
// 51px of drift - a visibly wonkier gesture - was accepted, purely because
// the shorter one had less absolute drift. A finger pivoting from the wrist
// drifts in proportion to how far it travels, so the rule has to be
// proportional too. 2:1 accepts everything a human means as a swipe (the
// straightest observed was 14:1, the wonkiest accepted 2.5:1) and still
// rejects a genuine 1:1 diagonal.
static const int SWIPE_AXIS_RATIO = 2;
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

// ------------------------------------------------- reserved setup gesture
//
// One finger held for five seconds opens the setup AP, and no project can
// take that away. See docs/device-contract.md (designer repo), "Getting
// back into setup mode": a device that connects happily to a network you
// cannot reach otherwise has no way back, because `goto-setup-mode` is
// only there if whoever designed the project chose to bind it - and the
// project this was found on bound it nowhere.
//
// One finger rather than several is measured, not preferred: on this
// panel a multi-finger contact is reported for about 100ms and then stops
// however long it is held, and the reported count saturates at 2. A
// multi-finger hold cannot be detected here at all.
//
// The countdown appears after a second so the gesture explains itself
// rather than surprising anyone, and releasing cancels it. From the moment
// it shows, the gesture belongs to setup mode: the release dispatches
// nothing, because someone who lets go wanted neither setup nor whatever
// button their finger happened to be resting on.
static const unsigned long SETUP_HOLD_MS = 5000;
static const unsigned long SETUP_HOLD_FEEDBACK_MS = 1000;
// A hold has to stay put. Without this a slow swipe would arm it, and on a
// screen where swiping pages back and forth this would fire eventually.
static const int SETUP_HOLD_MAX_DRIFT = 40;

static bool setupHoldCountdownShown = false;
static int setupHoldLastSecond = -1;
static bool setupHoldConsumed = false;  // this contact belongs to the gesture now
// This contact started on a dark screen, so it wakes and does nothing else.
// A separate flag from setupHoldConsumed because the two coexist: a wake
// touch may still be held into the setup gesture, which is deliberate -
// holding is unambiguous, and making someone touch twice to reach setup
// would be a worse first touch.
static bool wokeOnThisContact = false;

// Draws the countdown over whatever is on screen. Deliberately the plain
// text display rather than anything composited: this runs while a finger
// is held down and has to be legible instantly, and it is the same surface
// the setup screen itself uses, so there is no second look to maintain.
static void showSetupHoldCountdown(int secondsLeft) {
  displayAdapter.showLines({
      "",
      "SETUP MODE",
      "",
      "in " + String(secondsLeft) + "s",
      "",
      "release to cancel",
  });
}

// ------------------------------------------------- follow-the-finger swipe
//
// A paging swipe drags the boundary between the two screens under the
// finger instead of cutting to the next screen when the finger lifts.
//
// Why this is the answer to the flashing rather than another frame-cost
// optimization (see the navigation rate limit above for that whole arc):
// the artifact is only visible where consecutive frames *differ*, and here
// they differ by a few pixels of translation rather than by a whole screen
// of new colour. Nothing is repainted from scratch mid-turn, so there is no
// full-screen change racing the panel's scan to be seen tearing.
//
// It is a presentation of the existing paging action, not a new input
// concept: the drag only starts if the direction the finger is going is
// bound to next-screen or previous-screen on this screen. Anything else -
// unbound, send-mqtt, the screen menu on swipe-up - keeps the old behaviour
// of classifying once at release. That is what makes it work on both axes
// without a second binding system: a project that pages vertically binds
// swipe-up/down, and the same code follows the finger down the other axis.
static const int DRAG_START_PX = 12;    // past a jittery touch, still instant
static const int DRAG_COMMIT_PX = 120;  // a third of the way across
// A short flick should page even though it never travelled a third: below
// this duration the intent is a throw, not a drag. Deliberately the same
// distance the non-interactive classifier uses, so a quick swipe behaves
// identically whether or not its direction happens to be bound to paging.
static const unsigned long DRAG_FLICK_MS = 250;
static const int DRAG_FLICK_PX = SWIPE_MIN_DISTANCE;
// A flick is better recognised by speed than by distance, because distance
// is exactly what gets lost. Rendering the incoming screen blocks the loop
// for as long as that project's screen takes - 181ms on a real one - and
// the panel cannot be sampled while it does, so a fast swipe is over before
// the follow ever starts and reports whatever tiny travel was seen in the
// millisecond before the render began. Judged on distance it reads as a
// nudge and springs back; judged on speed, 25px in 12ms is unmistakably a
// throw. 500px/s, as travelled*2 >= duration to keep it in integers.
static const int FLICK_PX_PER_SECOND = 500;
static const uint32_t DRAG_SETTLE_MS = 160;

static bool dragDecided = false;  // this gesture has already been offered a drag
static int dragTargetIndex = -1;
static bool dragHorizontal = true;
static int dragTravel = 0;  // signed offset at which the incoming screen is centred
static int dragOffset = 0;
static bool dragAnimCommits = false;
static int dragAnimFrom = 0, dragAnimTo = 0;
static uint32_t dragAnimStartMs = 0;

#if INPUT_DEBUG
// The two points where the loop blocks and the display therefore freezes
// mid-gesture: rendering the incoming screen at the start, and finishing
// the transition at the end. The off-centre watchdog further down cannot
// see either of them - the loop is not running, so there is nothing to
// tick - which is exactly how a fifth of a second of visibly off-centre
// screen passed a green test suite.
static uint32_t lastCommitStallMs = 0;
static uint32_t worstCommitStallMs = 0;
static uint32_t lastStartStallMs = 0;
static uint32_t worstStartStallMs = 0;
// Speed of the last released gesture, so a swipe that did not page can be
// told apart from one that was never seen as fast.
static int lastFlickPxPerSecond = 0;
// How often a drag was abandoned because the gesture turned out to be on
// the other axis. Not an error - it is the correction working - but a
// number that should stay small: every one of them wasted a render of a
// screen that was never shown.
static uint32_t dragAxisAbandoned = 0;

// Every released gesture, not just the last one. Diagnosing "sometimes it
// does not open" needs the failures side by side with the successes from
// the same run - and it cannot be done by polling from outside, because
// each debug request blocks the loop for a few milliseconds and so costs
// exactly the touch samples under suspicion.
struct GestureRecord {
  int16_t dx, dy;
  uint16_t durationMs;
  uint16_t samples;   // touch reads this gesture actually got
  bool dragStarted;
  bool dragAbandoned;
  bool menuWasOpen;   // before this gesture was dispatched
  char verdict[16];
  uint32_t atMs;
  // What the CST816 reported in byte 2 during this contact - the field the
  // firmware treats as "touched", and which a single 2026-08-23 trial
  // suggested also counts additional fingers (one finger short: two
  // fingers read 1, three read 2). One trial is not a characterisation,
  // and this chip has already had a wrong claim about byte 2 sit in the
  // project notes for months, so this records every contact rather than
  // one sample: highest value seen, and a bitmask of every distinct value,
  // so a count that flickers between two numbers is visible as such
  // instead of averaging into a plausible-looking lie.
  uint8_t maxFingers;
  uint8_t fingerMask;  // bit n set = byte 2 read n at least once
};
static const int GESTURE_LOG_SIZE = 24;
static GestureRecord gestureLog[GESTURE_LOG_SIZE];
static uint32_t gestureLogIndex = 0;
static uint16_t gestureSamples = 0;
static bool gestureDragStarted = false;
#endif

static void applyDragOffset(int v) {
  dragOffset = v;
  if (dragHorizontal) canvasPresenter.setTransitionOffset((int16_t)v, 0);
  else canvasPresenter.setTransitionOffset(0, (int16_t)v);
}

// Keeps the pair between "no movement" and "incoming exactly centred", so
// dragging back past the start does not pull a third, non-existent screen
// into view, and overshooting cannot push the incoming one past centre.
static int clampDragOffset(int v) {
  if (dragTravel > 0) return v < 0 ? 0 : (v > dragTravel ? dragTravel : v);
  return v > 0 ? 0 : (v < dragTravel ? dragTravel : v);
}

// Renders a screen into one of the presenter's spare canvases. The renderer
// is pointed at the spare and back again around the call - and it must be
// put back even when the render fails, or every later screen would be drawn
// into a buffer nobody displays.
static bool renderIntoSlot(int slot, int screenIndex) {
  if (screenRenderer == nullptr || !canvasPresenter.canTransition()) return false;
  screenRenderer->setCanvas(&canvasPresenter.slotCanvas(slot));
  const bool ok = screenRenderer->renderScreen(screenIndex);
  screenRenderer->setCanvas(&canvasPresenter.canvas());
  canvasPresenter.setSlotScreen(slot, ok ? screenIndex : -1);
  return ok;
}

// Keeps both neighbours of the current screen rendered and waiting, so that
// starting a drag costs nothing. One screen per call: each render blocks the
// loop for as long as a screen takes (165ms on a real project), and doing
// both back to back would leave a third of a second in which a swipe cannot
// be seen - which is the very fault this exists to remove.
//
// Only while nothing is happening: no drag, no settle, no finger on the
// glass, no menu, and a quiet spell since the last touch. The quiet spell
// matters most - a user paging repeatedly leaves roughly a second between
// gestures, comfortably more than one render, and this must land inside
// that gap rather than under their next swipe.
// Slot 0 is borrowed as the menu's backdrop while it is open. Safe because a
// drag cannot start with the menu up, and prerenderNeighbours() stands down
// for the same reason - nothing else touches the spares in that window.
static const int MENU_BACKDROP_SLOT = 0;
static bool menuBackdropReady = false;
#if INPUT_DEBUG
// What one frame of the menu's fly-in costs, and how many it got. The
// animation runs on wall-clock time, so an expensive frame does not slow it
// down - it just means fewer frames, and at one or two the menu does not
// animate at all, it appears late and all at once.
static uint32_t menuFrameMs = 0;
static uint32_t menuFrames = 0;
#endif

// ------------------------------------------------------ display blanking
//
// The screen turns off - backlight included - after a configured stretch
// with no *human* input. See DeviceSettings.h for why MQTT traffic does
// not count and why the setting lives on the device rather than in the
// project.
//
// The first input afterwards only wakes the screen. It does not press the
// button it happens to land on: this device controls heating and air
// conditioning, and reaching for a dark panel to see what it says must not
// switch something. That is the same rule the reserved setup hold follows
// from the other direction - a hold is unambiguous, a stab in the dark is
// not.
static bool displayIsOff = false;
static uint32_t lastHumanInputMs = 0;
static uint32_t displayOffAfterMs = 0;  // 0 = never, cached from NVS

// The screen this device comes up on and returns to when it goes dark, held
// as the project's own screen *id* and resolved against whatever project is
// actually loaded. See DeviceSettings.h for why an id and not an index: the
// whole value of the feature is that the panel is never found sitting on a
// screen where a mistaken touch matters, and an index that a redeploy
// quietly repoints would deliver exactly that.
static String idleScreenId;  // empty = feature off, cached from NVS

// -1 when nothing is configured, no project is loaded, or the saved id is
// not in this project. Every caller treats -1 as "leave the screen alone",
// which is what the device did before this existed - an id the installed
// project does not have is not a reason to guess at a substitute.
static int resolveIdleScreenIndex() {
  if (idleScreenId.isEmpty()) return -1;
  if (!projectLoader.isLoaded()) return -1;
  const ProjectConfig &project = projectLoader.getProject();
  for (size_t i = 0; i < project.screens.size(); i++) {
    if (project.screens[i].id == idleScreenId) return (int)i;
  }
  return -1;
}

// The screen list the Device tab's dropdown is built from, served by both
// web servers. ArduinoJson rather than hand-built strings because screen
// names are free text from the designer and a quote in one would otherwise
// produce a broken document.
static String buildScreenListJson() {
  JsonDocument doc;
  JsonArray arr = doc.to<JsonArray>();
  if (projectLoader.isLoaded()) {
    const ProjectConfig &project = projectLoader.getProject();
    for (const auto &screen : project.screens) {
      JsonObject o = arr.add<JsonObject>();
      o["id"] = screen.id;
      o["name"] = screen.name;
    }
  }
  String out;
  serializeJson(doc, out);
  return out;
}

static void wakeDisplay() {
  lastHumanInputMs = millis();
  if (!displayIsOff) return;
  displayIsOff = false;
  digitalWrite(LCD_BL, HIGH);
  // Repaint before the backlight would reveal a stale frame - the panel
  // keeps whatever it last received, and that is the screen from before it
  // went dark, which may be minutes old.
  renderScreenIndex(currentScreenIndex);
  Serial.println("[display] woke");
}

static void blankDisplay() {
  if (displayIsOff) return;
  displayIsOff = true;
  digitalWrite(LCD_BL, LOW);
  Serial.println("[display] off after inactivity");

  // Back to the idle screen, *after* the backlight is off: the panel would
  // otherwise be seen flipping pages on its way out, which reads as the
  // device doing something rather than settling. Nothing is visible now, and
  // wakeDisplay() repaints currentScreenIndex - which this just moved - so
  // the next person finds the idle screen rather than wherever the last one
  // left it.
  const int idle = resolveIdleScreenIndex();
  if (idle < 0) return;
  // A navigator left open would otherwise still be up under the dark panel
  // and be the first thing shown on wake, on top of the idle screen.
  if (screenMenu && screenMenu->isActive()) screenMenu->dismiss();
  if (idle != currentScreenIndex) renderScreenIndex(idle);
}

static void updateDisplayBlanking() {
  if (setupModeActive) return;  // the setup screen must stay visible
  if (displayOffAfterMs == 0) return;
  if (displayIsOff) return;
  if (millis() - lastHumanInputMs >= displayOffAfterMs) blankDisplay();
}

// Re-read what the Device tab just saved, so it takes effect without a
// restart - someone who sets a timeout wants to watch it work, and a
// setting that needs a reboot invites power-cycling a device that is
// running fine. Registered with both web servers, because either can be
// the one serving the form (see DeviceSettingsHTML.h).
//
// Deliberately does not jump to a newly chosen idle screen. The person
// saving this is looking at a browser, not at the panel; moving the device
// under them would be a surprise, and the next blank puts it there anyway.
static void applyDeviceSettings() {
  displayOffAfterMs = DeviceSettings::displayOffAfterSeconds() * 1000UL;
  idleScreenId = DeviceSettings::idleScreenId();
  lastHumanInputMs = millis();
  wakeDisplay();
  Serial.printf("[display] blanking now after %us, idle screen \"%s\" (index %d)\n",
                (unsigned)(displayOffAfterMs / 1000), idleScreenId.c_str(),
                resolveIdleScreenIndex());
}

static const uint32_t PRERENDER_IDLE_MS = 400;
static uint32_t lastTouchActivityMs = 0;

static void prerenderNeighbours() {
  if (dragActive || dragAnimating || touchDown) return;
  if (screenMenu && screenMenu->isActive()) return;
  if (!projectLoader.isLoaded() || screenRenderer == nullptr) return;
  if (!canvasPresenter.canTransition()) return;
  if (millis() - lastTouchActivityMs < PRERENDER_IDLE_MS) return;

  const int count = (int)projectLoader.getProject().screens.size();
  if (count < 2) return;
  const int wanted[2] = {
      ((currentScreenIndex + 1) % count + count) % count,
      ((currentScreenIndex - 1) % count + count) % count,
  };
  for (int i = 0; i < 2; i++) {
    if (canvasPresenter.slotHolding(wanted[i]) >= 0) continue;
    renderIntoSlot(canvasPresenter.slotToRenderInto(wanted[i]), wanted[i]);
    return;  // one per pass
  }
}

static void tryStartDrag(int dx, int dy) {
  dragDecided = true;
  if (!canvasPresenter.canTransition() || screenRenderer == nullptr) return;
  if (!projectLoader.isLoaded()) return;
  const ProjectConfig &project = projectLoader.getProject();
  const int count = (int)project.screens.size();
  if (count < 2) return;

  const bool horizontal = abs(dx) > abs(dy);
  const char *id;
  if (horizontal) id = dx > 0 ? "swipe-right" : "swipe-left";
  else id = dy > 0 ? "swipe-down" : "swipe-up";

  const ButtonAction &action = projectLoader.getButtonAction(currentScreenIndex, String(id));
  int delta;
  if (action.type == "next-screen") delta = 1;
  else if (action.type == "previous-screen") delta = -1;
  else return;  // not a paging gesture here - the release classifier handles it

  // The incoming screen enters from the edge the finger is heading toward,
  // regardless of *which* screen it is. A project that binds swipe-left to
  // previous-screen still sees it come in from the right, because that is
  // where the finger is pulling from - the motion follows the hand, not the
  // index arithmetic.
  const int16_t base = (int16_t)((horizontal ? (dx > 0 ? -1 : 1) : (dy > 0 ? -1 : 1)) * SCREEN_SIZE);

  const int target = ((currentScreenIndex + delta) % count + count) % count;

  // Usually already there, rendered while the device was idle - which is the
  // whole point. Rendering here blocks the loop for as long as a screen
  // takes, and the panel is not read at all meanwhile, so the rest of the
  // gesture is lost.
  const uint32_t stallStart = millis();
  int slot = canvasPresenter.slotHolding(target);
  if (slot < 0) {
    slot = canvasPresenter.slotToRenderInto(target);
    if (!renderIntoSlot(slot, target)) return;
  }
#if INPUT_DEBUG
  lastStartStallMs = millis() - stallStart;
  if (lastStartStallMs > worstStartStallMs) worstStartStallMs = lastStartStallMs;
#endif

  dragTargetIndex = target;
  dragHorizontal = horizontal;
  dragTravel = -base;
  canvasPresenter.beginTransition(slot, horizontal ? base : 0, horizontal ? 0 : base);
  dragActive = true;
#if INPUT_DEBUG
  gestureDragStarted = true;
#endif
  applyDragOffset(0);
}

// Runs the settle after the finger lifts - to the destination if the drag
// earned it, back to the start if not. Hand-rolled rather than an lv_anim
// because it finishes by calling renderScreenIndex, which pumps LVGL: doing
// that from an animation's ready callback would re-enter lv_timer_handler.
// Watches for the exact symptom reported from hardware on 2026-08-22: the
// screen left sitting part-way across once a swipe has finished. Anything
// other than dead centre while no transition is running is that fault, so it
// is recorded rather than reasoned about - a state that only lasts a frame
// and one that persists look identical in a screenshot, and completely
// different here.
#if INPUT_DEBUG
static uint32_t offCentreTicks = 0;
static int16_t offCentreLastX = 0, offCentreLastY = 0;
static uint32_t offCentreRunStartMs = 0;
static uint32_t offCentreLongestMs = 0;
#endif

static void watchForOffCentre() {
#if INPUT_DEBUG
  if (dragActive || dragAnimating) {
    offCentreRunStartMs = 0;
    return;
  }
  const int16_t x = canvasPresenter.imageX();
  const int16_t y = canvasPresenter.imageY();
  if (x == 0 && y == 0) {
    offCentreRunStartMs = 0;
    return;
  }
  offCentreTicks++;
  offCentreLastX = x;
  offCentreLastY = y;
  const uint32_t now = millis();
  if (offCentreRunStartMs == 0) offCentreRunStartMs = now;
  const uint32_t runMs = now - offCentreRunStartMs;
  if (runMs > offCentreLongestMs) offCentreLongestMs = runMs;
#endif
}

static void updateDragAnimation() {
  if (!dragAnimating) return;
  const uint32_t elapsed = millis() - dragAnimStartMs;
  if (elapsed < DRAG_SETTLE_MS) {
    // Ease-out: fast away from the finger, gentle into place. Integer math
    // in thousandths - no float, and no overflow at these magnitudes.
    const int32_t t = (int32_t)((elapsed * 1000) / DRAG_SETTLE_MS);
    const int32_t e = 1000 - ((1000 - t) * (1000 - t)) / 1000;
    applyDragOffset(dragAnimFrom + (int)(((int32_t)(dragAnimTo - dragAnimFrom) * e) / 1000));
    return;
  }
  applyDragOffset(dragAnimTo);
  dragAnimating = false;
  // Everything from here to endTransition runs with the display frozen -
  // nothing pumps LVGL in between - so it must be short. Copying the pixels
  // the transition canvas already holds is ~10ms; re-rendering that screen,
  // which is what this did first, is however long the project takes, and on
  // a real one that was 181ms of the screen sitting visibly part-way across
  // at the end of every swipe. Reported from hardware, and the reason the
  // first fix for "not centred" was not enough: the position was right, the
  // display was just still showing the pose from before it was set.
  const uint32_t stallStart = millis();
  if (dragAnimCommits && canvasPresenter.adoptTransitionCanvas()) {
    currentScreenIndex = dragTargetIndex;
  }
  canvasPresenter.endTransition();
#if INPUT_DEBUG
  lastCommitStallMs = millis() - stallStart;
  if (lastCommitStallMs > worstCommitStallMs) worstCommitStallMs = lastCommitStallMs;
#endif
  dragActive = false;
}

// Works from sampleTouch()'s cached reading, never from cst816Read() - see
// its comment for why a second read would find nothing.
static void pollTouchGesturesInner() {
  if (touchIsDown) {
#if INPUT_DEBUG
    touchSampleCount++;
    gestureSamples++;
    lastTouchSampleX = touchX;
    lastTouchSampleY = touchY;
#endif
    if (!touchDown) {
      touchDown = true;
      touchStartX = touchX;
      touchStartY = touchY;
      touchStartMs = millis();
      dragDecided = false;
      wokeOnThisContact = false;
      setupHoldCountdownShown = false;
      setupHoldLastSecond = -1;
      setupHoldConsumed = false;
#if INPUT_DEBUG
      gestureSamples = 0;
      gestureDragStarted = false;
      gestureMaxFingers = 0;
      gestureFingerMask = 0;
#endif
    }
    touchLastX = touchX;
    touchLastY = touchY;
    touchLastDownMs = millis();
    lastTouchActivityMs = touchLastDownMs;

    // Human input: keeps the screen alive, and wakes it if it had gone
    // dark. A wake swallows the rest of this contact - see wakeDisplay.
    const bool wasOff = displayIsOff;
    wakeDisplay();
    if (wasOff) {
      // Marking dragDecided alone was not enough - that only stops the
      // interactive drag from starting, while the release classifier still
      // dispatched a swipe, so the first swipe on a dark screen paged
      // anyway (caught by the HIL suite, 2026-08-23). The contact has to be
      // swallowed at release too, which is what wokeOnThisContact does.
      wokeOnThisContact = true;
      dragDecided = true;
    }

    const int dx = touchX - touchStartX;
    const int dy = touchY - touchStartY;

    // The reserved setup gesture, checked before anything else can claim
    // this contact.
    if (!setupModeActive && abs(dx) <= SETUP_HOLD_MAX_DRIFT && abs(dy) <= SETUP_HOLD_MAX_DRIFT) {
      const unsigned long held = millis() - touchStartMs;
      if (held >= SETUP_HOLD_MS) {
        setupHoldConsumed = true;
        enterSetupMode();
        return;
      }
      if (held >= SETUP_HOLD_FEEDBACK_MS) {
        setupHoldConsumed = true;
        const int secondsLeft = (int)((SETUP_HOLD_MS - held + 999) / 1000);
        if (secondsLeft != setupHoldLastSecond) {
          showSetupHoldCountdown(secondsLeft);
          setupHoldLastSecond = secondsLeft;
          setupHoldCountdownShown = true;
        }
        return;  // nothing else may act on this contact any more
      }
    } else if (setupHoldCountdownShown) {
      // Drifted out of the hold after the countdown had started - treat it
      // as a cancel rather than letting it become a swipe, since the user
      // was told a countdown was running.
      setupHoldConsumed = true;
    }
    // Offered once per gesture, at the first movement worth calling a drag.
    // Once refused - unbound direction, single-screen project, no second
    // canvas - it is not reconsidered, so the decision cannot flip halfway
    // through and start a transition from a stale origin.
    if (!dragDecided && !dragAnimating && !(screenMenu && screenMenu->isActive()) &&
        (abs(dx) >= DRAG_START_PX || abs(dy) >= DRAG_START_PX)) {
      tryStartDrag(dx, dy);
    }
    // Not while the settle is running. This panel latches its last gesture
    // for ~450ms after a lift (see sampleTouch's probe notes), which is
    // longer than the settle - so a phantom touch lands mid-animation, with
    // touchStart taken from its frozen coordinates, and yanks the pair to a
    // raw offset the animation then fights for the rest of its run.
    if (dragActive && !dragAnimating) applyDragOffset(clampDragOffset(dragHorizontal ? dx : dy));
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

  // The setup hold owned this contact: cancelled by releasing, or already
  // acted on. Either way nothing downstream may treat it as a tap or a
  // swipe - see SETUP_HOLD_MS.
  // Woke the screen and nothing more - see wokeOnThisContact. Checked
  // before the setup hold, which has its own reason to swallow a contact.
  if (wokeOnThisContact) {
    wokeOnThisContact = false;
    setupHoldConsumed = false;
#if INPUT_DEBUG
    lastReleaseVerdict = "woke-display-only";
#endif
    return;
  }

  if (setupHoldConsumed) {
    setupHoldConsumed = false;
    if (setupHoldCountdownShown && !setupModeActive) {
      // Put the screen back the way it was; the countdown painted over it.
      setupHoldCountdownShown = false;
      renderScreenIndex(currentScreenIndex);
    }
#if INPUT_DEBUG
    lastReleaseVerdict = "setup-hold-cancelled";
#endif
    return;
  }
#if INPUT_DEBUG
  touchReleaseCount++;
  lastReleaseDx = dx;
  lastReleaseDy = dy;
  lastReleaseDurationMs = duration;
  lastReleaseVerdict = "tap-or-none";
#endif

  // The axis was chosen from the first 12px of movement, which on a real
  // finger is mostly noise: a swipe up that begins with a slight sideways
  // drift looks horizontal at that point and locks the drag to the wrong
  // axis. It then swallows the gesture - the classifier below never runs -
  // so the action actually bound to the swipe never fires. Found on
  // hardware 2026-08-22 as "swipe up no longer opens the screen menu";
  // measured, a 14px sideways start followed by 220px straight up.
  //
  // Judged again here, on the whole gesture, where the intent is
  // unambiguous. The transition is dropped rather than settled: the offset
  // is whatever the wrong axis picked up, a few pixels, so snapping it back
  // is invisible, and it has to be gone before the classifier runs or a
  // paging action from it would be discarded by navigateToScreen.
  if (dragActive && (dragHorizontal ? abs(dy) > abs(dx) : abs(dx) > abs(dy))) {
    canvasPresenter.endTransition();
    dragActive = false;
#if INPUT_DEBUG
    dragAxisAbandoned++;
#endif
  }

  // A followed drag has already done the navigating; all that is left is
  // to settle it one way or the other. It must never also fall through to
  // the classifier below, or the screen would page twice from one gesture.
  if (dragActive) {
    // Measured from the finger, not from dragOffset. The two are the same
    // only while the loop keeps up: rendering the incoming screen blocks it
    // for as long as that project's screen takes (181ms on a real one), and
    // a quick flick delivered during that block would be judged on an offset
    // still sitting near zero and cancelled - the swipe simply doing
    // nothing.
    const int travelled = abs(dragHorizontal ? dx : dy);
    // Guarded by DRAG_START_PX so that jitter over a millisecond - which is
    // arbitrarily fast - cannot page a screen; it has to be movement that
    // was deliberate enough to have started the drag in the first place.
    // Floored at half a normal swipe. The speed rule exists to rescue a fast
    // gesture whose travel was cut short, and pre-rendering the neighbours
    // removed the thing that cut it short - so its job now is only genuinely
    // quick short swipes. At the old floor (the 12px that starts a drag at
    // all) a 14px brush across the glass paged a screen, recorded on
    // hardware in the same run that found the off-axis rule above.
    const bool flicked = travelled >= DRAG_FLICK_PX / 2 &&
                         (unsigned long)(travelled * 1000) >= (unsigned long)FLICK_PX_PER_SECOND * duration;
    const bool commit = travelled >= DRAG_COMMIT_PX ||
                        (duration <= DRAG_FLICK_MS && travelled >= DRAG_FLICK_PX) ||
                        flicked;
    dragAnimFrom = dragOffset;
    dragAnimTo = commit ? dragTravel : 0;
    dragAnimCommits = commit;
    dragAnimStartMs = millis();
    dragAnimating = true;
#if INPUT_DEBUG
    lastReleaseVerdict = commit ? (flicked && travelled < DRAG_COMMIT_PX ? "drag-flick" : "drag-commit")
                                : "drag-cancel";
    lastFlickPxPerSecond = duration > 0 ? (int)((travelled * 1000L) / (long)duration) : 0;
#endif
    return;
  }

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
    if (abs(dx) >= abs(dy) * SWIPE_AXIS_RATIO) buttonId = dx > 0 ? "swipe-right" : "swipe-left";
  } else {
    if (abs(dy) >= abs(dx) * SWIPE_AXIS_RATIO) buttonId = dy > 0 ? "swipe-down" : "swipe-up";
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

// One row per released gesture, whichever of the inner function's several
// exits produced it. Wrapping is deliberate: the release path returns from
// half a dozen places (settled drag, tap, off-axis, menu kept open,
// dispatch) and threading a log call through all of them is how one gets
// missed. touchReleaseCount changing is the single reliable "a gesture just
// ended" signal, and lastReleaseVerdict is final by then.
static void pollTouchGesturesRecorded() {
#if INPUT_DEBUG
  const uint32_t releasesBefore = touchReleaseCount;
  const uint32_t abandonedBefore = dragAxisAbandoned;
  const bool menuWasOpen = screenMenu && screenMenu->isActive();
  const uint16_t samples = gestureSamples;
  const bool dragStarted = gestureDragStarted;
  const uint8_t maxFingers = gestureMaxFingers;
  const uint8_t fingerMask = gestureFingerMask;
  pollTouchGesturesInner();
  if (touchReleaseCount == releasesBefore) return;
  GestureRecord &g = gestureLog[gestureLogIndex % GESTURE_LOG_SIZE];
  g.dx = (int16_t)lastReleaseDx;
  g.dy = (int16_t)lastReleaseDy;
  g.durationMs = (uint16_t)lastReleaseDurationMs;
  g.samples = samples;
  g.dragStarted = dragStarted;
  g.dragAbandoned = dragAxisAbandoned != abandonedBefore;
  g.menuWasOpen = menuWasOpen;
  g.maxFingers = maxFingers;
  g.fingerMask = fingerMask;
  strncpy(g.verdict, lastReleaseVerdict, sizeof(g.verdict) - 1);
  g.verdict[sizeof(g.verdict) - 1] = ' ';
  g.atMs = millis();
  gestureLogIndex++;
#else
  pollTouchGesturesInner();
#endif
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
  closeOpenFrame();
  out += ",\"frameCount\":" + String(frameLogWriteIndex);
  out += ",\"totalChunks\":" + String(totalChunks);
  out += ",\"totalQspiMs\":" + String(totalQspiUs / 1000);
  out += ",\"totalLvglMs\":" + String(totalLvglUs / 1000);
  out += ",\"drawBufInternal\":" + String(drawBufInternal ? "true" : "false");
  out += ",\"navFrameMs\":" + String(navFrameMs);
  out += ",\"dragActive\":" + String(dragActive ? "true" : "false");
  out += ",\"dragAnimating\":" + String(dragAnimating ? "true" : "false");
  out += ",\"dragOffset\":" + String(dragOffset);
  out += ",\"dragTargetIndex\":" + String(dragTargetIndex);
  out += ",\"imageX\":" + String(canvasPresenter.imageX());
  out += ",\"imageY\":" + String(canvasPresenter.imageY());
  out += ",\"transitionX\":" + String(canvasPresenter.transitionImageX());
  out += ",\"transitionY\":" + String(canvasPresenter.transitionImageY());
  out += ",\"transitionHidden\":" + String(canvasPresenter.transitionImageHidden() ? "true" : "false");
  out += ",\"offCentreTicks\":" + String(offCentreTicks);
  out += ",\"offCentreLastX\":" + String(offCentreLastX);
  out += ",\"offCentreLastY\":" + String(offCentreLastY);
  out += ",\"offCentreLongestMs\":" + String(offCentreLongestMs);
  out += ",\"lastCommitStallMs\":" + String(lastCommitStallMs);
  out += ",\"worstCommitStallMs\":" + String(worstCommitStallMs);
  // Pixels where the glass disagrees with what was drawn. Computed here
  // rather than by downloading both bitmaps, because each of those is 388KB
  // and streams for about a second with the loop blocked - far too coarse to
  // measure how long a stale region survives, which is the whole question.
  {
    uint32_t diff = 0;
    if (panelShadow != nullptr) {
      const uint16_t *drawn = canvasPresenter.canvas().getBuffer();
      const uint32_t n = (uint32_t)SCREEN_SIZE * SCREEN_SIZE;
      for (uint32_t i = 0; i < n; i++) {
        if (panelShadow[i] != drawn[i]) diff++;
      }
    }
    out += ",\"panelDiffPixels\":" + String(diff);
    // A cheap fingerprint of the canvas, so "is anything still redrawing
    // the screen?" can be answered by polling instead of by downloading a
    // 388KB bitmap twice and hoping nothing moved in between.
    uint32_t hash = 2166136261u;
    const uint16_t *drawnPixels = canvasPresenter.canvas().getBuffer();
    for (uint32_t i = 0; i < (uint32_t)SCREEN_SIZE * SCREEN_SIZE; i += 7) {
      hash = (hash ^ drawnPixels[i]) * 16777619u;
    }
    out += ",\"canvasHash\":" + String(hash);
    out += ",\"scrollX\":" + String(lv_obj_get_scroll_x(lv_scr_act()));
    out += ",\"scrollY\":" + String(lv_obj_get_scroll_y(lv_scr_act()));
    out += ",\"menuFrameMs\":" + String(menuFrameMs);
    out += ",\"menuFrames\":" + String(menuFrames);
      out += ",\"screenMenuActive\":" + String(screenMenu && screenMenu->isActive() ? "true" : "false");
    out += ",\"flushes\":[";
    {
      uint32_t total = flushLogIndex;
      uint32_t n = total < (uint32_t)FLUSH_LOG_SIZE ? total : (uint32_t)FLUSH_LOG_SIZE;
      for (uint32_t k = 0; k < n; k++) {
        const FlushArea &fa = flushLog[(total - n + k) % FLUSH_LOG_SIZE];
        if (k > 0) out += ",";
        out += "[" + String(fa.x1) + "," + String(fa.y1) + "," + String(fa.x2) + "," +
               String(fa.y2) + "," + String(fa.atMs) + "]";
      }
    }
    out += "]";
  }
  out += ",\"lastFlickPxPerSecond\":" + String(lastFlickPxPerSecond);
  out += ",\"dragAxisAbandoned\":" + String(dragAxisAbandoned);
  // The reserved setup hold, visible so a spurious arming is diagnosable
  // rather than only observable as "the device went into setup mode and I
  // do not know why".
  out += ",\"setupHoldArmed\":" + String(setupHoldCountdownShown ? "true" : "false");
  out += ",\"setupHoldMs\":" + String(touchDown ? (millis() - touchStartMs) : 0);
  out += ",\"setupModeActive\":" + String(setupModeActive ? "true" : "false");
  out += ",\"displayOffAfterSeconds\":" + String(displayOffAfterMs / 1000);
  out += ",\"displayIsOff\":" + String(displayIsOff ? "true" : "false");
  // Both the stored id and what it resolves to right now: a setting that
  // saved fine but names a screen this project does not have looks exactly
  // like one that was never set, unless the two are reported apart.
  out += ",\"idleScreenId\":\"" + idleScreenId + "\"";
  out += ",\"idleScreenIndex\":" + String(resolveIdleScreenIndex());
  out += ",\"idleMs\":" + String(millis() - lastHumanInputMs);
  out += ",\"slotScreens\":[" + String(canvasPresenter.slotScreen(0)) + "," +
         String(canvasPresenter.slotScreen(1)) + "]";
  out += ",\"gestureCount\":" + String(gestureLogIndex);
  out += ",\"gestures\":[";
  {
    uint32_t total = gestureLogIndex;
    uint32_t n = total < (uint32_t)GESTURE_LOG_SIZE ? total : (uint32_t)GESTURE_LOG_SIZE;
    for (uint32_t k = 0; k < n; k++) {
      const GestureRecord &g = gestureLog[(total - n + k) % GESTURE_LOG_SIZE];
      if (k > 0) out += ",";
      out += "{\"dx\":" + String(g.dx) + ",\"dy\":" + String(g.dy);
      out += ",\"ms\":" + String(g.durationMs) + ",\"samples\":" + String(g.samples);
      out += ",\"dragStarted\":" + String(g.dragStarted ? "true" : "false");
      out += ",\"dragAbandoned\":" + String(g.dragAbandoned ? "true" : "false");
      out += ",\"menuWasOpen\":" + String(g.menuWasOpen ? "true" : "false");
      out += ",\"maxFingers\":" + String(g.maxFingers);
      out += ",\"fingerMask\":" + String(g.fingerMask);
      out += ",\"verdict\":\"" + String(g.verdict) + "\",\"at\":" + String(g.atMs) + "}";
    }
  }
  out += "]";
  out += ",\"lastStartStallMs\":" + String(lastStartStallMs);
  out += ",\"worstStartStallMs\":" + String(worstStartStallMs);
  // Which screen the device believes it is on. Without it the only way to
  // check where a burst of navigation ended is to stream a 260KB snapshot
  // and read pixels, which needs a fixture whose screens differ visibly -
  // far too much machinery for "did the rate limit lose the last detent".
  out += ",\"screenIndex\":" + String(currentScreenIndex);
  out += ",\"pendingScreenRender\":" + String(pendingScreenRender ? "true" : "false");
  out += ",\"freeInternalHeap\":" + String((uint32_t)heap_caps_get_free_size(MALLOC_CAP_INTERNAL));
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
  // Turning the knob is human input: it keeps the screen alive, and the
  // first detent on a dark screen only wakes it rather than also paging.
  const bool wasOff = displayIsOff;
  wakeDisplay();
  if (wasOff) return;

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
  canvasPresenter.invalidateSlots();
  if (screenRenderer == nullptr) {
    screenRenderer = new ColorScreenRenderer(projectLoader, &canvasPresenter.canvas());
  }
  // Before the first render, so no render - and in particular no
  // neighbour pre-render during a drag - ever pays for a font parse.
  screenRenderer->preloadFonts();

  topicsSubscribed = false;
  // Onto the idle screen, for the same reason blanking returns to it: the
  // device should come up on something harmless rather than on whatever
  // happens to be first in the project. Falls back to screen 0 when nothing
  // is configured or the saved id is not in this project - which is exactly
  // what this did before the setting existed.
  const int startIndex = resolveIdleScreenIndex();
  const int firstScreen = startIndex < 0 ? 0 : startIndex;
  if (!renderScreenIndex(firstScreen)) {
    Serial.printf("[project] renderScreen(%d) failed\n", firstScreen);
    return;
  }
  Serial.printf("[project] rendered screen %d%s\n", firstScreen,
                startIndex < 0 ? "" : " (idle screen)");
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
  // What this firmware can read, as one major.minor (DeviceInfo.h). The
  // designer's deploy dialog checks it *before* uploading, so a device on
  // an older major says so in the dialog rather than refusing the download
  // afterwards, where the only evidence is a serial log nobody is watching.
  doc["systemGeneration"] = String(SYSTEM_GENERATION_MAJOR) + "." + String(SYSTEM_GENERATION_MINOR);
  // ddfHash+url are what make this device appear under "Announced Devices"
  // at all - without them the designer treats it as firmware that publishes
  // no DDF and skips it. The hash is the identity of the bytes served at
  // url, and the designer verifies one against the other: if they disagree
  // the fetch is refused rather than cached, so an announcement that lies
  // is visible instead of quietly poisoning the designer's copy.
  doc["ddfHash"] = DDF_HASH;
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
  // Any screen may show this value, so every pre-rendered neighbour is now
  // a picture of how that screen used to look - including the menu's
  // backdrop, which is one of those slots.
  canvasPresenter.invalidateSlots();
  menuBackdropReady = false;

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
#if INPUT_DEBUG
      testInterface.setPanelShadowHandler([]() -> const uint16_t * { return panelShadow; });
      // Guarded with the rest of the diagnosis surface: the suppression
      // window it writes to only exists in that build, so registering it
      // unconditionally would break the moment INPUT_DEBUG is turned off -
      // a build failure hiding in a flag nobody flips often.
      testInterface.setTouchHandler([](int16_t x, int16_t y, bool down) {
        touchX = x;
        touchY = y;
        touchIsDown = down;
        // A lift ends the hold rather than extending it, so the release
        // debounce runs against the real panel (also reading idle) instead
        // of against 400ms of frozen injected state.
        touchInjectUntilMs = down ? millis() + TOUCH_INJECT_HOLD_MS : 0;
      });
#endif
      // Apply a saved setting immediately. Without this the value sits in
      // NVS and the running firmware keeps the one it read at boot -
      // someone changes the timeout, watches nothing happen, and concludes
      // it did not save.
      testInterface.setSettingsChangedHandler(applyDeviceSettings);
      testInterface.setScreenListHandler(buildScreenListJson);
      testInterface.setInputHandler([](const String &buttonId, bool deferRender) {
        dispatchInput(buttonId.c_str());
        // The navigation rate limit is there for a physical turn, where the
        // next detent is milliseconds away. An HTTP caller is not turning
        // anything: it dispatches one input and photographs the panel the
        // moment this returns, so a withheld frame would hand it the
        // previous screen - a race that reads exactly like "the action never
        // ran". Forced out here rather than left to the leading edge, which
        // only happens to cover it while navFrameMs stays below the time a
        // snapshot takes.
        if (!deferRender) renderPendingScreen();
      });
#if INPUT_DEBUG
      // GET /api/debug?set=touchPeriodMs=30 adjusts the touch sample
      // period per request - the polling-rate hypothesis is tested over
      // HTTP rather than one reflash per candidate value.
      testInterface.setDebugHandler([](const String &setArg) {
        if (setArg.startsWith("touchPeriodMs=")) {
          long v = setArg.substring(14).toInt();
          if (v >= 1 && v <= 500) touchPeriodMs = (uint32_t)v;
        } else if (setArg.startsWith("navFrameMs=")) {
          // 0 lifts the limit entirely (every detent repaints), which is how
          // the before/after can be compared without reflashing.
          long v = setArg.substring(11).toInt();
          if (v >= 0 && v <= 2000) navFrameMs = (uint32_t)v;
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
  displayOffAfterMs = DeviceSettings::displayOffAfterSeconds() * 1000UL;
  lastHumanInputMs = millis();
  // Read before the project loads, because loadAndRenderProject() renders
  // the idle screen and has to know which one that is.
  idleScreenId = DeviceSettings::idleScreenId();
  Serial.printf("[display] blanking after %us of inactivity%s\n",
                (unsigned)(displayOffAfterMs / 1000), displayOffAfterMs == 0 ? " (never)" : "");
  Serial.printf("[display] idle screen \"%s\"%s\n", idleScreenId.c_str(),
                idleScreenId.isEmpty() ? " (none - stays where it is)" : "");

  Wire.begin(TOUCH_SDA, TOUCH_SCL);
  cst816Init();

  // 80MHz, up from 40MHz (2026-08-21). The QSPI push is the largest single
  // cost of a screen change (22.2ms of 39.9ms) and it is not memory-bound -
  // moving the draw buffer into internal RAM barely touched it - so the
  // clock is the next thing to rule in or out. A panel that cannot keep up
  // shows it immediately and unmistakably as garbled pixels, and the
  // measurement in GET /api/debug says whether the rate actually changed at
  // all, which a datasheet reading alone would not.
  bool gfxOk = gfx->begin(80000000);
  Serial.printf("gfx->begin() = %s\n", gfxOk ? "true" : "false");
  gfx->fillScreen(RGB565_BLACK);

  // Formats on first boot; holds /PROJECT and the staged upload zip.
  if (!LittleFS.begin(true)) {
    Serial.println("LittleFS mount failed - project install/load unavailable");
  }
  configStorage.begin();

  lv_init();
  // This buffer is read twice per chunk and from two directions: LVGL walks
  // the canvas into it, then draw16bitRGBBitmap streams it out to the panel.
  // In PSRAM both halves pay external-memory latency, and the DMA out of it
  // cannot run at full rate. Measured with it in PSRAM (2026-08-21): 24.6ms
  // of QSPI per full frame against a theoretical ~13ms at 40MHz x4.
  //
  // 360x60x2 = 43200 bytes, which internal DRAM has room for at ~50% static
  // usage. Falling back rather than failing if it does not: a slower flush
  // is a far better outcome than a device that will not start, and the
  // fallback is visible in GET /api/debug's drawBufInternal so a timing
  // measurement can never be read against the wrong allocation.
  size_t bufPixels = SCREEN_SIZE * 60;
  const size_t bufBytes = bufPixels * sizeof(lv_color_t);
  lvBuf1 = (lv_color_t *)heap_caps_malloc(bufBytes, MALLOC_CAP_INTERNAL | MALLOC_CAP_DMA);
  drawBufInternal = lvBuf1 != nullptr;
  if (!drawBufInternal) {
    Serial.printf("[lvgl] %u-byte draw buffer did not fit in internal RAM - using PSRAM (slower flush)\n",
                  (unsigned)bufBytes);
    lvBuf1 = (lv_color_t *)heap_caps_malloc(bufBytes, MALLOC_CAP_SPIRAM);
  }
  Serial.printf("[lvgl] draw buffer: %u bytes in %s RAM\n", (unsigned)bufBytes,
                drawBufInternal ? "internal" : "PS");
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

#if INPUT_DEBUG
  // Allocated before the first flush can happen. A failure is not fatal -
  // /panel.bmp simply says it has nothing, and everything else runs as
  // usual.
  panelShadow = (uint16_t *)heap_caps_malloc((size_t)SCREEN_SIZE * SCREEN_SIZE * sizeof(uint16_t),
                                             MALLOC_CAP_SPIRAM);
  if (panelShadow == nullptr) {
    Serial.println("[panel] no shadow buffer - GET /panel.bmp will be unavailable");
  } else {
    memset(panelShadow, 0, (size_t)SCREEN_SIZE * SCREEN_SIZE * sizeof(uint16_t));
  }
#endif

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

  // The setup portal's Device tab is the same form the running device
  // serves at /settings, so it is filled and applied the same way. Wired
  // before setupWiFi(), which is what can put the device straight into
  // setup mode on a board that has never seen a network.
  wifiSetupServer.setScreenListHandler(buildScreenListJson);
  wifiSetupServer.setSettingsChangedHandler(applyDeviceSettings);

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
  pumpLvgl();

  if (setupModeActive) {
    wifiSetupServer.handleClient();

    // Tap below the QR code to leave setup mode again.
    //
    // The band rather than a drawn button: the code fills the middle of a
    // 46mm round screen and there is no room for one that would still be
    // hittable. Everything under the code is the target, and the screen
    // says so on its last line.
    //
    // Leaving restarts rather than tearing the AP down and resuming in
    // place - the same choice the timeout makes, and for the same reason:
    // a restart has one well-tested path back into normal operation, while
    // unwinding AP mode by hand has none.
    //
    // An accidental cancel costs a reboot and lands where whoever touched
    // it presumably wanted to be anyway, so this is deliberately a plain
    // tap with no confirmation.
    static bool setupTapDown = false;
    static int16_t setupTapStartY = 0;
    if (touchIsDown) {
      if (!setupTapDown) {
        setupTapDown = true;
        setupTapStartY = touchY;
      }
    } else if (setupTapDown) {
      setupTapDown = false;
      // Everything below the QR code, asked of the code that drew it - a
      // hardcoded y here is what broke this once already when the layout
      // moved. The credential lines are inside the band too; they are not
      // interactive, the screen says what a tap does, and cancelling costs
      // a restart that lands where an accidental tapper wanted to be.
      const int16_t bandTop = (int16_t)(displayAdapter.qrScreenTextTop() - 6);
      const bool inCancelBand = bandTop > 0 && setupTapStartY > bandTop && touchY > bandTop - 20;
      if (inCancelBand) {
        Serial.println("[setup] cancelled on the device - restarting");
        delay(100);
        ESP.restart();
      }
    }

    // The portal's own Cancel button, for someone already looking at the
    // page who decided not to change anything.
    if (wifiSetupServer.exitRequested()) {
      Serial.println("[setup] cancelled from the portal - restarting");
      delay(300);  // let the response reach the browser first
      ESP.restart();
    }

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

  pollTouchGesturesRecorded();
  updateDragAnimation();
  updateDisplayBlanking();
  prerenderNeighbours();
  watchForOffCentre();
  pollEncoder();

  // The rate limit's trailing edge - see navigateToScreen. Placed after this
  // pass's input polling so a detent that just arrived is already included
  // in the frame it triggers, and so the screen a turn ends on is always
  // painted even when the last detent fell inside a closed window.
  if (pendingScreenRender && (millis() - lastNavRenderMs) >= navFrameMs) {
    renderPendingScreen();
  }

  // No serial reporting here on purpose - the recorder is read over
  // GET /api/debug, so nothing needs a COM port open (see the recorder's
  // own comment for why that matters on this board).

  // The overlay owns the frame while it is up: it re-renders the current
  // screen and composites the tablets on top in one pass, so nothing else
  // may render underneath it. Doing both would show a bare frame between
  // them - the M5 Dial hit exactly that as a visible flicker on every jump.
  // The overlay redraws from scratch each frame and needs the screen
  // underneath put back first. It used to re-render it - 165ms a frame on a
  // real project, so a 250ms fly-in got two frames and the menu took most of
  // a second to appear. The screen underneath does not change while the menu
  // is up, so it is copied aside once and restored per frame instead.
  if (screenMenu && screenMenu->isActive()) {
    if (!menuBackdropReady) {
      menuBackdropReady = canvasPresenter.copyCanvasToSlot(MENU_BACKDROP_SLOT);
      // Borrowed, so the neighbour cache must not think a screen is still in
      // there. prerenderNeighbours() refills it once the menu is gone.
      if (menuBackdropReady) canvasPresenter.setSlotScreen(MENU_BACKDROP_SLOT, -1);
    }
    const uint32_t menuFrameStart = millis();
    const bool drew = screenMenu->update([]() {
      if (!menuBackdropReady || !canvasPresenter.copySlotToCanvas(MENU_BACKDROP_SLOT)) {
        // No spare to borrow: fall back to the old way rather than leaving
        // the previous frame's tablets smeared across the screen.
        if (screenRenderer) screenRenderer->renderScreen(currentScreenIndex);
      }
    });
    if (drew) {
      canvasPresenter.present();
#if INPUT_DEBUG
      menuFrameMs = millis() - menuFrameStart;
      menuFrames++;
#endif
    }
  } else {
    menuBackdropReady = false;
#if INPUT_DEBUG
    menuFrames = 0;
#endif
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
