#pragma once

// The Waveshare Knob-Touch LCD 1.8's numbers that shared files outside
// main.cpp need (CanvasDisplayAdapter's round setup screen). Constants only,
// safe to include from any translation unit; the hardware itself - panel,
// touch chip, encoder - is BoardHardware.h, which only main.cpp includes.
//
// The round firmware (src/main.cpp) runs on more than one board since the
// LilyGO T-Encoder Pro (designer tasks/lilygo-t-encoder-pro-plan.md,
// 2026-11-22); each board's directory holds these two headers, and the env's
// -I picks the directory.

// The panel is a square framebuffer showing its inscribed circle.
#define BOARD_SCREEN_SIZE 360

// The setup screen's QR code, quiet zone included. 130 scans reliably on
// this panel (verified on hardware, 2026-08).
#define BOARD_SETUP_QR_PX 130
