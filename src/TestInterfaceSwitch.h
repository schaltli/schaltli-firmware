#pragma once
#include <Arduino.h>

// The test interface switched over USB (DeviceSettings.h): one line on the
// serial port, 115200 baud -
//
//   hil-on <password>   on, with that password, then a restart
//   hil-off             off, then a restart
//   hil?                says whether it is on
//
// The other way besides the setup portal, and the one HIL uses: the boards
// on the test bench hang on USB anyway, and USB, like the portal, needs the
// device in someone's hands (hil/test-interface.js in the designer repo).
class TestInterfaceSwitch {
public:
  // From loop(). Reads what has arrived, never waits.
  void service();

private:
  void run(const String &line);
  String line_;
};
