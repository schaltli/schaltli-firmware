#pragma once
#include <Arduino.h>
#include "DeviceInfo.h"

// Which firmware images a board will install, and how it tells.
//
// Until 2026-09-15 POST /api/firmware installed any image with an ESP32-S3
// header - the knob's firmware would have gone onto the 4.3B, and a board
// running the wrong firmware may not come back up far enough to be given the
// right one without a cable. Every image now carries a marker naming the
// DEVICE_ID it was built for, and an upload whose stream never contained this
// board's marker is aborted before Update.end(), so the running slot stays
// bootable (designer repo, docs/2026-09-15-firmware-ota.md, decision 5).
//
// The marker is one string literal, and the scanner below searches for that
// very literal - which is what guarantees it is in the image: code that uses
// it cannot be linked without it. A marker only written into the image and
// never read could be dropped by --gc-sections without anyone noticing.
//
// It guards against mistakes, not intent. Anyone who can reach the board can
// still upload an image carrying the marker; the network is the trust
// boundary here, as it is for project deploys.
//
// A consequence worth knowing: an image built before this date carries no
// marker and can no longer be installed over the network. Going back that
// far takes a cable.
namespace FirmwareImage
{
inline const char *marker()
{
  static const char MARKER[] = "<<screenbee-image device=" DEVICE_ID ">>";
  return MARKER;
}

// Finds marker() in a stream fed in arbitrary chunks, matches split across a
// chunk boundary included (Knuth-Morris-Pratt, so no byte is looked at twice
// and nothing needs buffering).
class IdentityScanner
{
public:
  IdentityScanner() { reset(); }

  void reset()
  {
    needle_ = marker();
    length_ = strlen(needle_);
    if (length_ > sizeof(fail_)) length_ = sizeof(fail_);
    fail_[0] = 0;
    for (size_t i = 1, k = 0; i < length_; i++)
    {
      while (k > 0 && needle_[i] != needle_[k]) k = fail_[k - 1];
      if (needle_[i] == needle_[k]) k++;
      fail_[i] = (uint8_t)k;
    }
    matched_ = 0;
    found_ = false;
  }

  void feed(const uint8_t *data, size_t len)
  {
    if (found_) return;
    for (size_t i = 0; i < len; i++)
    {
      const char c = (char)data[i];
      while (matched_ > 0 && c != needle_[matched_]) matched_ = fail_[matched_ - 1];
      if (c == needle_[matched_]) matched_++;
      if (matched_ == length_)
      {
        found_ = true;
        return;
      }
    }
  }

  bool found() const { return found_; }

private:
  const char *needle_ = nullptr;
  size_t length_ = 0;
  uint8_t fail_[96];
  size_t matched_ = 0;
  bool found_ = false;
};

// One line for a board's /api/debug. hil/firmware-upload.js reads it before
// it ever sends a foreign image: a board that does not report it may still
// install anything, and sending it one would install it.
inline String debugLine()
{
  return String("firmware build ") + FIRMWARE_BUILD + ", installs only images for " + DEVICE_ID + "\n";
}
} // namespace FirmwareImage
