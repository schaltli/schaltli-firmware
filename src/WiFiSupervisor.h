#pragma once
#include <Arduino.h>
#include <WiFi.h>

// Keeps a board's station link up after boot, so the boot's own wait for
// WiFi is a wait and never a verdict.
//
// Written 2026-09-14 for the 4.3B and the PaperS3, whose boot gave WiFi ten
// seconds and, when the connection took longer, returned before the test
// interface and MQTT were set up. The core connected by itself a moment
// later, leaving a board that answered ping and refused port 80 until the
// next restart - found at the end of a conformance run, which restarts the
// board after every install. hil/late-wifi.js reproduces it.
//
// The knob has had its own version of this since 2026-08 (superviseWiFi() in
// src/main.cpp), including the lesson this copies: the core gives up for
// good on some disconnect reasons, so after a grace period this calls
// WiFi.begin() itself, again every retry interval, never blocking - begin()
// returns at once and the association runs in the background. It also puts
// the radio's power save back off after every reconnect, which costs nothing
// and makes "does the core forget it" a question nobody has to answer.
class WiFiSupervisor {
public:
  static const uint32_t GRACE_MS = 12000;
  static const uint32_t RETRY_INTERVAL_MS = 15000;

  // beginAfterMs > 0 holds the first WiFi.begin() back that long - the
  // ?set=latewifi test hook, nothing else.
  void begin(const String &ssid, const String &password, uint32_t beginAfterMs = 0)
  {
    ssid_ = ssid;
    password_ = password;
    active_ = true;
    WiFi.mode(WIFI_STA);
    WiFi.setSleep(false);
    if (beginAfterMs > 0)
    {
      beginAtMs_ = millis() + beginAfterMs;
      if (beginAtMs_ == 0) beginAtMs_ = 1;
    }
    else
    {
      WiFi.begin(ssid_.c_str(), password_.c_str());
    }
  }

  // Every loop turn. True on the turn the link comes up - first time or back.
  bool tick()
  {
    if (!active_) return false;
    const uint32_t now = millis();

    if (beginAtMs_ && now >= beginAtMs_)
    {
      beginAtMs_ = 0;
      WiFi.begin(ssid_.c_str(), password_.c_str());
      Serial.println("[wifi] held-back WiFi.begin() now");
    }

    if (WiFi.status() == WL_CONNECTED)
    {
      downSinceMs_ = 0;
      if (!up_)
      {
        up_ = true;
        ups_++;
        WiFi.setSleep(false);
        Serial.printf("[wifi] up, ip %s, rssi %d dBm (%u times so far, %u own retries)\n",
                      WiFi.localIP().toString().c_str(), WiFi.RSSI(), (unsigned)ups_,
                      (unsigned)retries_);
        return true;
      }
      return false;
    }

    up_ = false;
    if (beginAtMs_) return false;
    if (downSinceMs_ == 0)
    {
      downSinceMs_ = now;
      lastRetryMs_ = now;
      return false;
    }
    if (now - downSinceMs_ < GRACE_MS || now - lastRetryMs_ < RETRY_INTERVAL_MS) return false;

    lastRetryMs_ = now;
    retries_++;
    WiFi.begin(ssid_.c_str(), password_.c_str());
    Serial.printf("[wifi] retry %u, %us without a link\n", (unsigned)retries_,
                  (unsigned)((now - downSinceMs_) / 1000));
    return false;
  }

  bool up() const { return up_; }

  String report() const
  {
    return String("wifi ") + (up_ ? "up" : "down") + ", came up " + String(ups_) + " time(s), " +
           String(retries_) + " own retr" + (retries_ == 1 ? "y" : "ies") + "\n";
  }

private:
  String ssid_;
  String password_;
  bool active_ = false;
  bool up_ = false;
  uint32_t beginAtMs_ = 0;
  uint32_t downSinceMs_ = 0;
  uint32_t lastRetryMs_ = 0;
  uint32_t ups_ = 0;
  uint32_t retries_ = 0;
};
