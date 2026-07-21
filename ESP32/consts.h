#pragma once

#include <Arduino.h>

/*
 * Project-wide GPIO mapping.
 *
 * Button wiring:
 *   Main button: ESP32 GPIO -> button -> physical GND
 *   GUI buttons: input GPIO -> button -> virtual-ground GPIO
 *
 * All button inputs use INPUT_PULLUP:
 *   pressed  = LOW
 *   released = HIGH
 *
 * The virtual-ground GPIOs must be configured as OUTPUT and held LOW before
 * the GUI button inputs are initialized.
 */

namespace Pins {

// ---------------- User inputs ----------------
constexpr int MAIN_BUTTON      = 25;  // Push-to-talk / main button -> physical GND
constexpr int GUI_BUTTON_LEFT  = 4;   // Scroll / back
constexpr int GUI_BUTTON_RIGHT = 19;  // Select

// GPIO outputs held LOW to act as button grounds.
constexpr int GUI_BUTTON_LEFT_VIRTUAL_GND  = 15;
constexpr int GUI_BUTTON_RIGHT_VIRTUAL_GND = 5;

// ---------------- Indicators ----------------
constexpr int RECORDING_LED = 26;

// ---------------- I2S microphone ----------------
constexpr int MIC_BCLK = 32;  // SCK / BCLK
constexpr int MIC_WS   = 33;  // WS / LRCLK
constexpr int MIC_SD   = 34;  // SD / DOUT (input-only GPIO is valid here)

// ---------------- I2S speaker amplifier ----------------
constexpr int SPEAKER_BCLK = 14;  // BCLK / BCK
constexpr int SPEAKER_WS   = 12;  // LRC / WS
constexpr int SPEAKER_DIN  = 27;  // DIN / DATA

// ---------------- OLED display, SSD1306, I2C ----------------
constexpr int OLED_SDA = 23;
constexpr int OLED_SCL = 22;

}  // namespace Pins

namespace DisplayConfig {
constexpr int SCREEN_WIDTH  = 128;
constexpr int SCREEN_HEIGHT = 64;

// Most 0.96" SSD1306 I2C modules use 0x3C.
// Change only here if your module uses another I2C address.
constexpr int OLED_I2C_ADDRESS = 0x3C;

// SSD1306 reset pin is not connected in the current wiring.
constexpr int OLED_RESET_PIN = -1;
}  // namespace DisplayConfig

namespace ButtonLogic {
constexpr int PRESSED  = LOW;
constexpr int RELEASED = HIGH;
constexpr unsigned long DEBOUNCE_MS = 45;
}  // namespace ButtonLogic
