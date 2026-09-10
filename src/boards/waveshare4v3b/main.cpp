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
  bool ok = renderer->renderScreen(currentScreen);
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

  // GET /api/debug?set=bench=1 measures where a swipe frame goes. Behind a
  // parameter rather than run at boot because it writes rubbish into the
  // framebuffer for a few hundred milliseconds before re-rendering, and
  // nobody wants that on a panel that is showing a project.
  testInterface.setDebugHandler([](const String &setArg) -> String {
    String out = "ip " + WiFi.localIP().toString() + "  rssi " + String(WiFi.RSSI()) + " dBm\n";
    out += "heap " + String(ESP.getFreeHeap() / 1024) + " KB, psram free " + String(ESP.getFreePsram() / 1024) + " KB\n";
    out += "uptime " + String(millis() / 1000) + " s\n";
    out += panel.report();
    out += swipe.report();
    out += "  contacts " + String(contactCount) + ", of them missed " + String(missedContacts) +
           " (most samples in a missed one " + String(missedMaxSamples) + ", widest dx " +
           String(missedMaxDx) + " px)\n";
    {
      const unsigned long windowMs = millis() - dutyWindowStartMs;
      out += "  value renders " + String(topicRenders) + " in " + String(windowMs / 1000.0f, 1) +
             " s = " + String(windowMs ? topicRenderUs / 10.0f / windowMs : 0.0f, 1) +
             "% of the time blind" + String(topicRenderEnabled ? "" : "  (rendering is OFF)") + "\n";
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
      out += "?set=topicrender=off to judge a gesture with nothing else drawing\n";
    }
    return out;
  });

  if (creds.mqttHost.isEmpty())
  {
    Serial.println("[4v3b] no broker configured - skipping MQTT");
    return;
  }

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
    Gt911::Point dragPoints[Gt911::MAX_POINTS];
    uint8_t dragN = 0;
    const Gt911::Status dragStatus = touch.poll(dragPoints, Gt911::MAX_POINTS, dragN);

    if (dragStatus == Gt911::Touched && dragN > 0)
    {
      lastTouchReportMs = millis();
      swipe.follow((int16_t)dragPoints[0].x);
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

  Gt911::Point points[Gt911::MAX_POINTS];
  uint8_t n = 0;
  const uint32_t touchT0 = micros();
  const Gt911::Status touchStatus = touch.poll(points, Gt911::MAX_POINTS, n);
  {
    const uint32_t d = micros() - touchT0;
    touchUs += d;
    if (d > worstTouchUs) worstTouchUs = d;
  }
  if (touchStatus == Gt911::Touched && n > 0)
  {
    fingerDown = true;
    touchX = (int16_t)points[0].x;
    touchY = (int16_t)points[0].y;
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
    if (abs(dx) > abs(missedMaxDx)) missedMaxDx = dx;

    if (!holdConsumed && abs(dx) >= FollowSwipe::startThresholdPx() && abs(dx) > 2 * abs(dy) &&
        swipe.start(dx < 0 ? 1 : -1, holdStartX))
    {
      holdActive = false;
      return;
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

    // Nothing else gets a turn while the finger is down. 5ms is short
    // against a gesture and long against an I2C read, so a 300ms swipe is
    // seen dozens of times instead of once or twice.
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
