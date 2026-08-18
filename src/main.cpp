// Minimal power-measurement test for the Waveshare ESP32-S3-Knob-Touch-LCD-1.8.
//
// Toggles the display backlight (GPIO47) off for 10s, then on for 10s, in an
// endless loop. Backlight is the dominant power draw of the display, so this
// is enough to see the on/off delta on a USB power meter without bringing up
// the ST77916 (QSPI) panel or CST816 touch controller at all.
//
// Board pin reference (Waveshare ESP32-S3-Knob-Touch-LCD-1.8):
//   LCD QSPI: CS=14 SCK=13 D0=15 D1=16 D2=17 D3=18 RST=21
//   Backlight: 47
//   Touch (CST816, I2C): not used here

#include <Arduino.h>

static const uint8_t BACKLIGHT_PIN = 47;
static const uint32_t INTERVAL_MS = 10000;

void setup() {
  Serial.begin(115200);
  pinMode(BACKLIGHT_PIN, OUTPUT);
  digitalWrite(BACKLIGHT_PIN, HIGH);
  Serial.println("screenbee-waveshare-1v8: backlight power test starting, screen ON");
  Serial.printf("PSRAM found: %s, size: %u bytes\n", psramFound() ? "yes" : "no", ESP.getPsramSize());
}

void loop() {
  digitalWrite(BACKLIGHT_PIN, LOW);
  Serial.println("screen OFF");
  delay(INTERVAL_MS);

  digitalWrite(BACKLIGHT_PIN, HIGH);
  Serial.println("screen ON");
  delay(INTERVAL_MS);
}
