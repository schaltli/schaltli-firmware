// Bring-up sketch for the Waveshare ESP32-S3-Touch-LCD-4.3B.
//
// Milestone 1 of the port: prove the panel alone. No LVGL, no project
// loader, no MQTT, no touch, no IO expander - so that a black screen after
// the first flash can only mean the RGB timings or the pin map, and nothing
// else. Everything above this is added once this is known good.
//
// Deliberately does NOT touch the CH422G expander. Waveshare's own
// 08_DrawColorBar example for this board doesn't either, which is the
// evidence that the backlight (EXIO2/DISP) is on by default after reset.
// Touch (GT911) needs EXIO1 released and therefore waits for milestone 2.
//
// Pin map and RGB timings are Waveshare's own, taken from
// waveshareteam/ESP32-S3-Touch-LCD-4.3B (Apache-2.0),
// examples/Arduino/examples/08_DrawColorBar/waveshare_lcd_port.h, and
// cross-checked line by line against the pin table in the board's wiki.
// Both sources agree; nothing here is inferred from the 1.8" knob port,
// which is the mistake that produced four hardware bugs on the last port.
#include <Arduino.h>
#include <Arduino_GFX_Library.h>

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

// --- pin map --------------------------------------------------------------
// Waveshare's DATA0..DATA15 is an RGB565 bus in the order B, G, R:
// DATA0-4 = blue, DATA5-10 = green, DATA11-15 = red. Arduino_GFX wants them
// split, hence the regrouping below. The wiki names the same lines B3..B7,
// G2..G7 and R3..R7 - the panel's own bit numbering, same physical pins.
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

// No panel reset line on this board (LCD_RST is on the expander, and the
// panel comes up without it - Waveshare's example passes -1 as well).
static Arduino_RGB_Display *gfx = new Arduino_RGB_Display(PANEL_W, PANEL_H, panel);

static void drawTestPattern()
{
  // Colour bars first: if the pin grouping were wrong, these come out in
  // the wrong order or with the wrong channel, which is far easier to read
  // off the glass than a wrong-looking photo.
  const uint16_t bars[] = {RGB565_RED, RGB565_GREEN, RGB565_BLUE, RGB565_YELLOW, RGB565_MAGENTA, RGB565_CYAN, RGB565_WHITE, RGB565_BLACK};
  const int16_t barW = PANEL_W / 8;
  for (int i = 0; i < 8; i++)
  {
    gfx->fillRect(i * barW, 0, barW, PANEL_H / 2, bars[i]);
  }

  // A one-pixel frame around the whole panel. If the timings are off, this
  // is where it shows: the frame either does not close or wraps around.
  gfx->drawRect(0, 0, PANEL_W, PANEL_H, RGB565_WHITE);

  gfx->setTextColor(RGB565_WHITE);
  gfx->setTextSize(3);
  gfx->setCursor(20, PANEL_H / 2 + 30);
  gfx->print("ScreenBee 4.3B bring-up");
  gfx->setTextSize(2);
  gfx->setCursor(20, PANEL_H / 2 + 80);
  gfx->printf("%dx%d  pclk %ld MHz", PANEL_W, PANEL_H, (long)(PCLK_HZ / 1000000));

  // Corner markers, so a photograph shows whether the visible area really
  // starts at 0,0 and ends at 799,479 rather than being shifted by a porch.
  gfx->fillRect(0, 0, 20, 20, RGB565_RED);
  gfx->fillRect(PANEL_W - 20, 0, 20, 20, RGB565_GREEN);
  gfx->fillRect(0, PANEL_H - 20, 20, 20, RGB565_BLUE);
  gfx->fillRect(PANEL_W - 20, PANEL_H - 20, 20, 20, RGB565_YELLOW);
}

void setup()
{
  Serial.begin(115200);
  delay(200);
  Serial.println();
  Serial.println("[4v3b] bring-up start");

  if (!gfx->begin())
  {
    // Nothing to draw on if this fails, so the serial line is the only
    // channel left - which is why this build keeps CDC-on-boot enabled.
    Serial.println("[4v3b] gfx->begin() FAILED");
    return;
  }
  Serial.println("[4v3b] gfx->begin() ok");

  // A framebuffer of 800*480*2 = 768000 bytes has to come out of PSRAM.
  // Report it rather than assume it: this is the number every later
  // decision about transition buffers depends on.
  Serial.printf("[4v3b] PSRAM free: %u of %u bytes\n",
                (unsigned)ESP.getFreePsram(), (unsigned)ESP.getPsramSize());
  Serial.printf("[4v3b] heap free: %u bytes\n", (unsigned)ESP.getFreeHeap());

  gfx->fillScreen(RGB565_BLACK);
  drawTestPattern();
  Serial.println("[4v3b] test pattern drawn");
}

void loop()
{
  static uint32_t last = 0;
  if (millis() - last >= 5000)
  {
    last = millis();
    Serial.printf("[4v3b] alive, uptime %lus, psram free %u\n",
                  (unsigned long)(millis() / 1000), (unsigned)ESP.getFreePsram());
  }
  delay(50);
}
