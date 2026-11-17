#ifndef _M5NSDEVICE_H
#define _M5NSDEVICE_H

// Single include point for the device layer. M5Stack boards go through M5Unified
// (auto-detect Basic/Fire/Core2/CoreS3); non-M5 boards (Guition JC3248W535, Waveshare
// ESP32-Touch-LCD-3.5) get a shim that exposes the
// same `M5` object surface (Lcd/Display/BtnA-C/Touch/Speaker/Power/begin/update),
// so the sketch and the web-config code compile unchanged for every target.

#include <SD.h>          // must precede M5GFX/M5Unified so the fs::FS (SD) image overloads are enabled

#if defined(DEVICE_JC3248W535)
  #include "hal_jc3248w535.h"
  inline int halSafeLedPin(int pin) { return pin; }
#elif defined(DEVICE_WS_TOUCH_LCD_35)
  #include "hal_ws_touchlcd35.h"
  inline int halSafeLedPin(int pin) { return pin; }
#else
  #include <M5Unified.h>
  // SD wiring differs per device: M5 boards are handled by the variant's default CS,
  // the shim boards bring their own SPI bus (see their halSDBegin()).
  inline bool halSDBegin() { return SD.begin(); }
  // On the Core2, GPIO 15 is the screen's D/C line: an LED strip on pin 15 turns the screen
  // black. The M5GO battery base's side lights are on GPIO 25, so on a Core2 pin 15 becomes 25.
  inline int halSafeLedPin(int pin) {
    return (M5.getBoard() == m5::board_t::board_M5StackCore2 && pin == 15) ? 25 : pin;
  }
#endif

#endif
