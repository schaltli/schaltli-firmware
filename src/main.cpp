// EXPERIMENT: knob-wake from deep sleep, with extra peripherals held in
// hardware reset to reduce residual current further.
//
// The rotary encoder's pin_a is wired to GPIO7 (reverse-engineered from
// https://github.com/KrX3D/WaveShare-Knob-Esp32S3 - Waveshare doesn't
// document this pin directly). GPIO7 is RTC-capable on the S3, so it's
// used as an EXT0 deep-sleep wake source: idle HIGH via internal pull-up,
// a rotation pulses it LOW. Confirmed working on real hardware.
//
// Besides the backlight (GPIO47), two more known pins are driven LOW and
// held there through deep sleep via rtc_gpio_hold_en():
//   - GPIO21: ST77916 LCD reset - holds the display controller in
//     hardware reset instead of leaving it actively refreshing/undriven.
//   - GPIO10: CST816 touch controller reset - same idea.
// Everything else on the board (companion ESP32, audio DAC, vibration
// motor driver) isn't touched here - no schematic access to know how to
// reach them safely, so the ~50mA deep-sleep floor measured with only the
// backlight off likely still includes some of their idle draw.

#include <Arduino.h>
#include "esp_sleep.h"
#include "driver/rtc_io.h"

static const gpio_num_t WAKE_PIN = GPIO_NUM_7;      // encoder pin_a
static const gpio_num_t LCD_RST_PIN = GPIO_NUM_21;  // ST77916 reset
static const gpio_num_t TOUCH_RST_PIN = GPIO_NUM_10;  // CST816 reset
static const uint8_t BACKLIGHT_PIN = 47;

static void holdLow(gpio_num_t pin) {
  rtc_gpio_hold_dis(pin);
  rtc_gpio_init(pin);
  rtc_gpio_set_direction(pin, RTC_GPIO_MODE_OUTPUT_ONLY);
  rtc_gpio_set_level(pin, 0);
  rtc_gpio_hold_en(pin);
}

void setup() {
  Serial.begin(115200);
  delay(500);

  pinMode(BACKLIGHT_PIN, OUTPUT);
  digitalWrite(BACKLIGHT_PIN, LOW);

  rtc_gpio_hold_dis(LCD_RST_PIN);
  rtc_gpio_hold_dis(TOUCH_RST_PIN);

  esp_sleep_wakeup_cause_t cause = esp_sleep_get_wakeup_cause();
  if (cause == ESP_SLEEP_WAKEUP_EXT0) {
    Serial.println(">>> WOKE UP from deep sleep via knob rotation (EXT0 on GPIO7) <<<");
    digitalWrite(BACKLIGHT_PIN, HIGH);
    delay(5000);
    digitalWrite(BACKLIGHT_PIN, LOW);
  } else {
    Serial.printf("Boot, wakeup cause = %d (0 = not a sleep wake)\n", cause);
  }

  Serial.println("Entering deep sleep in 3s - LCD/touch held in reset, rotate the knob to wake.");
  delay(3000);

  holdLow(LCD_RST_PIN);
  holdLow(TOUCH_RST_PIN);
  gpio_deep_sleep_hold_en();

  rtc_gpio_init(WAKE_PIN);
  rtc_gpio_set_direction(WAKE_PIN, RTC_GPIO_MODE_INPUT_ONLY);
  rtc_gpio_pullup_en(WAKE_PIN);
  rtc_gpio_pulldown_dis(WAKE_PIN);
  esp_sleep_enable_ext0_wakeup(WAKE_PIN, 0);  // wake when GPIO7 goes LOW

  Serial.flush();
  esp_deep_sleep_start();
}

void loop() {}
