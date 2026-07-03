/*
  ESP32 + SSD1306 OLED two-button GUI

  Based on the project connection document:
  - OLED SDA  -> GPIO21
  - OLED SCL  -> GPIO19
  - OLED VCC  -> 3.3V
  - OLED GND  -> GND
  - GUI Button 1 / Left  -> GPIO18 -> button -> GND
  - GUI Button 2 / Right -> GPIO23 -> button -> GND
  - Existing main button -> GPIO14 -> button -> GND
  - Recording LED        -> GPIO4  -> resistor -> LED anode, LED cathode -> GND

  Required Arduino libraries:
  - Adafruit GFX Library
  - Adafruit SSD1306

  Controls:
  - GUI Button 1: move to next option / change value
  - GUI Button 2: select option / go back

  Notes:
  - All buttons are configured as INPUT_PULLUP, so pressed = LOW.
  - This sketch does not initialize I2S, so it should not interfere with the
    microphone/speaker pins. Use it as a GUI base, then connect menu actions
    to your existing audio code.
*/

#include <Wire.h>
#include <Adafruit_GFX.h>
#include <Adafruit_SSD1306.h>

// ------------------------- Pin configuration -------------------------
static const uint8_t PIN_RECORDING_LED = 4;
static const uint8_t PIN_MAIN_BUTTON   = 14;
static const uint8_t PIN_GUI_BUTTON_1  = 18;  // Left button: next/change
static const uint8_t PIN_GUI_BUTTON_2  = 23;  // Right button: select/back

static const uint8_t OLED_SDA = 21;
static const uint8_t OLED_SCL = 19;

// Existing I2S pins are documented here only. This sketch does not use them.
static const uint8_t MIC_BCLK = 32;
static const uint8_t MIC_WS   = 25;
static const uint8_t MIC_SD   = 33;
static const uint8_t AMP_BCLK = 27;
static const uint8_t AMP_WS   = 26;
static const uint8_t AMP_DIN  = 22;

// ------------------------- OLED configuration -------------------------
static const int SCREEN_WIDTH  = 128;
static const int SCREEN_HEIGHT = 64;
static const int OLED_RESET    = -1;
static const uint8_t SCREEN_ADDRESS = 0x3C;  // If the screen is blank, try 0x3D.

Adafruit_SSD1306 display(SCREEN_WIDTH, SCREEN_HEIGHT, &Wire, OLED_RESET);

// ------------------------- Button debouncing -------------------------
class DebouncedButton {
public:
  explicit DebouncedButton(uint8_t pin) : pin_(pin) {}

  void begin() {
    pinMode(pin_, INPUT_PULLUP);
    lastReading_ = digitalRead(pin_);
    stableState_ = lastReading_;
    lastChangeMs_ = millis();
  }

  void update() {
    bool reading = digitalRead(pin_);

    if (reading != lastReading_) {
      lastReading_ = reading;
      lastChangeMs_ = millis();
    }

    if ((millis() - lastChangeMs_) >= debounceMs_ && reading != stableState_) {
      stableState_ = reading;
      if (stableState_ == LOW) {
        pressEvent_ = true;
      }
    }
  }

  bool wasPressed() {
    if (!pressEvent_) {
      return false;
    }
    pressEvent_ = false;
    return true;
  }

  bool isDown() const {
    return stableState_ == LOW;
  }

private:
  uint8_t pin_;
  bool lastReading_ = HIGH;
  bool stableState_ = HIGH;
  bool pressEvent_ = false;
  unsigned long lastChangeMs_ = 0;
  static const unsigned long debounceMs_ = 35;
};

DebouncedButton guiButton1(PIN_GUI_BUTTON_1);
DebouncedButton guiButton2(PIN_GUI_BUTTON_2);
DebouncedButton mainButton(PIN_MAIN_BUTTON);

// ------------------------- GUI state -------------------------
enum ScreenState {
  SCREEN_MENU,
  SCREEN_STATUS,
  SCREEN_RECORDING_LED,
  SCREEN_INVERT_DISPLAY,
  SCREEN_BUTTON_TEST,
  SCREEN_ABOUT
};

ScreenState currentScreen = SCREEN_MENU;

const char* menuItems[] = {
  "Status",
  "Recording LED",
  "Invert OLED",
  "Button test",
  "About"
};
static const int MENU_COUNT = sizeof(menuItems) / sizeof(menuItems[0]);

int selectedItem = 0;
bool recordingLedOn = false;
bool oledInverted = false;
bool redrawRequested = true;
unsigned long lastLiveRefreshMs = 0;

// ------------------------- Drawing helpers -------------------------
void drawHeader(const char* title) {
  display.clearDisplay();
  display.setTextColor(SSD1306_WHITE);
  display.setTextSize(1);
  display.setCursor(0, 0);
  display.print(title);
  display.drawLine(0, 10, SCREEN_WIDTH - 1, 10, SSD1306_WHITE);
}

void drawFooter(const char* text) {
  display.drawLine(0, 54, SCREEN_WIDTH - 1, 54, SSD1306_WHITE);
  display.setTextColor(SSD1306_WHITE);
  display.setTextSize(1);
  display.setCursor(0, 56);
  display.print(text);
}

void drawMenu() {
  drawHeader("ESP32 OLED GUI");

  // Show up to four menu rows at once. Scroll in groups of four if needed.
  int firstVisible = (selectedItem / 4) * 4;
  int lastVisible = min(firstVisible + 4, MENU_COUNT);

  for (int i = firstVisible; i < lastVisible; ++i) {
    int row = i - firstVisible;
    int y = 14 + row * 10;

    if (i == selectedItem) {
      display.fillRect(0, y - 1, SCREEN_WIDTH, 9, SSD1306_WHITE);
      display.setTextColor(SSD1306_BLACK);
    } else {
      display.setTextColor(SSD1306_WHITE);
    }

    display.setCursor(2, y);
    display.print(i == selectedItem ? "> " : "  ");
    display.print(menuItems[i]);
  }

  display.setTextColor(SSD1306_WHITE);
  drawFooter("B1 next  B2 select");
  display.display();
}

void drawStatusScreen() {
  drawHeader("Status");

  display.setCursor(0, 14);
  display.print("Main btn: ");
  display.print(mainButton.isDown() ? "PRESSED" : "released");

  display.setCursor(0, 24);
  display.print("LED: ");
  display.print(recordingLedOn ? "ON" : "OFF");

  display.setCursor(0, 34);
  display.print("OLED I2C: SDA21 SCL19");

  display.setCursor(0, 44);
  display.print("GUI: B1=18 B2=23");

  drawFooter("B2 back");
  display.display();
}

void drawRecordingLedScreen() {
  drawHeader("Recording LED");

  display.setCursor(0, 18);
  display.setTextSize(2);
  display.print(recordingLedOn ? "LED ON" : "LED OFF");

  display.setTextSize(1);
  display.setCursor(0, 42);
  display.print("Use this as a test output.");

  drawFooter("B1 toggle  B2 back");
  display.display();
}

void drawInvertDisplayScreen() {
  drawHeader("Invert OLED");

  display.setCursor(0, 18);
  display.setTextSize(2);
  display.print(oledInverted ? "INVERTED" : "NORMAL");

  display.setTextSize(1);
  display.setCursor(0, 42);
  display.print("Useful for display testing.");

  drawFooter("B1 toggle  B2 back");
  display.display();
}

void drawButtonTestScreen() {
  drawHeader("Button test");

  display.setCursor(0, 14);
  display.print("GUI B1 GPIO18: ");
  display.print(guiButton1.isDown() ? "DOWN" : "up");

  display.setCursor(0, 26);
  display.print("GUI B2 GPIO23: ");
  display.print(guiButton2.isDown() ? "DOWN" : "up");

  display.setCursor(0, 38);
  display.print("Main  GPIO14: ");
  display.print(mainButton.isDown() ? "DOWN" : "up");

  drawFooter("B2 back");
  display.display();
}

void drawAboutScreen() {
  drawHeader("About");

  display.setCursor(0, 14);
  display.print("Two-button menu base");

  display.setCursor(0, 26);
  display.print("SSD1306 128x64 I2C");

  display.setCursor(0, 38);
  display.print("Address: 0x3C");

  display.setCursor(0, 48);
  display.print("Wire.begin(21,19)");

  drawFooter("B2 back");
  display.display();
}

void drawCurrentScreen() {
  switch (currentScreen) {
    case SCREEN_MENU:
      drawMenu();
      break;
    case SCREEN_STATUS:
      drawStatusScreen();
      break;
    case SCREEN_RECORDING_LED:
      drawRecordingLedScreen();
      break;
    case SCREEN_INVERT_DISPLAY:
      drawInvertDisplayScreen();
      break;
    case SCREEN_BUTTON_TEST:
      drawButtonTestScreen();
      break;
    case SCREEN_ABOUT:
      drawAboutScreen();
      break;
  }
}

void openSelectedMenuItem() {
  switch (selectedItem) {
    case 0:
      currentScreen = SCREEN_STATUS;
      break;
    case 1:
      currentScreen = SCREEN_RECORDING_LED;
      break;
    case 2:
      currentScreen = SCREEN_INVERT_DISPLAY;
      break;
    case 3:
      currentScreen = SCREEN_BUTTON_TEST;
      break;
    case 4:
      currentScreen = SCREEN_ABOUT;
      break;
  }
  redrawRequested = true;
}

void returnToMenu() {
  currentScreen = SCREEN_MENU;
  redrawRequested = true;
}

// ------------------------- Arduino setup/loop -------------------------
void setup() {
  Serial.begin(115200);
  delay(200);

  pinMode(PIN_RECORDING_LED, OUTPUT);
  digitalWrite(PIN_RECORDING_LED, LOW);

  guiButton1.begin();
  guiButton2.begin();
  mainButton.begin();

  // Important: your project uses non-default I2C SCL GPIO19.
  Wire.begin(OLED_SDA, OLED_SCL);

  if (!display.begin(SSD1306_SWITCHCAPVCC, SCREEN_ADDRESS)) {
    Serial.println("SSD1306 allocation/init failed. Check OLED wiring/address.");
    while (true) {
      digitalWrite(PIN_RECORDING_LED, !digitalRead(PIN_RECORDING_LED));
      delay(250);
    }
  }

  display.clearDisplay();
  display.display();
  drawMenu();
}

void loop() {
  guiButton1.update();
  guiButton2.update();
  mainButton.update();

  bool b1Pressed = guiButton1.wasPressed();
  bool b2Pressed = guiButton2.wasPressed();

  switch (currentScreen) {
    case SCREEN_MENU:
      if (b1Pressed) {
        selectedItem = (selectedItem + 1) % MENU_COUNT;
        redrawRequested = true;
      }
      if (b2Pressed) {
        openSelectedMenuItem();
      }
      break;

    case SCREEN_STATUS:
      if (b2Pressed) {
        returnToMenu();
      }
      // This screen includes live button state, so refresh it periodically.
      if (millis() - lastLiveRefreshMs > 150) {
        lastLiveRefreshMs = millis();
        redrawRequested = true;
      }
      break;

    case SCREEN_RECORDING_LED:
      if (b1Pressed) {
        recordingLedOn = !recordingLedOn;
        digitalWrite(PIN_RECORDING_LED, recordingLedOn ? HIGH : LOW);
        redrawRequested = true;
      }
      if (b2Pressed) {
        returnToMenu();
      }
      break;

    case SCREEN_INVERT_DISPLAY:
      if (b1Pressed) {
        oledInverted = !oledInverted;
        display.invertDisplay(oledInverted);
        redrawRequested = true;
      }
      if (b2Pressed) {
        returnToMenu();
      }
      break;

    case SCREEN_BUTTON_TEST:
      if (b2Pressed) {
        returnToMenu();
      }
      // Live refresh for the visible button states.
      if (millis() - lastLiveRefreshMs > 100) {
        lastLiveRefreshMs = millis();
        redrawRequested = true;
      }
      break;

    case SCREEN_ABOUT:
      if (b2Pressed) {
        returnToMenu();
      }
      break;
  }

  if (redrawRequested) {
    redrawRequested = false;
    drawCurrentScreen();
  }
}
