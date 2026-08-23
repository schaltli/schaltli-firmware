#pragma once
#include <Arduino.h>
#include <initializer_list>

/**
 * Interface for display operations
 * Allows display implementations to be swapped without affecting business logic
 */
class IDisplay {
public:
  virtual ~IDisplay() = default;
  
  /**
   * Display multiple lines of text
   */
  virtual void showLines(std::initializer_list<String> lines) = 0;
  
  /**
   * Clear the display
   */
  virtual void clear() = 0;

  /**
   * Show a QR code with a caption above and status lines below, for the
   * setup screen: the code carries a WIFI: URI so a phone camera can join
   * the device's AP in one tap, which is the difference between a setup
   * screen someone can use and one they have to transcribe a password from.
   *
   * Default is the caption and lines as plain text - a display that cannot
   * draw a QR code (an e-paper panel too small for one, a text-only
   * device) is still correct, just less convenient, and does not have to
   * implement anything.
   */
  virtual void showQrScreen(const String& qrData, const String& caption,
                            std::initializer_list<String> lines) {
    (void)qrData;
    showLines(lines);
    (void)caption;
  }
};


