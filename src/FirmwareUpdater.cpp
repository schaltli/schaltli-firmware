#include "FirmwareUpdater.h"
#include <ArduinoJson.h>
#include <HTTPClient.h>
#include <Update.h>
#include <WiFi.h>
#include "mbedtls/sha256.h"

#include "DeviceInfo.h"
#include "FirmwareImage.h"

namespace
{
// No bytes for this long ends the download. Long against a WiFi hiccup,
// short against someone watching a progress bar that stopped.
const unsigned long STALL_MS = 15000;

String toHex(const uint8_t *bytes, size_t len)
{
  static const char *DIGITS = "0123456789abcdef";
  String out;
  out.reserve(len * 2);
  for (size_t i = 0; i < len; i++)
  {
    out += DIGITS[bytes[i] >> 4];
    out += DIGITS[bytes[i] & 0x0F];
  }
  return out;
}
} // namespace

void FirmwareUpdater::service()
{
  if (!pending_) return;
  pending_ = false;
  String payload = pendingPayload_;
  pendingPayload_ = "";
  run(payload);
}

// Every way an update can end without one: the half-written slot is dropped,
// the screen goes back, and the board keeps running what it had.
bool FirmwareUpdater::fail(const String &updateId, const String &error, bool updateBegun)
{
  Serial.printf("[FirmwareUpdater] %s\n", error.c_str());
  if (updateBegun) Update.abort();
  publishStatus_(updateId, "error", error, -1);
  if (onFailed_) onFailed_();
  busy_ = false;
  return false;
}

void FirmwareUpdater::run(const String &payload)
{
  if (!publishStatus_) return;

  // The clearing publish comes back here as an empty payload, which fails to
  // parse and is ignored - not a second update.
  JsonDocument doc;
  if (payload.isEmpty() || deserializeJson(doc, payload)) return;

  const String updateId = doc["updateId"] | "";
  const String url = doc["url"] | "";
  String sha256 = doc["sha256"] | "";
  const long size = doc["size"] | -1L;
  const String deviceId = doc["deviceId"] | "";
  const String build = doc["build"] | "";
  const bool force = doc["force"] | false;
  sha256.toLowerCase();
  if (updateId.isEmpty() || url.isEmpty() || sha256.length() != 64)
  {
    Serial.println("[FirmwareUpdater] firmware message without updateId, url or sha256 - ignored");
    return;
  }

  if (clearTrigger_) clearTrigger_();

  if (busy_)
  {
    publishStatus_(updateId, "busy", "", -1);
    return;
  }

  // Cheap refusals first, before a byte is downloaded. The marker check below
  // is the one that cannot be talked round; this one only saves the download
  // when the designer itself already knows the image is for someone else.
  if (!deviceId.isEmpty() && deviceId != DEVICE_ID)
  {
    publishStatus_(updateId, "error", "Firmware for " + deviceId + ", this device is " + DEVICE_ID, -1);
    return;
  }
  if (!force && !build.isEmpty() && build == FIRMWARE_BUILD)
  {
    Serial.printf("[FirmwareUpdater] already running %s\n", FIRMWARE_BUILD);
    publishStatus_(updateId, "up_to_date", "", -1);
    return;
  }

  busy_ = true;
  Serial.printf("[FirmwareUpdater] update %s from %s\n", updateId.c_str(), url.c_str());
  if (onStart_) onStart_();
  publishStatus_(updateId, "downloading", "", 0);

  HTTPClient http;
  if (!http.begin(url)) return (void)fail(updateId, "Download failed (bad URL)", false);
  const int code = http.GET();
  if (code != HTTP_CODE_OK)
  {
    http.end();
    return (void)fail(updateId, "Download failed (HTTP " + String(code) + ")", false);
  }

  int total = http.getSize();
  if (size > 0 && total > 0 && total != size)
  {
    http.end();
    return (void)fail(updateId, "Download is " + String(total) + " bytes, expected " + String(size), false);
  }
  if (total <= 0) total = (int)size;
  if (total <= 0)
  {
    http.end();
    return (void)fail(updateId, "Download failed (no size)", false);
  }

  if (!Update.begin((size_t)total))
  {
    http.end();
    return (void)fail(updateId, String("Could not start: ") + Update.errorString(), false);
  }

  FirmwareImage::IdentityScanner identity;
  mbedtls_sha256_context sha;
  mbedtls_sha256_init(&sha);
  mbedtls_sha256_starts(&sha, 0);

  WiFiClient *stream = http.getStreamPtr();
  uint8_t buf[4096];
  int written = 0;
  int lastPercent = -1;
  unsigned long lastDataMs = millis();
  String error;

  while (written < total)
  {
    const size_t avail = stream->available();
    if (avail == 0)
    {
      if (!http.connected() && stream->available() == 0)
      {
        error = "Download ended early";
        break;
      }
      if (millis() - lastDataMs > STALL_MS)
      {
        error = "Download stalled";
        break;
      }
      delay(1);
      continue;
    }
    const size_t want = min(min(avail, sizeof(buf)), (size_t)(total - written));
    const int got = stream->readBytes(buf, want);
    if (got <= 0) continue;
    lastDataMs = millis();

    identity.feed(buf, (size_t)got);
    mbedtls_sha256_update(&sha, buf, (size_t)got);
    if (Update.write(buf, (size_t)got) != (size_t)got)
    {
      error = String("Write failed: ") + Update.errorString();
      break;
    }
    written += got;

    // Every 10%, like a project deploy - the designer's progress bar cannot
    // tell the difference, and a burst of publishes is what once silenced
    // deploy-status altogether (DeployManager.cpp).
    const int percent = (int)((int64_t)written * 100 / total / 10 * 10);
    if (percent != lastPercent)
    {
      lastPercent = percent;
      publishStatus_(updateId, "downloading", "", percent);
    }
  }
  http.end();

  uint8_t digest[32];
  mbedtls_sha256_finish(&sha, digest);
  mbedtls_sha256_free(&sha);

  if (!error.isEmpty()) return (void)fail(updateId, error, true);
  publishStatus_(updateId, "download_complete", "", 100);

  publishStatus_(updateId, "verifying", "", -1);
  const String got = toHex(digest, sizeof(digest));
  if (got != sha256) return (void)fail(updateId, "Checksum mismatch", true);
  if (!identity.found())
    return (void)fail(updateId, String("Not a firmware image for ") + DEVICE_ID + " - refused, nothing installed", true);

  publishStatus_(updateId, "applying", "", -1);
  if (!Update.end(true)) return (void)fail(updateId, String("Could not finish: ") + Update.errorString(), false);

  publishStatus_(updateId, "rebooting", "", -1);
  Serial.printf("[FirmwareUpdater] %d bytes installed, restarting\n", written);
  delay(500); // let "rebooting" leave before the restart takes the radio down
  ESP.restart();
}
