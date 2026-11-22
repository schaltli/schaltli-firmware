#pragma once

// The LilyGO T-Encoder Pro's numbers that shared files outside main.cpp need
// (CanvasDisplayAdapter's round setup screen). Constants only; the hardware
// is BoardHardware.h. See the knob's BoardProfile.h for the split.

// 1.2" round AMOLED, a square framebuffer showing its inscribed circle.
#define BOARD_SCREEN_SIZE 390

// The setup screen's QR code, quiet zone included: about as many millimetres
// as the knob's 130 px (12.8 px/mm here against 7.88).
#define BOARD_SETUP_QR_PX 190
