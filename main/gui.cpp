#include "gui.h"

#include <Adafruit_GFX.h>
#include <Adafruit_SSD1306.h>
#include <Wire.h>

#include "consts.h"

namespace {

Adafruit_SSD1306 display(
    DisplayConfig::SCREEN_WIDTH,
    DisplayConfig::SCREEN_HEIGHT,
    &Wire,
    DisplayConfig::OLED_RESET_PIN
);

class DebouncedButton {
public:
  void begin(int pin) {
    pin_ = pin;
    pinMode(pin_, INPUT_PULLUP);

    const bool pressed = isPressedRaw();
    lastRawPressed_ = pressed;
    stablePressed_ = pressed;
    lastChangeMs_ = millis();
  }

  bool pressedEdge() {
    const bool rawPressed = isPressedRaw();
    const unsigned long now = millis();

    if (rawPressed != lastRawPressed_) {
      lastRawPressed_ = rawPressed;
      lastChangeMs_ = now;
    }

    if ((now - lastChangeMs_) >= ButtonLogic::DEBOUNCE_MS &&
        rawPressed != stablePressed_) {
      stablePressed_ = rawPressed;

      // Trigger only once, on the transition into pressed state.
      return stablePressed_;
    }

    return false;
  }

private:
  bool isPressedRaw() const {
    return digitalRead(pin_) == ButtonLogic::PRESSED;
  }

  int pin_ = -1;
  bool lastRawPressed_ = false;
  bool stablePressed_ = false;
  unsigned long lastChangeMs_ = 0;
};

DebouncedButton leftButton;
DebouncedButton rightButton;

struct MenuItem {
  Gui::ScreenId screen;
  const char* label;
};

constexpr MenuItem MAIN_MENU_ITEMS[] = {
    {Gui::ScreenId::SelectChannels, "Select channels"},
    {Gui::ScreenId::ViewUsers,      "View users"},
    {Gui::ScreenId::ViewStatistics, "View statistics"},
    {Gui::ScreenId::ViewSettings,   "View settings"},
};

constexpr uint8_t MAIN_MENU_COUNT =
    sizeof(MAIN_MENU_ITEMS) / sizeof(MAIN_MENU_ITEMS[0]);

void drawHeader(const char* title) {
  display.setTextSize(1);
  display.setTextColor(SSD1306_WHITE);
  display.setCursor(0, 0);
  display.println(title);
  display.drawLine(0, 10, DisplayConfig::SCREEN_WIDTH - 1, 10, SSD1306_WHITE);
}

void drawFooter(const char* leftHint, const char* rightHint) {
  display.drawLine(0, 53, DisplayConfig::SCREEN_WIDTH - 1, 53, SSD1306_WHITE);
  display.setCursor(0, 56);
  display.print("L:");
  display.print(leftHint);

  display.setCursor(68, 56);
  display.print("R:");
  display.print(rightHint);
}

}  // namespace

namespace Gui {

AppGui appGui;

bool AppGui::begin() {
  Wire.begin(Pins::OLED_SDA, Pins::OLED_SCL);

  leftButton.begin(Pins::GUI_BUTTON_LEFT);
  rightButton.begin(Pins::GUI_BUTTON_RIGHT);

  if (!display.begin(SSD1306_SWITCHCAPVCC, DisplayConfig::OLED_I2C_ADDRESS)) {
    return false;
  }

  display.clearDisplay();
  display.display();

  showMainMenu();
  return true;
}

UpdateResult AppGui::update() {
  UpdateResult result;

  const bool leftPressed = leftButton.pressedEdge();
  const bool rightPressed = rightButton.pressedEdge();

  if (activeScreen_ == ScreenId::MainMenu) {
    if (leftPressed) {
      scrollNext();
    }

    if (rightPressed) {
      result = selectCurrent();
    }
  } else {
    // Placeholder behavior until the individual screens are implemented.
    // Later, this branch can delegate input to the active screen module.
    if (leftPressed) {
      showMainMenu();
    }
  }

  if (needsRedraw_) {
    render();
    needsRedraw_ = false;
  }

  return result;
}

void AppGui::showMainMenu() {
  activeScreen_ = ScreenId::MainMenu;
  needsRedraw_ = true;
}

void AppGui::forceRedraw() {
  needsRedraw_ = true;
}

ScreenId AppGui::activeScreen() const {
  return activeScreen_;
}

uint8_t AppGui::highlightedIndex() const {
  return highlightedIndex_;
}

void AppGui::scrollNext() {
  highlightedIndex_ = (highlightedIndex_ + 1) % MAIN_MENU_COUNT;
  needsRedraw_ = true;
}

UpdateResult AppGui::selectCurrent() {
  UpdateResult result;
  result.hasSelection = true;
  result.selectedScreen = MAIN_MENU_ITEMS[highlightedIndex_].screen;

  activeScreen_ = result.selectedScreen;
  needsRedraw_ = true;

  return result;
}

void AppGui::render() {
  display.clearDisplay();

  if (activeScreen_ == ScreenId::MainMenu) {
    renderMainMenu();
  } else {
    renderPlaceholder();
  }

  display.display();
}

void AppGui::renderMainMenu() {
  drawHeader("Walkie-Talkie");

  display.setTextSize(1);
  display.setTextColor(SSD1306_WHITE);

  for (uint8_t i = 0; i < MAIN_MENU_COUNT; ++i) {
    const int y = 16 + i * 9;

    display.setCursor(0, y);
    display.print(i == highlightedIndex_ ? ">" : " ");

    display.setCursor(10, y);
    display.print(MAIN_MENU_ITEMS[i].label);
  }

  drawFooter("Next", "Open");
}

void AppGui::renderPlaceholder() {
  drawHeader(screenTitle(activeScreen_));

  display.setTextSize(1);
  display.setTextColor(SSD1306_WHITE);
  display.setCursor(0, 20);
  display.println("Screen placeholder");
  display.setCursor(0, 32);
  display.println("Implementation will");
  display.setCursor(0, 42);
  display.println("be added later.");

  drawFooter("Back", "-");
}

const char* AppGui::screenTitle(ScreenId screen) const {
  switch (screen) {
    case ScreenId::MainMenu:
      return "Main menu";
    case ScreenId::SelectChannels:
      return "Select channels";
    case ScreenId::ViewUsers:
      return "View users";
    case ScreenId::ViewStatistics:
      return "View statistics";
    case ScreenId::ViewSettings:
      return "View settings";
    default:
      return "Unknown";
  }
}

}  // namespace Gui
