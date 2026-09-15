#pragma once
#include <Arduino.h>
#include <esp_ota_ops.h>
#include <esp_system.h>

// Why the board last started, and which app slot it started from - one line
// for a board's /api/debug.
//
// Added 2026-09-15, after a firmware upload to the PaperS3 got no answer, the
// board restarted, and it came back on the firmware it had before. Twelve
// uploads in a row afterwards all landed, and nothing on the board could say
// whether that one had been a crash mid-upload: by the time anyone looked,
// several restarts had overwritten the evidence. The reset reason is that
// evidence, and the slot is what tells a landed upload from one that did
// not when the image uploaded is the one already running - an update always
// boots from the other slot. hil/firmware-upload.js in the designer repo
// asserts both.
namespace BootReport
{
inline const char *resetReason()
{
  switch (esp_reset_reason())
  {
  case ESP_RST_POWERON: return "power-on";
  case ESP_RST_EXT: return "external pin";
  case ESP_RST_SW: return "software";
  case ESP_RST_PANIC: return "panic";
  case ESP_RST_INT_WDT: return "interrupt watchdog";
  case ESP_RST_TASK_WDT: return "task watchdog";
  case ESP_RST_WDT: return "other watchdog";
  case ESP_RST_DEEPSLEEP: return "deep sleep";
  case ESP_RST_BROWNOUT: return "brownout";
  case ESP_RST_SDIO: return "sdio";
  case ESP_RST_USB: return "usb";
  case ESP_RST_JTAG: return "jtag";
  default: return "unknown";
  }
}

inline String line()
{
  const esp_partition_t *running = esp_ota_get_running_partition();
  return String("last reset ") + resetReason() + ", running from " + (running ? running->label : "?") + "\n";
}
} // namespace BootReport
