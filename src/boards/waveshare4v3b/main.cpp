// Firmware entry point for the Waveshare ESP32-S3-Touch-LCD-4.3B.
//
// The bring-up scaffolding this file grew during the port has been removed
// now that the board runs real projects: the colour-bar test pattern, the
// backlight blink, the fill-rate benchmark, the font sample sheet, the touch
// dots and the NVS tap counters. Each earned its keep once and then started
// costing something - they flickered through every boot and one of them was
// still reachable by tapping. They are in the history if a measurement needs
// repeating.
//
// NO FINGER-TRACKING SWIPE ON THIS BOARD. That is a decision, and the first
// version of this note got the reason wrong, so here is the measured one.
//
// The claim used to be that the panel's memory bus was full: 72ms per frame
// against "a measured 43ms floor". Then the factory LVGL demo was flashed
// back and slid roughly 80% of the screen smoothly - so the bus plainly was
// not full, and a measurement of one implementation had been generalised into
// a property of the hardware. PanelBenchmark.h now lives next to this file so
// the question is settled with numbers instead of a story; run it with
// GET /api/debug?set=bench=1.
//
// What it says (2026-09-01, 800x480, 750KB a frame):
//
//   fb_memset      25.3 ms   write the framebuffer, nothing else
//   psram_read     27.1 ms   read a rendered canvas, nothing else
//   fb_from_sram   25.3 ms   internal SRAM -> framebuffer = 39 fps
//   fb_from_psram  66.2 ms   canvas -> framebuffer
//   compose        65.7 ms   one real swipe frame = 15.2 fps
//
// So the hardware does 39 fps full-screen - if the source sits in internal
// SRAM. That is exactly what LVGL does: it re-renders cheap widgets into a
// small SRAM buffer every frame and pushes only the result, 750KB in one
// direction. We cannot, because a real project takes 782ms to draw, so our
// screens must be pre-rendered and cached in PSRAM - and then every frame
// both reads 750KB and writes 750KB. Twice the traffic, 2.6x the time. The
// gap is structural, not a missing optimisation.
//
// Three ways out were measured and none worked. Staging the copy through a
// 16KB SRAM block, on the theory that interleaved read and write bursts were
// costing bus turnarounds, came out at 66.3ms - identical, so that theory is
// dead. Handing the copy to GDMA came out at 133.9ms, twice as slow as the
// CPU, because it arbitrates for the same memory controller the LCD is
// already streaming through. The third is real but insufficient: our data
// cache has 32-byte lines where Waveshare's own ESP-IDF demo sets 64
// (CONFIG_ESP32S3_DATA_CACHE_LINE_64B), which the Arduino framework cannot
// change without rebuilding it as an ESP-IDF component. It would only help
// the read half, so the ceiling would still be around 22 fps.
//
// Half-row compositing during motion did reach ~28 fps by halving the
// traffic, and was judged on the glass: "sieht nicht so toll aus" - losing
// vertical resolution during a horizontal movement is the most visible way to
// spend that. So screen changes stay on a tab-control or a button here, where
// nothing is expected to track a finger. A short non-tracking transition at
// 15 fps has NOT been tried and is the one option still open.
//
// Pin map and RGB timings are Waveshare's own, taken from
// waveshareteam/ESP32-S3-Touch-LCD-4.3B (Apache-2.0),
// examples/Arduino/examples/08_DrawColorBar/waveshare_lcd_port.h, and
// cross-checked line by line against the pin table in the board's wiki. Both
// sources agree; nothing here is inferred from the 1.8" knob port, which is
// the mistake that produced four hardware bugs on the last one.
#include <Arduino.h>
#include <Wire.h>
#include <WiFi.h>
#include <LittleFS.h>
#include "Ch422g.h"
#include "Gt911.h"
#include "TestProjectJson.h"
#include "PanelDisplayAdapter.h"
#include "../../NvsConfigStorage.h"
#include "../../WiFiSetupServer.h"
#include "../../MqttClient.h"
#include "../../DeployManager.h"
#include "../../TestInterfaceServer.h"
#include "RgbPanel.h"
#include "PanelBenchmark.h"
#include "../../project/RenderProfile.h"
#include "ArcSelfTest.h"
#include "FollowSwipe.h"
#include "../../DeviceProtocol.h"
#include "../../ClippedCanvas16.h"
#include "../../project/ProjectLoader.h"
#include "../../project/ColorScreenRenderer.h"

// --- panel geometry -------------------------------------------------------
static const int16_t PANEL_W = 800;
static const int16_t PANEL_H = 480;

// --- RGB timings (ST7262, 800x480) ---------------------------------------
static const int32_t PCLK_HZ = 16 * 1000 * 1000;
static const uint16_t HSYNC_PULSE_WIDTH = 4;
static const uint16_t HSYNC_BACK_PORCH = 8;
static const uint16_t HSYNC_FRONT_PORCH = 8;
static const uint16_t VSYNC_PULSE_WIDTH = 4;
static const uint16_t VSYNC_BACK_PORCH = 8;
static const uint16_t VSYNC_FRONT_PORCH = 8;

// Bounce buffer, in pixels. The RGB peripheral streams the framebuffer out
// of PSRAM continuously; without a bounce buffer through internal SRAM the
// picture drifts sideways under bus contention. Waveshare uses width * 10.
static const size_t BOUNCE_BUFFER_PX = PANEL_W * 10;

// --- shared I2C bus -------------------------------------------------------
// Touch, the CH422G expander and the RTC all sit on the same two lines.
static const int I2C_SDA = 8;
static const int I2C_SCL = 9;
static const int TP_INT = 4;

// --- pin map --------------------------------------------------------------
// Waveshare's DATA0..DATA15 is an RGB565 bus in the order B, G, R:
// DATA0-4 = blue, DATA5-10 = green, DATA11-15 = red. Arduino_GFX wants them
// split, hence the regrouping. The wiki names the same lines B3..B7, G2..G7
// and R3..R7 - the panel's own bit numbering, same physical pins.
static const RgbPanel::Pins PANEL_PINS = {
    /* de */ 5, /* vsync */ 3, /* hsync */ 46, /* pclk */ 7,
    /* r0..r4 */ {1, 2, 42, 41, 40},
    /* g0..g5 */ {39, 0, 45, 48, 47, 21},
    /* b0..b4 */ {14, 38, 18, 17, 10}};

static const RgbPanel::Timing PANEL_TIMING = {
    PCLK_HZ,
    HSYNC_PULSE_WIDTH, HSYNC_BACK_PORCH, HSYNC_FRONT_PORCH,
    VSYNC_PULSE_WIDTH, VSYNC_BACK_PORCH, VSYNC_FRONT_PORCH};

static RgbPanel panel;

// No panel reset line on this board - LCD_RST is on the expander and the
// panel comes up without it, as Waveshare's own example also assumes.

static Ch422g expander;
static Gt911 touch;

// --- rendering ------------------------------------------------------------
// The same ColorScreenRenderer the knob and the M5 Dial run, unchanged. Its
// output goes into an RGB565 canvas; the only board-specific part is where
// that canvas then goes - here a straight blit to the RGB panel, with no
// LVGL in the path. The knob composites through LVGL because it wants
// transitions; that costs a second and third 768KB buffer here and is a
// decision to take with measurements, not before them.
static ProjectLoader projectLoader;
static ClippedCanvas16 *canvas = nullptr;
static ColorScreenRenderer *renderer = nullptr;
static int currentScreen = 0;
// The transition's second surface and the transition itself. See
// FollowSwipe.h for why a full-screen follow exists on a board that
// measures 15fps for one.
static ClippedCanvas16 *spareCanvas = nullptr;
static FollowSwipe swipe;
// A value that arrived while a transition was running. The value itself is
// stored the moment it lands; only the drawing waits.
// Which broker this device is pointed at. Read once at boot with the rest
// of the credentials and kept here, because "mqtt NOT connected" says
// nothing about who it failed to reach - and the only other way to find out
// is a serial monitor, whose attach resets the board. It also makes the
// setting restorable: POST /api/mqtt can overwrite it and nothing could
// read it back before changing it.
static String brokerHost;
static uint16_t brokerPort = 0;

static bool renderPending = false;
static unsigned long lastTopicRenderMs = 0;
// A render plus its blit costs 180ms. Drawing every value as it lands left
// the loop no time for anything else, so values are coalesced to at most
// one render in this window. The first value after a quiet spell is still
// drawn at once - the window has already passed by then.
static const unsigned long TOPIC_RENDER_MIN_MS = 250;

// Where the swipes that never happened went. Half of twenty were missing
// twice in a row while every recorded one was clean, and the two possible
// explanations need different fixes: either the contact is never seen at
// all, because the loop was busy elsewhere for the whole 400ms of it, or it
// is seen and never recognised. Counting both settles it in one run.
// The finger, as opposed to the controller. A GT911 report says what the
// glass looks like at that instant; the silence between reports says
// nothing at all, so the state has to be held here rather than re-derived
// from every read. See Gt911::poll().
// How far a contact wandered, so a release can tell a tap from a gesture
// that was never wide enough to become one.
static int16_t contactDrift = 0;
static bool fingerDown = false;
static int16_t touchX = 0;
static int16_t touchY = 0;
static unsigned long lastTouchReportMs = 0;
// Longer than any gap this controller leaves between reports, short enough
// that a report lost for good cannot hold a gesture open.
static const unsigned long TOUCH_LOST_MS = 150;

// How much of the time the loop is inside a value render and therefore
// blind to the glass. A render plus its blit is 180ms and values arrive
// faster than that on this vehicle, so this is the number that decides
// whether a 300ms gesture can be seen at all.
static uint32_t topicRenders = 0;
static uint32_t topicRenderUs = 0;
static unsigned long dutyWindowStartMs = 0;
// Diagnostic only: lets a gesture be judged with nothing else touching the
// panel. Not a setting - it survives nothing, and the board renders again
// after a restart.
static bool topicRenderEnabled = true;

// The measuring square owns the panel while it is up. Without this it lasted
// until the next touch: a press repaints its object from the project, and a
// tap on a button repaints everything - and a ruler in one hand and a finger
// on the glass is exactly the situation it exists for.
static bool measuringPattern = false;

// Where a turn of the loop goes. Twelve of twenty gestures never produced a
// contact at all while the board was rendering nothing, so something else
// is holding the loop for longer than a finger stays on the glass. Guessing
// which has now been wrong twice, so each section is timed instead.
static uint32_t loopTurns = 0;
static uint32_t webUs = 0, mqttUs = 0, touchUs = 0;
static uint32_t worstWebUs = 0, worstMqttUs = 0, worstTouchUs = 0;
static uint32_t worstTurnUs = 0;

static uint32_t contactCount = 0;
static uint32_t contactSamples = 0;
static uint32_t missedContacts = 0;
static uint32_t missedMaxSamples = 0;
static int missedMaxDx = 0;

// --- networking -----------------------------------------------------------
// All six of these compiled for this board without a single change. What was
// missing was only the wiring, and that lived in the knob's main.cpp rather
// than anywhere shareable - which is why DeviceProtocol.h exists.
static NvsConfigStorage configStorage;
static PanelDisplayAdapter *displayAdapter = nullptr;
static WiFiSetupServer *wifiSetupServer = nullptr;
static MqttClient mqttClient;
static DeployManager deployManager;
static TestInterfaceServer testInterface;
static DeviceProtocol protocol(mqttClient);
static bool topicsSubscribed = false;
static bool setupModeActive = false;

// The reserved setup gesture, same numbers as the knob: hold a finger still
// for five seconds, with a countdown from one second in so it is
// discoverable rather than secret. It has to exist - once a board has
// credentials it never starts its AP again, so without this there is no way
// back into setup short of erasing NVS. Identical to the knob's on purpose:
// entering setup is one of the cross-device building blocks that were agreed
// to be the same everywhere rather than per board.
static const unsigned long SETUP_HOLD_MS = 5000;
static const unsigned long SETUP_HOLD_FEEDBACK_MS = 1000;
static const int SETUP_HOLD_MAX_DRIFT = 40;
// A finger on an 800x480 panel is not a mouse pointer, and a button here is
// finger sized. Well below the 12px that starts a horizontal drag, so a tap
// and a swipe can never both fire from one contact.
static const int TAP_MAX_DRIFT = 10;

// What the last tap hit, in words. Serial is not a neutral observer on this
// board - opening the port resets it - and the effect of a tap is usually a
// published command, which is invisible when the broker is away. Without
// this, "did the tap land" is unanswerable from outside.
static String lastTap = "none since boot";
// What the last feedback repaint cost. Claimed at roughly 80ms before it
// was built; kept on the device so the claim stays checkable.
static String lastFeedback = "none since boot";

// Touch feedback. The shared renderer has drawn all three of these states
// from the start and this board had never asked for any of them: a finger
// went down on a switch, a command went out, and the panel looked exactly
// as it had a moment earlier. On a vehicle, where the answer comes back
// from a broker over a link that may be down, "nothing happened" and "I
// have your tap and I am waiting" have to look different.
static String pressedButtonId;
static String pressedSwitchId;
static int pressedSwitchIndex = -1;
static String pendingSwitchId;
static int pendingSwitchIndex = -1;
static unsigned long pendingSinceMs = 0;
// Long enough for a broker on the same network to answer several times
// over, short enough that a dead link does not leave a switch waiting for
// ever on a screen nobody is watching.
static const unsigned long PENDING_TIMEOUT_MS = 4000;
// The rectangle the feedback lives in. Only this is redrawn, because a
// button lighting up does not justify the whole screen.
static int16_t feedbackX = 0, feedbackY = 0, feedbackW = 0, feedbackH = 0;
static bool feedbackActive = false;
// The pending mark keeps its OWN rectangle, and that is the whole fix for a
// bug reported from the glass: tap three tiles in a row and only the last
// one's mark ever cleared. Both states used to share one rectangle, so the
// press on tile three moved it there, and when tile three's mark expired it
// repainted tile three - leaving marks on one and two standing for ever,
// because nothing pointed at them any more.
static int16_t pendingX = 0, pendingY = 0, pendingW = 0, pendingH = 0;
// Declared here because the test interface is wired up before the feedback
// helpers are defined, and a screen forced from outside invalidates them
// exactly as one chosen by a finger does.
static void forgetTouchFeedback();
// Counts full renders, so a caller can tell whether something it invoked
// already repainted the screen and a region render would be waste.
static uint32_t renderGeneration = 0;

// A touch sample posted to /api/touch, standing in for the panel.
//
// Exists because a gesture is not otherwise reachable from a test. Posting
// an action says whether the action works; posting a series of these drives
// the real detector, the real dispatch and the real transition - which is
// where every fault of the last two days actually lived.
static int16_t injectX = 0, injectY = 0;
static bool injectDown = false;
static bool injectLiftOnce = false;
static unsigned long injectUntilMs = 0;
// The knob holds an injected sample for 400ms. This board needs longer, and
// the reason is its own snapshot: 1.15MB against the knob's 259KB, roughly
// a second on the wire. A press that expires while the photograph of it is
// still being sent cannot be photographed - which is how the press feedback
// first appeared to do nothing at all. Still short enough that a test which
// stops posting cannot wedge the panel into a phantom contact.
static const unsigned long TOUCH_INJECT_HOLD_MS = 2000;
static bool holdActive = false;
static int16_t holdStartX = 0, holdStartY = 0;
static unsigned long holdStartMs = 0;
static int holdLastSecondShown = -1;
static bool holdConsumed = false;

static void setupRenderStack();
static void setupNetworking();
static void enterSetupMode();

static void renderAndPresent(const char *reason)
{
  if (!renderer || !canvas) return;
  uint32_t t0 = micros();
  bool ok = renderer->renderScreen(currentScreen, pressedButtonId, pendingSwitchId, pendingSwitchIndex,
                                   pressedSwitchId, pressedSwitchIndex);
  renderGeneration++;
  uint32_t t1 = micros();
  panel.blit(canvas->getBuffer());
  uint32_t t2 = micros();
  Serial.printf("[4v3b] render(screen %d, %s): %s | draw %lums, blit %lums\n",
                currentScreen, reason, ok ? "ok" : "FAILED",
                (unsigned long)((t1 - t0) / 1000), (unsigned long)((t2 - t1) / 1000));
}

static void setupRenderStack()
{
  if (!LittleFS.begin(true))
  {
    Serial.println("[4v3b] LittleFS mount FAILED");
    return;
  }

  // A deployed project wins, always. The built-in fixture exists only so
  // this board had something to render before it could receive anything -
  // and for a while it also silently outranked a real deploy, because this
  // ran unconditionally and never looked at /PROJECT. A deploy would
  // download, verify, install and reboot correctly, and the panel would come
  // back showing the test pattern as if nothing had happened.
  bool loaded = projectLoader.loadProject();
  if (loaded)
  {
    Serial.println("[4v3b] loaded deployed project from /PROJECT");
  }
  else
  {
    Serial.println("[4v3b] no deployed project - falling back to the built-in fixture");
    File f = LittleFS.open("/render-smoke.json", "w");
    if (!f)
    {
      Serial.println("[4v3b] could not write the fallback project");
      return;
    }
    f.print(TEST_PROJECT_JSON);
    f.close();
    loaded = projectLoader.loadProject("/render-smoke.json");
  }
  if (!loaded)
  {
    Serial.println("[4v3b] loadProject FAILED");
    return;
  }
  Serial.printf("[4v3b] project \"%s\", %ux%u, %u screen(s)\n",
                projectLoader.getProject().name.c_str(),
                projectLoader.getProject().screenWidth,
                projectLoader.getProject().screenHeight,
                (unsigned)projectLoader.getProject().screens.size());

  // 800*480*2 = 768000 bytes. Internal SRAM has nothing like that free, so a
  // non-null buffer is itself the proof it landed in PSRAM - but report
  // where, because "it worked" and "it worked for the reason I assumed" are
  // different claims.
  canvas = new ClippedCanvas16(PANEL_W, PANEL_H);
  if (!canvas || !canvas->getBuffer())
  {
    Serial.printf("[4v3b] FATAL: %u-byte canvas allocation failed\n",
                  (unsigned)(PANEL_W * PANEL_H * 2));
    return;
  }
  Serial.printf("[4v3b] canvas at %p (%s), psram free %u\n", canvas->getBuffer(),
                esp_ptr_external_ram(canvas->getBuffer()) ? "PSRAM" : "internal SRAM",
                (unsigned)ESP.getFreePsram());

  renderer = new ColorScreenRenderer(projectLoader, canvas);

  // A second canvas of the same size, so the incoming screen can be drawn
  // while the outgoing one is still on the panel. Taken at boot rather than
  // at the first gesture: there are 6.5MB of PSRAM free and an allocation
  // that failed mid-swipe would have nowhere to go.
  spareCanvas = new ClippedCanvas16(PANEL_W, PANEL_H);
  if (spareCanvas && spareCanvas->getBuffer())
  {
    swipe.attach(&panel, &canvas, spareCanvas, renderer, &currentScreen,
                 (int)projectLoader.getProject().screens.size(), PANEL_W, PANEL_H);
    Serial.printf("[4v3b] swipe %s: %u screens, spare canvas at %p, psram free %u\n",
                  swipe.ready() ? "ready" : "idle (needs two screens)",
                  (unsigned)projectLoader.getProject().screens.size(), spareCanvas->getBuffer(),
                  (unsigned)ESP.getFreePsram());
  }
  else
  {
    Serial.println("[4v3b] spare canvas allocation failed - no transitions");
  }

  renderAndPresent("boot");
}

static void subscribeToProjectTopics()
{
  if (topicsSubscribed || !projectLoader.isLoaded() || !mqttClient.isConnected()) return;
  std::vector<String> topics;
  for (const auto &t : projectLoader.getProject().topics) topics.push_back(t.topic);
  if (!topics.empty()) mqttClient.subscribeToKeys(topics);
  topicsSubscribed = true;
  Serial.printf("[4v3b] subscribed to %d project topic(s)\n", (int)topics.size());
}

static void enterSetupMode()
{
  if (setupModeActive || !wifiSetupServer) return;
  Serial.println("[4v3b] entering setup mode - starting AP");
  setupModeActive = true;
  // The test interface owns port 80 in normal operation and the setup server
  // wants the same port, so it has to be stopped rather than left to fail a
  // bind nothing checks. In setup mode the device serves no snapshots and
  // accepts no deploys anyway.
  testInterface.stop();
  wifiSetupServer->startAP();
}

static void setupNetworking()
{
  if (!canvas) return;

  displayAdapter = new PanelDisplayAdapter(*canvas, panel, PANEL_W, PANEL_H);
  configStorage.begin();
  wifiSetupServer = new WiFiSetupServer(*displayAdapter, configStorage);

  WiFiCredentials creds;
  if (!configStorage.loadWiFiCredentials(creds) || creds.ssid.isEmpty())
  {
    Serial.println("[4v3b] no WiFi credentials - starting setup AP");
    setupModeActive = true;
    wifiSetupServer->startAP();
    return;
  }

  Serial.printf("[4v3b] connecting to \"%s\"\n", creds.ssid.c_str());
  WiFi.mode(WIFI_STA);
  WiFi.begin(creds.ssid.c_str(), creds.password.c_str());
  for (int i = 0; i < 40 && WiFi.status() != WL_CONNECTED; i++) delay(250);

  if (WiFi.status() != WL_CONNECTED)
  {
    Serial.println("[4v3b] WiFi connect FAILED");
    displayAdapter->showLines({"WLAN nicht erreichbar", creds.ssid});
    return;
  }
  Serial.printf("[4v3b] WiFi ok, ip %s\n", WiFi.localIP().toString().c_str());

  // The snapshot endpoint reads this canvas directly, which is what gives
  // this board a HIL comparison at all.
  if (testInterface.begin(*canvas)) Serial.println("[4v3b] test interface on :80");

  // The three the HIL orchestrator drives. Only the debug handler was wired
  // before, which is why no pixel-parity run has ever been possible on this
  // board even though the endpoints serving it were already there.
  testInterface.setScreenSwitchHandler([](int index) -> bool {
    if (!projectLoader.isLoaded()) return false;
    if (index < 0 || index >= (int)projectLoader.getProject().screens.size()) return false;
    currentScreen = index;
    forgetTouchFeedback();
    renderAndPresent("test interface: screen switch");
    return true;
  });

  // Lets a test ask "has the device applied this value yet" instead of
  // sleeping and hoping - the fix for a whole class of flaky HIL runs.
  testInterface.setTopicValueHandler(
      [](const String &topic) { return projectLoader.getTopicValue(topic); });

  testInterface.setTouchHandler([](int16_t x, int16_t y, bool down) {
    injectX = x;
    injectY = y;
    injectDown = down;
    // A posted lift ends the window rather than extending it, so the
    // release runs against the real panel - which is also reading idle -
    // instead of against 400ms of frozen injected state.
    injectUntilMs = down ? millis() + TOUCH_INJECT_HOLD_MS : 0;
    injectLiftOnce = !down;
  });

  // GET /api/debug?set=bench=1 measures where a swipe frame goes. Behind a
  // parameter rather than run at boot because it writes rubbish into the
  // framebuffer for a few hundred milliseconds before re-rendering, and
  // nobody wants that on a panel that is showing a project.
  testInterface.setDebugHandler([](const String &setArg) -> String {
    String out = "ip " + WiFi.localIP().toString() + "  rssi " + String(WiFi.RSSI()) + " dBm\n";
    out += "heap " + String(ESP.getFreeHeap() / 1024) + " KB, psram free " + String(ESP.getFreePsram() / 1024) + " KB\n";
    out += "uptime " + String(millis() / 1000) + " s\n";
    out += panel.report();
    out += "  last tap: " + lastTap + "\n";
    out += "  last feedback: " + lastFeedback + "\n";
    out += swipe.report();
    out += "  contacts " + String(contactCount) + ", of them missed " + String(missedContacts) +
           " (most samples in a missed one " + String(missedMaxSamples) + ", widest dx " +
           String(missedMaxDx) + " px)\n";
    {
      const unsigned long windowMs = millis() - dutyWindowStartMs;
      out += "  value renders " + String(topicRenders) + " in " + String(windowMs / 1000.0f, 1) +
             " s = " + String(windowMs ? topicRenderUs / 10.0f / windowMs : 0.0f, 1) +
             "% of the time blind" + String(topicRenderEnabled ? "" : "  (rendering is OFF)") + "\n";
      out += "  broker " + (brokerHost.isEmpty() ? String("(none configured)")
                                               : brokerHost + ":" + String(brokerPort)) + "\n";
    out += "  loop " + String(loopTurns) + " turns, " +
             String(loopTurns ? windowMs * 1000.0f / loopTurns / 1000.0f : 0.0f, 1) +
             " ms each on average, worst turn " + String(worstTurnUs / 1000.0f, 1) + " ms\n";
      out += "  worst web " + String(worstWebUs / 1000.0f, 1) + " ms, worst mqtt " +
             String(worstMqttUs / 1000.0f, 1) + " ms, worst touch read " +
             String(worstTouchUs / 1000.0f, 1) + " ms\n";
      out += "  totals over the window: web " + String(webUs / 1000) + " ms, mqtt " +
             String(mqttUs / 1000) + " ms, touch " + String(touchUs / 1000) + " ms, mqtt " +
             String(mqttClient.isConnected() ? "connected" : "NOT connected") + "\n";
    }
    if (setArg == "bench=1" && canvas)
    {
      out += "\n" + PanelBenchmark::run(panel.back(), canvas->getBuffer(), PANEL_W, PANEL_H);
      renderAndPresent("after benchmark");
    }
    else if (setArg == "render=1" && canvas && renderer)
    {
      // What a value change actually costs, measured before anything is
      // instrumented. The breakdown further down is inflated by its own
      // probes - two cycle-counter reads on every one of 129,600 arc
      // pixels - so quoting a profiled total as the render time would
      // overstate it, and this board has been burnt by a number that
      // nobody re-checked once already.
      //
      // Both halves are here because a value arriving over MQTT pays for
      // both: renderScreen() draws into the PSRAM canvas, and the whole
      // canvas is then pushed to the panel.
      out += "\nrender (screen " + String(currentScreen) + ")\n";

      uint32_t plainBest = UINT32_MAX, plainWorst = 0;
      for (int i = 0; i < 5; i++)
      {
        uint32_t p0 = micros();
        renderer->renderScreen(currentScreen);
        uint32_t d = micros() - p0;
        if (d < plainBest) plainBest = d;
        if (d > plainWorst) plainWorst = d;
      }

      // The same call the render path makes, so the number is the real
      // one and not a benchmark standing in for it.
      uint32_t blitBest = UINT32_MAX;
      for (int i = 0; i < 5; i++)
      {
        uint32_t b0 = micros();
        panel.blit(canvas->getBuffer());
        uint32_t d = micros() - b0;
        if (d < blitBest) blitBest = d;
      }

      out += "  render          " + String(plainBest / 1000.0f, 1) + " ms  (best of 5, worst " + String(plainWorst / 1000.0f, 1) + ")\n";
      out += "  blit to panel   " + String(blitBest / 1000.0f, 1) + " ms  (best of 5)\n";
      out += "  a value change costs both: " + String((plainBest + blitBest) / 1000.0f, 1) + " ms\n";

      out += "\nbreakdown (profiled, so inflated - see the overhead line)\n";

      RenderProfile::setEnabled(true);

      // No cold-cache run is reported, because there is no way to get one
      // here: the board renders this screen at boot, so every asset it
      // uses is already decoded and cached by the time anyone can ask.
      // An earlier version labelled the first run "first render" as though
      // it were cold, which was simply wrong.
      uint32_t bestUs = UINT32_MAX;
      String bestReport;
      for (int i = 0; i < 3; i++)
      {
        RenderProfile::reset();
        uint32_t s0 = micros();
        renderer->renderScreen(currentScreen);
        uint32_t d = micros() - s0;
        if (d < bestUs)
        {
          bestUs = d;
          bestReport = RenderProfile::report();
        }
      }

      RenderProfile::setEnabled(false);

      out += "  profiled render " + String(bestUs / 1000.0f, 1) + " ms  (best of 3)\n";
      out += bestReport;
      out += "  profiler overhead " + String((bestUs > plainBest ? bestUs - plainBest : 0) / 1000.0f, 1) + " ms - subtract it from every line above\n";
      out += "  floor for touching every pixel once: 25 ms (see PanelBenchmark.h)\n";

      renderAndPresent("after render profile");
    }
    else if (setArg == "swipes=1" || setArg == "swipes=clear")
    {
      if (setArg == "swipes=clear")
      {
        swipe.clearLog();
        contactCount = 0;
        contactSamples = 0;
        missedContacts = 0;
        missedMaxSamples = 0;
        missedMaxDx = 0;
        topicRenders = 0;
        topicRenderUs = 0;
        dutyWindowStartMs = millis();
        loopTurns = 0;
        webUs = mqttUs = touchUs = 0;
        worstWebUs = worstMqttUs = worstTouchUs = worstTurnUs = 0;
        out += "\ngesture log cleared\n";
      }
      else
      {
        out += "\ngesture log\n" + swipe.log();
      }
    }
    else if (setArg == "topicrender=off" || setArg == "topicrender=on")
    {
      topicRenderEnabled = setArg == "topicrender=on";
      out += String("\nvalue rendering is now ") + (topicRenderEnabled ? "on" : "off") + "\n";
      if (topicRenderEnabled) renderAndPresent("rendering back on");
    }
    else if (setArg == "aspect=1" && canvas)
    {
      // A square of equal pixel counts, for measuring with a ruler.
      //
      // Why the panel and not the datasheet: a circle was reported as wider
      // than tall, and the render turned out to be exactly round - 360px
      // across and 360 down. So the distortion is the panel's pixel pitch,
      // and the way to learn it is to measure something drawn rather than
      // to work backwards from the case, where a bezel eats a few
      // millimetres of height and turns a 6% error into an apparent 16%.
      //
      // 400x400 with a cross through the middle: the block is easy to lay a
      // ruler along, and the arms say where its edges are if the corners
      // fall under a rounded bezel.
      measuringPattern = true;
      const int16_t side = 400;
      const int16_t x0 = (int16_t)((PANEL_W - side) / 2);
      const int16_t y0 = (int16_t)((PANEL_H - side) / 2);
      canvas->fillScreen(0x0000);
      canvas->fillRect(x0, y0, side, side, 0xFFFF);
      canvas->fillRect(x0 + side / 2 - 1, 0, 2, PANEL_H, 0xF800);
      canvas->fillRect(0, y0 + side / 2 - 1, PANEL_W, 2, 0xF800);
      panel.blit(canvas->getBuffer());
      out += "\nmeasuring square: " + String(side) + "x" + String(side) +
             " px, centred, red cross through its middle\n";
      out += "  measure the WHITE block, width and height, in mm\n";
      out += "  the panel ignores touches until you put the project back\n";
      out += "  equal numbers mean square pixels; wider means a circle cannot look round\n";
      out += "  ?set=aspect=0 puts the project back\n";
    }
    else if (setArg == "circle=1" && canvas)
    {
      // A closed ring of exactly 400px outer diameter, drawn through the
      // project's own rasterizer.
      //
      // Why not simply trust the measurement already taken from a snapshot:
      // the project's dial is open at the bottom, so its vertical diameter
      // had to be inferred from the centre and the top rather than read off.
      // A closed ring can be laid a ruler against, and it exercises
      // ArcRaster on the way, so a bug in the rasterizer could not hide
      // behind a hand-drawn circle either.
      measuringPattern = true;
      const int32_t side = 400;
      const int16_t x0 = (int16_t)((PANEL_W - side) / 2);
      const int16_t y0 = (int16_t)((PANEL_H - side) / 2);

      ArcRaster::RingGeometry geom;
      geom.size = side;
      geom.thickness = 20;
      geom.track = ArcRaster::makeSector(0, 360 * ArcRaster::ANGLE_SCALE);
      geom.fill = ArcRaster::makeSector(0, 0);
      geom.marker = ArcRaster::makeSector(0, 0);

      const ArcRaster::Rgb565 ink = ArcRaster::toRgb565(0xFFFF);
      const ArcRaster::Rgb565 back = ArcRaster::toRgb565(0x0000);
      const ArcRaster::Rgb565 none = ArcRaster::toRgb565(0x0000);

      canvas->fillScreen(0x0000);
      const uint32_t t0 = micros();
      for (int32_t py = 0; py < side; py++)
      {
        const ArcRaster::RowSpan span = ArcRaster::rowSpan(geom, py);
        for (int32_t px = span.x0; px <= span.x1; px++)
        {
          if (span.holeX1 >= span.holeX0 && px >= span.holeX0 && px <= span.holeX1) continue;
          const ArcRaster::PixelBands bands = ArcRaster::pixelBands(geom, px, py);
          const int32_t covered = bands.fill + bands.track + bands.marker;
          if (covered == 0) continue;
          const uint16_t mixed = ArcRaster::blendBands(none, bands.fill, ink, bands.track, none,
                                                       bands.marker, back,
                                                       (uint8_t)(ArcRaster::COVERAGE_MAX - covered));
          canvas->drawPixel(x0 + px, y0 + py, mixed);
        }
      }
      const uint32_t drawUs = micros() - t0;

      // Marks at the four extremes, so a ruler has something to aim at.
      canvas->fillRect(x0 - 30, y0 + side / 2 - 1, 24, 2, 0xF800);
      canvas->fillRect(x0 + side + 6, y0 + side / 2 - 1, 24, 2, 0xF800);
      canvas->fillRect(x0 + side / 2 - 1, y0 - 30, 2, 24, 0xF800);
      canvas->fillRect(x0 + side / 2 - 1, y0 + side + 6, 2, 24, 0xF800);
      panel.blit(canvas->getBuffer());

      out += "\nclosed ring: outer diameter " + String(side) + " px both ways, drawn in " +
             String(drawUs / 1000.0f, 1) + " ms\n";
      out += "  red marks sit level with its outer edge on all four sides\n";
      out += "  measure across the outside of the ring, horizontally and vertically\n";
      out += "  the panel ignores touches until ?set=aspect=0 puts the project back\n";
    }
    else if (setArg == "aspect=0")
    {
      measuringPattern = false;
      renderAndPresent("after the measuring square");
      out += "\nproject restored\n";
    }
    else if (setArg == "arcselftest=1")
    {
      // Needs no project, no broker and no fixture - it compares the
      // rasterizer against itself, which is why it can live on the device.
      bool ok = true;
      out += "\n" + ArcSelfTest::run(ok);
    }
    else
    {
      out += "\nadd ?set=bench=1 for panel bandwidth, ?set=render=1 for the render breakdown, ";
      out += "?set=arcselftest=1 to prove the arc short cut, ";
      out += "?set=swipes=1 for the gesture log (?set=swipes=clear to reset it), ";
      out += "?set=topicrender=off to judge a gesture with nothing else drawing, ";
      out += "?set=aspect=1 for a 400x400 measuring square, ?set=circle=1 for a closed 400px ring ";
      out += "(?set=aspect=0 puts the project back)\n";
    }
    return out;
  });

  if (creds.mqttHost.isEmpty())
  {
    Serial.println("[4v3b] no broker configured - skipping MQTT");
    return;
  }

  brokerHost = creds.mqttHost;
  brokerPort = (uint16_t)creds.mqttPort;
  mqttClient.configure(creds.mqttHost, (uint16_t)creds.mqttPort, creds.mqttUsername, creds.mqttPassword);
  deployManager.setPublishStatusHandler(
      [](const String &id, const String &state, const String &error, int percent) {
        protocol.publishDeployStatus(id, state, error, percent);
      });
  deployManager.setClearTriggerHandler([]() { protocol.clearDeployTrigger(); });

  // Subscribed up front rather than from the connected callback: connect()
  // replays its subscription list on every reconnect, so queueing it once
  // covers every later connection too.
  mqttClient.subscribe(protocol.deviceTopic("deploy"));
  mqttClient.setConnectedCallback([]() {
    protocol.publishHello("http://" + WiFi.localIP().toString() + "/ddf.zip");
    Serial.println("[4v3b] published hello");
  });
  mqttClient.setCallback([](const String &topic, const String &payload) {
    if (topic == protocol.deviceTopic("deploy"))
    {
      deployManager.handleDeployMessage(payload);
      return;
    }
    projectLoader.setTopicValue(topic, payload);

    // Never drawn from here. This runs inside mqttClient.loop(), so a
    // render costing 180ms happened between two touch samples and, during a
    // transition, drew the outgoing screen straight over the frame being
    // composed. Measured on a real gesture: 7 composed frames in 3.4
    // seconds of dragging, the rest of the time spent here.
    //
    // The value is stored the moment it lands. Only the drawing waits, and
    // the loop decides when.
    renderPending = true;
  });
  mqttClient.connect();
  Serial.printf("[4v3b] mqtt %s:%d as %s\n", creds.mqttHost.c_str(), creds.mqttPort,
                mqttClient.getClientId().c_str());
}

void setup()
{
  Serial.begin(115200);
  delay(200);
  Serial.println();
  Serial.println("[4v3b] boot");

  if (!panel.begin(PANEL_W, PANEL_H, PANEL_PINS, PANEL_TIMING, BOUNCE_BUFFER_PX))
  {
    // Nothing to draw on if this fails, so the serial line is the only
    // channel left - which is why this build keeps CDC-on-boot enabled.
    Serial.println("[4v3b] panel.begin() FAILED");
    return;
  }
  panel.fill(0x0000);

  Wire.begin(I2C_SDA, I2C_SCL);
  Wire.setClock(400000);
  if (!expander.begin()) Serial.println("[4v3b] CH422G init FAILED");
  if (!touch.begin(expander, TP_INT))
    Serial.println("[4v3b] GT911 not found on 0x5D or 0x14");
  else
    Serial.printf("[4v3b] GT911 at 0x%02X\n", touch.address());

  setupRenderStack();
  setupNetworking();
}

// Which segment of a switch a tap at x selects.
//
// One function rather than two, because the pressed segment and the
// published one have to be the same or the panel lights up one thing and
// commands another.
static int switchSegmentFor(const ScreenObject *hit, int16_t x, int16_t objX)
{
  const int states = (int)hit->properties.states.size();
  if (states <= 0) return -1;

  if (hit->properties.mode == "single")
  {
    // One area, one target: a tap moves to the next state and wraps. Two
    // states make a toggle, three a cycle, so there is no special case.
    // The active index comes from the renderer rather than from a second
    // copy of the same rule here - what is drawn and what is tapped have
    // to agree about the state.
    const int active = renderer->getActiveSwitchStateIndex(*hit);
    // No retained value yet: the tile shows "?" and a tap starts at the
    // beginning instead of doing nothing.
    return (active < 0) ? 0 : (active + 1) % states;
  }

  // Equal segments across the object's width, the same division
  // ColorScreenRenderer::renderSwitch() draws. If this differs, the finger
  // hits a segment other than the one it is pointing at.
  int index = ((int)(x - objX) * states) / hit->width;
  if (index < 0) index = 0;
  if (index >= states) index = states - 1;
  return index;
}

// Redraws only the rectangle the feedback lives in, then shows it.
//
// The region render is what makes this worth doing at all: a full screen is
// 113ms before the panel even sees it, and a press that arrives a tenth of a
// second late is not feedback. The blit is still whole, because with three
// framebuffers the one being drawn into is two flips old and a partial copy
// would show two frames at once - the tearing this board just got rid of, by
// another route.
static void renderRegion(int16_t rx, int16_t ry, int16_t rw, int16_t rh, const char *reason,
                         bool present = true)
{
  if (!renderer || !canvas || rw <= 0 || rh <= 0) return;
  const uint32_t t0 = micros();
  renderer->renderScreenRegion(currentScreen, rx, ry, rw, rh,
                               pressedButtonId, pendingSwitchId, pendingSwitchIndex,
                               pressedSwitchId, pressedSwitchIndex);
  const uint32_t t1 = micros();
  // A transition composes from the canvas, not from the panel, so the one
  // caller that is about to start one needs the canvas corrected and
  // nothing else - the first composed frame replaces the glass anyway.
  if (present) panel.blit(canvas->getBuffer());
  const uint32_t t2 = micros();
  Serial.printf("[feedback] %s: region %lums, blit %lums\n", reason,
                (unsigned long)((t1 - t0) / 1000), (unsigned long)((t2 - t1) / 1000));
  lastFeedback = String(reason) + ": " + String(rw) + "x" + String(rh) +
                 " region " + String((t1 - t0) / 1000.0f, 1) + " ms" +
                 (present ? ", blit " + String((t2 - t1) / 1000.0f, 1) + " ms" : ", no blit");
}

// The rectangle whatever is currently showing lives in.
static void renderFeedbackRegion(const char *reason, bool present = true)
{
  if (!feedbackActive) return;
  renderRegion(feedbackX, feedbackY, feedbackW, feedbackH, reason, present);
}

// A finger has landed on something tappable: show it pressed at once,
// before anything is decided about what the gesture will turn out to be.
static void beginPressFeedback(int16_t x, int16_t y)
{
  if (!projectLoader.isLoaded() || renderer == nullptr) return;

  int16_t objX = 0, objY = 0;
  const ScreenObject *hit = renderer->hitTestTappable(currentScreen, x, y, objX, objY);
  if (hit == nullptr) return;

  if (hit->type == "SoftwareButton") pressedButtonId = hit->id;
  else if (hit->type == "Switch")
  {
    pressedSwitchId = hit->id;
    pressedSwitchIndex = switchSegmentFor(hit, x, objX);
  }
  else return;

  feedbackX = objX;
  feedbackY = objY;
  feedbackW = hit->width;
  feedbackH = hit->height;
  feedbackActive = true;
  renderFeedbackRegion("press");
}

// The finger is gone, whatever it turned out to mean. Anything still shown
// pressed has to stop being pressed, and a switch waiting for its broker
// keeps its pending mark.
static void clearPressFeedback()
{
  if (pressedButtonId.isEmpty() && pressedSwitchId.isEmpty()) return;
  pressedButtonId = "";
  pressedSwitchId = "";
  pressedSwitchIndex = -1;
  renderFeedbackRegion("release");
  if (pendingSwitchId.isEmpty()) feedbackActive = false;
}

// A tap has gone out and nothing has confirmed it yet.
static void markPending(const String &id, int index, int16_t rx, int16_t ry, int16_t rw, int16_t rh)
{
  // A mark on some other tile is now stale, and it is the only thing that
  // knows where it is. Wiped before the new one takes the state over.
  if (!pendingSwitchId.isEmpty() && pendingSwitchId != id)
  {
    const int16_t oldX = pendingX, oldY = pendingY, oldW = pendingW, oldH = pendingH;
    pendingSwitchId = "";
    pendingSwitchIndex = -1;
    renderRegion(oldX, oldY, oldW, oldH, "superseded");
  }

  pendingSwitchId = id;
  pendingSwitchIndex = index;
  pendingSinceMs = millis();
  pendingX = rx;
  pendingY = ry;
  pendingW = rw;
  pendingH = rh;
}

// Whatever came back, or ran out of patience.
static void clearPending(const char *reason)
{
  if (pendingSwitchId.isEmpty()) return;
  const int16_t x = pendingX, y = pendingY, w = pendingW, h = pendingH;
  pendingSwitchId = "";
  pendingSwitchIndex = -1;
  renderRegion(x, y, w, h, reason);
  feedbackActive = false;
}

// Everything the touch feedback believes, dropped without repainting.
// For a screen change, where the rectangles it remembers belong to a screen
// that is no longer showing and repainting them would put a mark on the
// wrong picture.
static void forgetTouchFeedback()
{
  pressedButtonId = "";
  pressedSwitchId = "";
  pressedSwitchIndex = -1;
  pendingSwitchId = "";
  pendingSwitchIndex = -1;
  feedbackActive = false;
}

// What a project binds to a button, and what this board does about it.
// Ported from the knob rather than reinvented: same action names, same
// order, same treatment of an unknown one.
static void dispatchButtonAction(const ButtonAction &action)
{
  if (action.type.isEmpty() || !projectLoader.isLoaded()) return;
  const ProjectConfig &project = projectLoader.getProject();
  const int count = (int)project.screens.size();

  if (action.type == "next-screen" || action.type == "previous-screen")
  {
    if (count == 0) return;
    const int delta = (action.type == "next-screen") ? 1 : -1;
    currentScreen = ((currentScreen + delta) % count + count) % count;
    forgetTouchFeedback();
    renderAndPresent("button: screen change");
  }
  else if (action.type == "goto-screen")
  {
    for (int i = 0; i < count; i++)
    {
      if (project.screens[i].id == action.targetScreenId)
      {
        currentScreen = i;
        forgetTouchFeedback();
        renderAndPresent("button: goto screen");
        break;
      }
    }
  }
  else if (action.type == "send-mqtt")
  {
    // Not retained: this is a command, not a state. What comes back on the
    // read topic is what the panel then shows.
    mqttClient.publish(action.mqttTopic, action.mqttMessage, false);
    Serial.printf("[tap] send-mqtt %s = %s\n", action.mqttTopic.c_str(), action.mqttMessage.c_str());
  }
  else if (action.type == "device-action")
  {
    // This board declares no deviceActions in its DDF yet, so every id is
    // unknown here. Logged and skipped rather than treated as an error: a
    // project may legitimately have been built against a device offering
    // more of them, and additive changes degrade rather than break.
    Serial.printf("[tap] device action \"%s\" - this board offers none\n",
                  action.deviceActionId.c_str());
  }
  else if (action.type == "goto-setup-mode")
  {
    enterSetupMode();
  }
}

// What a tap at this point hits, and what that does.
//
// Until now this board could render a SoftwareButton and a Switch, and show
// the Switch's state correctly because that arrives over MQTT - but a finger
// on either did nothing at all. It rendered a control panel without being
// one.
//
// The search runs through ColorScreenRenderer::hitTestTappable(), not a loop
// over screen.objects here, because the answer depends on two things that
// already live in the renderer: which panel of a tab-control is showing, and
// how far its children are drawn shifted. A second copy would mean the finger
// and the picture disagreeing about geometry. The knob learned that the hard
// way - its top-level switch worked and the four inside panels did not.
static bool dispatchTapAt(int16_t x, int16_t y)
{
  if (!projectLoader.isLoaded() || renderer == nullptr) return false;

  // Absolute corner of whatever was hit. For an object inside a panel that
  // is NOT obj.x/obj.y, and the segment arithmetic below depends on it.
  int16_t objX = 0, objY = 0;
  const ScreenObject *hit = renderer->hitTestTappable(currentScreen, x, y, objX, objY);
  if (hit == nullptr)
  {
    lastTap = "(" + String(x) + "," + String(y) + ") hit nothing tappable";
    return false;
  }

  if (hit->type == "SoftwareButton")
  {
    Serial.printf("[tap] SoftwareButton \"%s\"\n", hit->id.c_str());
    lastTap = "(" + String(x) + "," + String(y) + ") SoftwareButton \"" + hit->id + "\" action " +
              (hit->properties.action.type.isEmpty() ? String("(none)") : hit->properties.action.type);
    dispatchButtonAction(hit->properties.action);
    return true;
  }

  const int states = (int)hit->properties.states.size();
  if (states <= 0 || hit->properties.writeTopic.isEmpty())
  {
    lastTap = "(" + String(x) + "," + String(y) + ") " + hit->type + " \"" + hit->id +
              "\" has nothing to write";
    return false;
  }

  const int index = switchSegmentFor(hit, x, objX);
  if (index < 0) return false;

  const String &value = hit->properties.states[index].writeValue;
  if (value.isEmpty()) return false;

  Serial.printf("[tap] Switch \"%s\" segment %d -> %s = %s\n", hit->id.c_str(), index,
                hit->properties.writeTopic.c_str(), value.c_str());
  // Not retained: a command, not a state. The state comes back on the read
  // topic and colours the segment, so the switch shows what actually
  // happened rather than what it believed it was doing.
  const bool sent = mqttClient.publish(hit->properties.writeTopic, value, false);
  // Marked as asked-for either way. If the command did not even leave the
  // device, the mark expiring is the honest outcome - the alternative is a
  // segment that looks switched because we said so.
  markPending(hit->id, index, objX, objY, hit->width, hit->height);
  lastTap = "(" + String(x) + "," + String(y) + ") Switch \"" + hit->id + "\" segment " +
            String(index) + " -> " + hit->properties.writeTopic + " = " + value +
            (sent ? "" : "  (NOT published - broker away)");
  return true;
}

// One touch sample, from the glass or from a test, and the caller cannot
// tell which. Injection wins while its window lasts; a posted lift ends it
// at once rather than leaving the panel to time out into it.
static Gt911::Status readTouch(int16_t &x, int16_t &y)
{
  if (injectLiftOnce)
  {
    injectLiftOnce = false;
    return Gt911::Lifted;
  }
  if (injectUntilMs != 0 && (long)(millis() - injectUntilMs) < 0)
  {
    x = injectX;
    y = injectY;
    return injectDown ? Gt911::Touched : Gt911::Lifted;
  }

  Gt911::Point points[Gt911::MAX_POINTS];
  uint8_t n = 0;
  const Gt911::Status status = touch.poll(points, Gt911::MAX_POINTS, n);
  if (status == Gt911::Touched && n > 0)
  {
    x = (int16_t)points[0].x;
    y = (int16_t)points[0].y;
  }
  return status;
}

void loop()
{
  // Setup mode owns the device completely: it draws its own screen, it is
  // the only thing on port 80, and nothing else may run. Both faults in the
  // first version came from not doing that - the normal loop kept rendering
  // underneath and painted the project back over the setup screen, and
  // tick(), which draws that screen and reports the idle timeout, was never
  // called at all.
  if (setupModeActive)
  {
    wifiSetupServer->handleClient();
    // Leaving restarts rather than tearing the AP down and resuming in
    // place, the same choice the knob makes: a restart has one well-tested
    // path back into normal operation, unwinding AP mode by hand has none.
    // It also means setupModeActive never has to be cleared.
    if (wifiSetupServer->tick() || wifiSetupServer->exitRequested())
    {
      Serial.println("[4v3b] leaving setup mode - restarting");
      delay(200);
      ESP.restart();
    }
    delay(5);
    return;
  }

  // A transition owns the whole loop while it runs, the same way setup mode
  // does above. The web client and the MQTT client can each block for
  // longer than a frame, and a value arriving through the second one used
  // to render the outgoing screen straight over the gesture. Nothing else
  // gets a turn until the finger is up.
  if (swipe.active())
  {
    int16_t dragX = touchX, dragY = touchY;
    const Gt911::Status dragStatus = readTouch(dragX, dragY);

    if (dragStatus == Gt911::Touched)
    {
      lastTouchReportMs = millis();
      swipe.follow(dragX);
      return;
    }

    if (dragStatus == Gt911::NoNewSample)
    {
      // Nothing new to say is not a lifted finger. Wait for the controller
      // instead of composing the same frame again, and give up only once it
      // has been quiet for longer than any gap it leaves between reports.
      swipe.noteRecovery();
      if (millis() - lastTouchReportMs < TOUCH_LOST_MS)
      {
        delay(2);
        return;
      }
    }

    swipe.release();
    // The transition may have swapped which canvas is current. Anything
    // holding the old pointer would now be looking at the screen that just
    // left the glass.
    testInterface.setCanvas(*canvas);
    if (displayAdapter) displayAdapter->setCanvas(*canvas);
    // Whatever was marked belonged to the screen that just slid away, or to
    // the one that stayed - either way its rectangle has been repainted by
    // the transition and the mark is gone from the glass already.
    forgetTouchFeedback();
    // Deliberately not rendering here, even though a value is usually
    // waiting. That render is 180ms during which the next contact cannot
    // be seen, and these gestures come in quick succession - a fifth of a
    // second of blindness immediately after every swipe is its own way of
    // losing every other one. The loop below draws it on its next turn,
    // after it has looked for a finger.
    return;
  }

  // A finger on the glass owns the loop, for the same reason a transition
  // does. Recognising a swipe needs two touch samples inside one contact,
  // and a contact is over in 300ms - while one arriving value costs 180ms
  // to draw and the web client can block for longer. Eleven of twenty real
  // swipes were never seen at all: both samples landed either side of the
  // gesture, so the finger was only ever observed standing still.
  //
  // Whatever arrives meanwhile waits in the socket for a second or two,
  // which nobody can see, and is drawn when the finger lifts.
  const uint32_t turnStart = micros();
  loopTurns++;

  // Nothing may draw over the measuring square, including a finger.
  if (measuringPattern)
  {
    testInterface.handleClient();
    delay(10);
    return;
  }

  int16_t sampleX = touchX, sampleY = touchY;
  const uint32_t touchT0 = micros();
  const Gt911::Status touchStatus = readTouch(sampleX, sampleY);
  {
    const uint32_t d = micros() - touchT0;
    touchUs += d;
    if (d > worstTouchUs) worstTouchUs = d;
  }
  if (touchStatus == Gt911::Touched)
  {
    fingerDown = true;
    touchX = sampleX;
    touchY = sampleY;
    lastTouchReportMs = millis();
  }
  else if (touchStatus == Gt911::Lifted)
  {
    fingerDown = false;
  }
  else if (fingerDown && (millis() - lastTouchReportMs) >= TOUCH_LOST_MS)
  {
    // Quiet for longer than a gap between reports, and it never said the
    // finger was gone. Treated as gone, so one lost report cannot leave a
    // contact open for ever.
    fingerDown = false;
  }

  // The reserved setup hold, checked before anything else can claim this
  // contact.
  if (fingerDown)
  {
    if (!holdActive)
    {
      holdActive = true;
      holdStartX = touchX;
      holdStartY = touchY;
      holdStartMs = millis();
      holdLastSecondShown = -1;
      holdConsumed = false;
      contactCount++;
      contactSamples = 0;
      contactDrift = 0;
      // Before anything is decided about what this gesture will be. A press
      // that waits to find out is not a press.
      beginPressFeedback(touchX, touchY);
    }
    contactSamples++;
    const int dx = (int)touchX - holdStartX;
    const int dy = (int)touchY - holdStartY;

    // Travel that is clearly horizontal is a page turn, not a hold that
    // wandered. Tested before the drift branch below, which would otherwise
    // swallow every drag by treating it as a fresh hold reference. A finger
    // that has already been shown a countdown is left alone.
    // Measured from where the finger went down, not from where the drag was
    // recognised: the screen then sits exactly under the finger, and the
    // commit threshold means the distance the user actually swiped.
    const int16_t drift = (int16_t)max(abs(dx), abs(dy));
    if (drift > contactDrift) contactDrift = drift;

    if (abs(dx) > abs(missedMaxDx)) missedMaxDx = dx;

    if (!holdConsumed && abs(dx) >= FollowSwipe::startThresholdPx() && abs(dx) > 2 * abs(dy))
    {
      // The finger is leaving, so nothing it touched on the way is pressed.
      // Corrected in the canvas only: the transition reads from there, and
      // the panel is about to be replaced wholesale.
      if (!pressedButtonId.isEmpty() || !pressedSwitchId.isEmpty())
      {
        pressedButtonId = "";
        pressedSwitchId = "";
        pressedSwitchIndex = -1;
        renderFeedbackRegion("drag took over", false);
        if (pendingSwitchId.isEmpty()) feedbackActive = false;
      }
      if (swipe.start(dx < 0 ? 1 : -1, holdStartX))
      {
        holdActive = false;
        return;
      }
    }

    if (abs(dx) <= SETUP_HOLD_MAX_DRIFT && abs(dy) <= SETUP_HOLD_MAX_DRIFT)
    {
      const unsigned long held = millis() - holdStartMs;
      if (held >= SETUP_HOLD_MS)
      {
        enterSetupMode();
        return;
      }
      if (held >= SETUP_HOLD_FEEDBACK_MS)
      {
        holdConsumed = true;
        const int secondsLeft = (int)((SETUP_HOLD_MS - held + 999) / 1000);
        if (secondsLeft != holdLastSecondShown)
        {
          holdLastSecondShown = secondsLeft;
          if (displayAdapter)
            displayAdapter->showLines({"Setup in " + String(secondsLeft) + "s",
                                       "loslassen zum Abbrechen"});
        }
      }
    }
    else
    {
      // Moved too far for a hold. Nothing else claims a horizontal drag
      // on this board, so the reference simply restarts from here - a
      // finger that wandered and then settled can still reach five seconds.
      holdStartX = touchX;
      holdStartY = touchY;
      holdStartMs = millis();
      holdLastSecondShown = -1;
    }

    // The web client still gets its turn, MQTT still does not. That split is
    // the whole point: the 3s that blinded this board was a broker connect,
    // measured, while the web server's worst turn was 13ms. Leaving it out
    // as well cost more than it saved - a press cannot be photographed if
    // the request for the photograph is only served after the finger lifts,
    // which is exactly how the press feedback came to look like it was not
    // working at all. A test fetching a full snapshot does block a gesture
    // for about a second; that is a trade taken deliberately, because
    // nobody fetches snapshots while swiping except a test that wants one.
    testInterface.handleClient();

    // Otherwise nothing else gets a turn. 5ms is short against a gesture
    // and long against an I2C read, so a 300ms swipe is seen dozens of
    // times instead of once or twice.
    delay(5);
    return;
  }

  if (holdActive)
  {
    holdActive = false;
    // A contact that came and went without ever being read as a swipe.
    // How many samples it got, and how far it was seen to travel, say
    // which of the two explanations is the right one.
    missedContacts++;
    if (contactSamples > missedMaxSamples) missedMaxSamples = contactSamples;

    // A contact that stayed put and was not claimed by the setup hold is a
    // tap. Dispatched from where the finger went down rather than where it
    // left, because that is the point the user aimed at.
    // Unpressed first, then dispatched: an action that changes screens
    // repaints everything itself, and a region render afterwards would be
    // drawing yesterday's rectangle onto today's screen.
    pressedButtonId = "";
    pressedSwitchId = "";
    pressedSwitchIndex = -1;

    const uint32_t generationBefore = renderGeneration;
    if (!holdConsumed && contactDrift <= TAP_MAX_DRIFT)
    {
      dispatchTapAt(holdStartX, holdStartY);
    }
    if (renderGeneration == generationBefore)
    {
      renderFeedbackRegion("release");
      if (pendingSwitchId.isEmpty()) feedbackActive = false;
    }
    // A hold that showed a countdown and was then released must not count as
    // anything else, and the panel has to be put back because the countdown
    // drew over it.
    if (holdConsumed)
    {
      holdConsumed = false;
      renderAndPresent("hold cancelled");
    }
  }

  const uint32_t webT0 = micros();
  testInterface.handleClient();
  {
    const uint32_t d = micros() - webT0;
    webUs += d;
    if (d > worstWebUs) worstWebUs = d;
  }

  const uint32_t mqttT0 = micros();
  mqttClient.loop();
  subscribeToProjectTopics();
  {
    const uint32_t d = micros() - mqttT0;
    mqttUs += d;
    if (d > worstMqttUs) worstMqttUs = d;
  }

  // A value arrived, so the device has newer state than the mark does. The
  // mark goes before the render rather than after it, so the fresh values
  // decide the picture instead of being drawn under a stale "waiting".
  if (renderPending && !pendingSwitchId.isEmpty())
  {
    pendingSwitchId = "";
    pendingSwitchIndex = -1;
    feedbackActive = false;
  }
  else if (!pendingSwitchId.isEmpty() && (millis() - pendingSinceMs) >= PENDING_TIMEOUT_MS)
  {
    // Nothing came back. Better to drop the mark than to leave a switch
    // looking like it is still being asked.
    clearPending("pending timed out");
  }

  if (renderPending && topicRenderEnabled && (millis() - lastTopicRenderMs) >= TOPIC_RENDER_MIN_MS)
  {
    renderPending = false;
    lastTopicRenderMs = millis();
    const uint32_t t0 = micros();
    renderAndPresent("topic");
    topicRenderUs += micros() - t0;
    topicRenders++;
  }

  {
    const uint32_t turn = micros() - turnStart;
    if (turn > worstTurnUs) worstTurnUs = turn;
  }

  delay(20);
}
