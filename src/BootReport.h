#pragma once
#include <Arduino.h>
#include <esp_ota_ops.h>
#include <esp_system.h>
#include <esp_core_dump.h>

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

// The last panic's core dump, as far as the board itself can tell it: the
// task, the program counter and the backtrace, for xtensa-esp32s3-elf-addr2line
// against the firmware.elf of the build named by its sha. It stays in the
// coredump partition over restarts and firmware uploads until the next panic.
//
// Added 2026-10-30, when the 4.3B panicked several times in the van with no
// serial line attached - the reset reason said panic, and nothing said where.
inline String coreDump()
{
#if CONFIG_ESP_COREDUMP_ENABLE_TO_FLASH && CONFIG_ESP_COREDUMP_DATA_FORMAT_ELF
  esp_core_dump_summary_t *s = (esp_core_dump_summary_t *)malloc(sizeof(esp_core_dump_summary_t));
  if (!s) return String("{\"error\":\"no memory\"}");
  const esp_err_t err = esp_core_dump_get_summary(s);
  if (err != ESP_OK)
  {
    free(s);
    return String("{\"error\":\"") + esp_err_to_name(err) + "\"}";
  }
  char hex[12];
  String out = String("{\"task\":\"") + String(s->exc_task).substring(0, 15) + "\"";
  snprintf(hex, sizeof(hex), "0x%08lx", (unsigned long)s->exc_pc);
  out += String(",\"pc\":\"") + hex + "\"";
  out += String(",\"cause\":") + String((unsigned long)s->ex_info.exc_cause);
  snprintf(hex, sizeof(hex), "0x%08lx", (unsigned long)s->ex_info.exc_vaddr);
  out += String(",\"vaddr\":\"") + hex + "\"";
  out += ",\"backtrace\":[";
  for (uint32_t i = 0; i < s->exc_bt_info.depth && i < 16; i++)
  {
    snprintf(hex, sizeof(hex), "0x%08lx", (unsigned long)s->exc_bt_info.bt[i]);
    out += String(i ? ",\"" : "\"") + hex + "\"";
  }
  out += String("],\"corrupted\":") + (s->exc_bt_info.corrupted ? "true" : "false");
  out += ",\"elf\":\"";
  for (int i = 0; i < APP_ELF_SHA256_SZ && s->app_elf_sha256[i]; i++) out += (char)s->app_elf_sha256[i];
  out += "\"}";
  free(s);
  return out;
#else
  return String("{\"error\":\"no core dump in this build\"}");
#endif
}
} // namespace BootReport
