#include "TestInterfaceSwitch.h"
#include "DeviceSettings.h"

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
