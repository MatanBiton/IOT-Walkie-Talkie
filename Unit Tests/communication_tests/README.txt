ESP32 Walkie-Talkie Experiments

This folder contains three successful ESP-NOW communication experiments using two ESP32 boards.

Hardware used:
- Two ESP32 boards with CP2102 USB-serial chips
- Push button
- I2S speaker amplifier + speaker
- I2S microphone
- LED + 220-330 ohm resistor

General button wiring:
GPIO14 -> button -> GND

The button code uses INPUT_PULLUP:
not pressed = HIGH
pressed     = LOW

Recording LED wiring:
GPIO4 -> resistor -> long LED leg
short LED leg -> GND


Experiment 1: Basic ESP-NOW byte communication

Files:
sender.ino
receiver.ino

Goal:
Press button on sender -> send a small packet
Receiver -> print received bytes

Sender hardware:
GPIO14 -> button -> GND

Receiver hardware:
No extra hardware.
Only USB connection for Serial Monitor.


Experiment 2: Fake audio packet + speaker playback

Files:
sender_fake_sound.ino
receiver_with_speaker.ino

Goal:
Sender generates fake audio samples (short pulses) as long as button is pressed.
Receiver receives them and plays them through the speaker.

Sender hardware:
GPIO14 -> button -> GND

Receiver hardware:
I2S speaker amplifier:
ESP32 GPIO27 -> BCLK / BCK
ESP32 GPIO26 -> LRC / WS
ESP32 GPIO22 -> DIN / DATA
ESP32 GND    -> GND
ESP32 5V/VIN -> VIN / VCC

Speaker:
Speaker + -> amplifier +
Speaker - -> amplifier -


Experiment 3: Microphone recording, send, and play

Files:
sender_with_microphone.ino
receiver_mic_to_speaker.ino

Goal:
First button press  -> start recording
Second button press -> stop recording
Auto-stop           -> after 5 seconds
Then sender transmits the recording over ESP-NOW.
Receiver plays the recording through the speaker.

Sender hardware:
Button:
GPIO14 -> button -> GND

Recording LED:
GPIO4 -> resistor -> long LED leg
short LED leg -> GND

I2S microphone:
Mic VCC / VDD  -> 3.3V
Mic GND        -> GND
Mic SCK / BCLK -> GPIO32
Mic WS / LRCLK -> GPIO25
Mic SD / DOUT  -> GPIO33


Receiver hardware:
I2S speaker amplifier:
ESP32 GPIO27 -> BCLK / BCK
ESP32 GPIO26 -> LRC / WS
ESP32 GPIO22 -> DIN / DATA
ESP32 GND    -> GND
ESP32 5V/VIN -> VIN / VCC

Speaker:
Speaker + -> amplifier +
Speaker - -> amplifier -


Important notes:

The receiver MAC address is hardcoded in the sender code:
uint8_t receiverMac[] = {0xF4, 0x65, 0x0B, 0xE9, 0x3B, 0x64};

If using a different receiver ESP, update this MAC address.

The final recording experiment used a lower sample rate than the original mic test to reduce memory usage and packet pressure.