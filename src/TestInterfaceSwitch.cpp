#include "TestInterfaceSwitch.h"
#include "DeviceSettings.h"
#include <WiFi.h>
#include <esp_heap_caps.h>

void TestInterfaceSwitch::service()
{
  while (Serial.available() > 0)
  {
    const char c = (char)Serial.read();
    if (c == '\r') continue;
    if (c == '\n')
    {
      String line = line_;
      line_ = "";
      line.trim();
      if (line.startsWith("hil")) run(line);
      continue;
    }
    // A line is short; anything longer is not one of these.
    if (line_.length() < 96) line_ += c;
  }
}

void TestInterfaceSwitch::run(const String &line)
{
  if (line == "hil?")
  {
    Serial.printf("[hil] test interface %s\n", DeviceSettings::testInterfaceOn() ? "on" : "off");
    return;
  }
  if (line == "hil-diag")
  {
    Serial.printf("[hil] diag uptime %lu ms, wifi status %d rssi %d ip %s, internal free %u largest %u min %u, psram free %u, tasks %u%s%s\n",
                  (unsigned long)millis(), (int)WiFi.status(), (int)WiFi.RSSI(), WiFi.localIP().toString().c_str(),
                  (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT),
                  (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT),
                  (unsigned)heap_caps_get_minimum_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT),
                  (unsigned)heap_caps_get_free_size(MALLOC_CAP_SPIRAM), (unsigned)uxTaskGetNumberOfTasks(),
                  diag_ ? ", " : "", diag_ ? diag_().c_str() : "");
    return;
  }
  if (line == "hil-off")
  {
    DeviceSettings::setTestInterface(false, "");
    Serial.println("[hil] test interface off - restarting");
    Serial.flush();
    delay(200);
    ESP.restart();
  }
  if (line.startsWith("hil-on "))
  {
    const String password = line.substring(7);
    if (!DeviceSettings::setTestInterface(true, password))
    {
      Serial.printf("[hil] refused: a password of %u to %u characters\n", (unsigned)DeviceSettings::MIN_TEST_PASSWORD_LENGTH,
                    (unsigned)DeviceSettings::MAX_TEST_PASSWORD_LENGTH);
      return;
    }
    Serial.println("[hil] test interface on - restarting");
    Serial.flush();
    delay(200);
    ESP.restart();
  }
}
