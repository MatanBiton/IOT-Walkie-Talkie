# Walkie-Talkie Project — Group 14
Group members: Erel Hadad, Sapir Kvetny, Matan Biton  

Advisors: Rami Abu Much, Tom Sofer
## Details about the project

A half-duplex ESP32 walkie-talkie with channel selection through an OLED interface. Audio is transmitted through Firebase Realtime Database in **VoIP mode** and can fall back to **ESP-NOW P2P mode** after repeated upload failures. The system also provides user-availability checks, adjustable audio settings, communication-status feedback, and a companion Flutter application.

Before flashing each ESP32, configure its unique `DEVICE_ID` and `USER_ID`, Wi-Fi settings, and Firebase RTDB URL in `ESP32/app_config.h`.

## Folder description

* `ESP32/`: ESP32 firmware source code.
* `Documentation/`: wiring diagram, Fritzing connection diagram file and basic operating instructions.
* `Unit Tests/`: tests for individual input and output hardware components.
* `assets/`: project poster.

## ESP32 SDK version used in this project

* **ESP32 Arduino Core by Espressif Systems:** `2.0.17`

## Arduino/ESP32 libraries used in this project

* **FirebaseClient by Mobizt** — version **2.2.13** or a compatible 2.2.x release.
* **ESP_SSLClient** — **3.1.3**.
* **Adafruit GFX Library** — **1.12.6**.
* **Adafruit SSD1306** — **2.5.17**.
* **WiFi, Wire, ESP-NOW, I2S, FreeRTOS and mbedTLS** — included with the ESP32 Arduino Core.

## Hardware list

Quantities below are for **one Walkie-Talkie device**.

|  Quantity | Component                                      | Purpose                                            |
| --------: | ---------------------------------------------- | -------------------------------------------------- |
|         1 | ESP32 development board                        | Main controller, Wi-Fi and ESP-NOW communication   |
|         1 | INMP441 I2S microphone                         | Captures voice input                               |
|         1 | max98357 audio DAC amplifier                   | Drives the speaker                                 |
|         1 | Speaker                                        | Plays received audio                               |
|         1 | 128×64 SSD1306 I2C OLED display                | Displays channels, status, settings and statistics |
|         3 | Momentary push buttons                         | PTT, left navigation and right navigation          |
|         1 | LED                                            | Indicates recording and pending communication work |
|         1 | LED current-limiting resistor                  | Protects the status LED                            |
|         1 | USB cable and suitable USB power source        | Programs and powers the ESP32                      |
| As needed | Breadboard, perfboard or custom mounting board | Holds and connects the components                  |
| As needed | Jumper wires / hookup wire                     | Electrical connections                             |
|     1 set | 3D-printed enclosure parts                     | Houses the completed device                        |


## Connection diagram

| Component       | ESP32 connection                             |
| --------------- | -------------------------------------------- |
| I2S speaker     | LRC/WS: GPIO 12, BCLK: GPIO 14, DIN: GPIO 27 |
| I2S microphone  | SD: GPIO 34, SCK/BCLK: GPIO 32, WS: GPIO 33  |
| Main PTT button | GPIO 25 to physical GND                      |
| Left button     | Input: GPIO 4, virtual GND: GPIO 15          |
| Right button    | Input: GPIO 19, virtual GND: GPIO 5          |
| SSD1306 OLED    | SCL: GPIO 22, SDA: GPIO 23                   |
| Status LED      | GPIO 26 through a current-limiting resistor  |

The buttons use `INPUT_PULLUP`, so a pressed button reads `LOW`. GPIO 15 and GPIO 5 are configured as outputs and held `LOW` to act as virtual grounds.

![Connection diagram](Documentation/Walki-Talkie_Diagram.png)

## Project Poster

![Project poster](assets/poster.png)

---

This project is part of [ICST — The Interdisciplinary Center for Smart Technologies](https://icst.cs.technion.ac.il/), Taub Faculty of Computer Science, Technion.
