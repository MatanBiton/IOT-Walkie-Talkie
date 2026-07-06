#pragma once

#include <Arduino.h>

/*
 * Project-wide constants.
 *
 * GPIO source:
 *   Documentation/connection diagram/esp32_project_connections.txt
 *
 * Button wiring:
 *   ESP32 GPIO -> button -> GND
 *   pinMode(..., INPUT_PULLUP)
 *   pressed  = LOW
 *   released = HIGH
 */

namespace Pins {

// ---------------- User inputs ----------------
constexpr int MAIN_BUTTON      = 14;  // Existing push-to-talk / main button
constexpr int GUI_BUTTON_LEFT  = 18;  // Scroll / back
constexpr int GUI_BUTTON_RIGHT = 23;  // Select

// ---------------- Indicators ----------------
constexpr int RECORDING_LED = 4;

// ---------------- I2S microphone ----------------
constexpr int MIC_BCLK = 32;  // SCK / BCLK
constexpr int MIC_WS   = 25;  // WS / LRCLK
constexpr int MIC_SD   = 33;  // SD / DOUT

// ---------------- I2S speaker amplifier ----------------
constexpr int SPEAKER_BCLK = 27;  // BCLK / BCK
constexpr int SPEAKER_WS   = 26;  // LRC / WS
constexpr int SPEAKER_DIN  = 22;  // DIN / DATA

// ---------------- OLED display, SSD1306, I2C ----------------
constexpr int OLED_SDA = 21;
constexpr int OLED_SCL = 19;

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
