#pragma once

// The Waveshare ESP32-S3-Touch-LCD-7 (800x480, ST7262, GT911, CH422G).
//
// Electrically a 4.3B with a larger glass: Waveshare's own board
// configurations for the two differ only in the board name (diffed
// 2026-11-12, designer tasks/waveshare-7-plan.md), and the 4.3B's image ran
// unchanged on a forum user's 7" Rev 1.2, picture and touch (2026-11-17).
// So this file sets the values that differ and takes everything else from
// the 4.3B's profile.

#define BOARD_LOG "[ws7] "

// 16 MHz, Waveshare's value, where the 4.3B runs at 12 for its WiFi
// (waveshare4v3b/BoardProfile.h). A second user's 7", also Rev 1.2, crashed
// at 12 MHz two seconds into the first boot ("Cache disabled but cached
// memory region accessed", while NVS was being written) and ran at 16 with
// the 4.3B's bounce buffer unchanged (Pekaway forum 2558 #27, #30,
// 2026-11-17/18). Whether 16 slows this board's WiFi as it did the 4.3B's
// is not measured; ?set=pclk= is there for that.
#define BOARD_PCLK_HZ (16 * 1000 * 1000)

// All CH422G lines high at start, as on the 4.3B, except EXIO5 USB_SEL. On
// the 7" USB_SEL high puts GPIO19/20 on the CAN transceiver (FSUSB42UMX)
// and the USB-C socket marked USB goes dead, so neither the log nor an
// update over that socket reaches the board (Waveshare's examples say so;
// forum 2558 #25).
#define CH422G_IO_AT_START (0xFF & ~(1u << 5))

#include "../waveshare4v3b/BoardProfile.h"
