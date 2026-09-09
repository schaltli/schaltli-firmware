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
#include <Arduino_GFX_Library.h>
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
#include "PanelBenchmark.h"
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
static Arduino_ESP32RGBPanel *panel = new Arduino_ESP32RGBPanel(
    /* de */ 5, /* vsync */ 3, /* hsync */ 46, /* pclk */ 7,
    /* r0..r4 */ 1, 2, 42, 41, 40,
    /* g0..g5 */ 39, 0, 45, 48, 47, 21,
    /* b0..b4 */ 14, 38, 18, 17, 10,
    /* hsync */ 0, HSYNC_FRONT_PORCH, HSYNC_PULSE_WIDTH, HSYNC_BACK_PORCH,
    /* vsync */ 0, VSYNC_FRONT_PORCH, VSYNC_PULSE_WIDTH, VSYNC_BACK_PORCH,
    /* pclk_active_neg */ 1, /* prefer_speed */ PCLK_HZ, /* useBigEndian */ false,
    /* de_idle_high */ 0, /* pclk_idle_high */ 0,
    /* bounce_buffer_size_px */ BOUNCE_BUFFER_PX);

// No panel reset line on this board - LCD_RST is on the expander and the
// panel comes up without it, as Waveshare's own example also assumes.
static Arduino_RGB_Display *gfx = new Arduino_RGB_Display(PANEL_W, PANEL_H, panel);

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
  gfx->draw16bitRGBBitmap(0, 0, canvas->getBuffer(), PANEL_W, PANEL_H);
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

  displayAdapter = new PanelDisplayAdapter(*canvas, *gfx, PANEL_W, PANEL_H);
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
    if (setArg == "bench=1" && canvas && gfx)
    {
      out += "\n" + PanelBenchmark::run(gfx->getFramebuffer(), canvas->getBuffer(), PANEL_W, PANEL_H);
      renderAndPresent("after benchmark");
    }
    else
    {
      out += "\nadd ?set=bench=1 to measure the panel's memory bandwidth\n";
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
    renderAndPresent("topic");
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

  if (!gfx->begin())
  {
    // Nothing to draw on if this fails, so the serial line is the only
    // channel left - which is why this build keeps CDC-on-boot enabled.
    Serial.println("[4v3b] gfx->begin() FAILED");
    return;
  }
  gfx->fillScreen(0x0000);

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

  testInterface.handleClient();
  mqttClient.loop();
  subscribeToProjectTopics();

  Gt911::Point points[Gt911::MAX_POINTS];
  uint8_t n = touch.read(points, Gt911::MAX_POINTS);

  // The reserved setup hold, checked before anything else can claim this
  // contact.
  if (n > 0)
  {
    if (!holdActive)
    {
      holdActive = true;
      holdStartX = (int16_t)points[0].x;
      holdStartY = (int16_t)points[0].y;
      holdStartMs = millis();
      holdLastSecondShown = -1;
      holdConsumed = false;
    }
    const int dx = (int)points[0].x - holdStartX;
    const int dy = (int)points[0].y - holdStartY;
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
      holdStartX = (int16_t)points[0].x;
      holdStartY = (int16_t)points[0].y;
      holdStartMs = millis();
      holdLastSecondShown = -1;
    }
  }
  else if (holdActive)
  {
    holdActive = false;
    // A hold that showed a countdown and was then released must not count as
    // anything else, and the panel has to be put back because the countdown
    // drew over it.
    if (holdConsumed)
    {
      holdConsumed = false;
      renderAndPresent("hold cancelled");
    }
  }

  delay(n ? 10 : 30);
}
