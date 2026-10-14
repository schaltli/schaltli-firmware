#include "DeviceRequests.h"
#include <LittleFS.h>
#include "DeviceInfo.h"
#include "DdfBytes.h"

// Pieces small enough for the socket's send buffer, so a write never waits
// long, and few enough that 300 KB is not a thousand calls.
static const size_t CHUNK = 2048;

String DeviceRequests::topic(const char *leaf) const
{
  return String(TOPIC_PREFIX) + "/" + client_.getClientId() + "/" + leaf;
}

void DeviceRequests::subscribe()
{
  client_.subscribe(topic("ddf/get"));
  client_.subscribe(topic("recovery/get"));
}

bool DeviceRequests::onMessage(const String &t)
{
  if (t == topic("ddf/get"))
  {
    ddfAsked_ = true;
    return true;
  }
  if (t == topic("recovery/get"))
  {
    recoveryAsked_ = true;
    return true;
  }
  return false;
}

void DeviceRequests::service()
{
  if (ddfAsked_)
  {
    ddfAsked_ = false;
    bool sent = publishDdf();
    Serial.printf("[requests] description %s (%u bytes)\n", sent ? "sent" : "NOT sent", (unsigned)ddfZipSize());
  }
  if (recoveryAsked_)
  {
    recoveryAsked_ = false;
    bool sent = publishRecovery();
    Serial.printf("[requests] recovery copy %s\n", sent ? "sent" : "NOT sent");
  }
}

// Straight from flash: DDF_ZIP is a const array there, readable like RAM on
// the ESP32, so it goes out without a copy. Retained, so the next designer
// to connect has it without asking.
bool DeviceRequests::publishDdf()
{
  const uint8_t *bytes = ddfZipBytes();
  const size_t length = ddfZipSize();
  if (!client_.beginLargePublish(topic("ddf"), length, true)) return false;
  for (size_t offset = 0; offset < length; offset += CHUNK)
  {
    size_t n = length - offset < CHUNK ? length - offset : CHUNK;
    if (client_.writeLargePublish(bytes + offset, n) != n)
    {
      client_.endLargePublish();
      return false;
    }
    yield();
  }
  return client_.endLargePublish();
}

bool DeviceRequests::publishRecovery()
{
  File file = LittleFS.open(RECOVERY_PROJECT_PATH, "r");
  if (!file || file.isDirectory() || file.size() == 0)
  {
    if (file) file.close();
    return client_.publish(topic("recovery"), "", false);
  }
  const size_t length = file.size();
  if (!client_.beginLargePublish(topic("recovery"), length, false))
  {
    file.close();
    return false;
  }
  uint8_t buffer[CHUNK];
  size_t sent = 0;
  while (sent < length)
  {
    size_t n = file.read(buffer, sizeof(buffer));
    if (n == 0 || client_.writeLargePublish(buffer, n) != n) break;
    sent += n;
    yield();
  }
  file.close();
  bool ended = client_.endLargePublish();
  return ended && sent == length;
}
