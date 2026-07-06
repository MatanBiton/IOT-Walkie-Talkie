#include "gui.h"

#include <Arduino.h>
#include <Wire.h>
#include <Adafruit_GFX.h>
#include <Adafruit_SSD1306.h>

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
  {Gui::ScreenId::ViewUsers, "View users"},
  {Gui::ScreenId::ViewStatistics, "View statistics"},
  {Gui::ScreenId::ViewSettings, "View settings"},
};

constexpr uint8_t MAIN_MENU_COUNT =
  sizeof(MAIN_MENU_ITEMS) / sizeof(MAIN_MENU_ITEMS[0]);

constexpr uint8_t CHANNELS_VISIBLE_ON_SCREEN = 4;

constexpr int CHANNEL_LIST_FIRST_Y = 16;
constexpr int CHANNEL_LIST_ROW_HEIGHT = 9;

// Static values for GUI evaluation.
// Later, replace/update these through Gui::appGui.setChannelStats(...).
Gui::ChannelStats channelStats[Gui::CHANNEL_COUNT] = {
  {1,  2, 1},
  {2,  0, 3},
  {3,  4, 0},
  {4,  1, 2},
  {5,  5, 1},
  {6,  0, 0},
  {7,  3, 2},
  {8,  1, 0},
  {9,  2, 4},
  {10, 0, 1},
};

void drawHeader(const char* title) {
  display.setTextSize(1);
  display.setTextColor(SSD1306_WHITE);

  display.setCursor(0, 0);
  display.println(title);

  display.drawLine(
    0,
    10,
    DisplayConfig::SCREEN_WIDTH - 1,
    10,
    SSD1306_WHITE
  );
}

void drawFooter(const char* leftHint, const char* rightHint) {
  display.drawLine(
    0,
    53,
    DisplayConfig::SCREEN_WIDTH - 1,
    53,
    SSD1306_WHITE
  );

  display.setCursor(0, 56);
  display.print("L:");
  display.print(leftHint);

  display.setCursor(68, 56);
  display.print("R:");
  display.print(rightHint);
}

void printTwoDigit(uint8_t value) {
  if (value < 10) {
    display.print('0');
  }
  display.print(value);
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

  switch (activeScreen_) {
    case ScreenId::MainMenu:
      if (leftPressed) {
        scrollNextMainMenuItem();
      }

      if (rightPressed) {
        result = selectCurrentMainMenuItem();
      }
      break;

    case ScreenId::SelectChannels:
      if (leftPressed) {
        scrollNextChannel();
      }

      if (rightPressed) {
        result = selectCurrentChannel();
      }
      break;

    case ScreenId::ChannelJoinPreview:
      // Temporary screen behavior:
      // Left returns to channel list.
      // Right returns to main menu.
      if (leftPressed) {
        showChannelList();
      }

      if (rightPressed) {
        showMainMenu();
      }
      break;

    case ScreenId::ViewUsers:
    case ScreenId::ViewStatistics:
    case ScreenId::ViewSettings:
    default:
      // Placeholder behavior until the individual screens are implemented.
      if (leftPressed) {
        showMainMenu();
      }
      break;
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

void AppGui::showChannelList() {
  activeScreen_ = ScreenId::SelectChannels;
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

uint8_t AppGui::highlightedChannel() const {
  return channelStats[highlightedChannelIndex_].channelNumber;
}

uint8_t AppGui::selectedChannel() const {
  return channelStats[selectedChannelIndex_].channelNumber;
}

void AppGui::setChannelStats(
  uint8_t channelNumber,
  uint8_t voipUsers,
  uint8_t p2pUsers
) {
  if (channelNumber < 1 || channelNumber > CHANNEL_COUNT) {
    return;
  }

  const uint8_t index = channelNumber - 1;
  channelStats[index].voipUsers = voipUsers;
  channelStats[index].p2pUsers = p2pUsers;

  if (activeScreen_ == ScreenId::SelectChannels ||
      activeScreen_ == ScreenId::ChannelJoinPreview) {
    needsRedraw_ = true;
  }
}

void AppGui::scrollNextMainMenuItem() {
  highlightedIndex_ = (highlightedIndex_ + 1) % MAIN_MENU_COUNT;
  needsRedraw_ = true;
}

void AppGui::scrollNextChannel() {
  highlightedChannelIndex_ = (highlightedChannelIndex_ + 1) % CHANNEL_COUNT;
  needsRedraw_ = true;
}

UpdateResult AppGui::selectCurrentMainMenuItem() {
  UpdateResult result;

  result.hasSelection = true;
  result.selectedScreen = MAIN_MENU_ITEMS[highlightedIndex_].screen;

  activeScreen_ = result.selectedScreen;
  needsRedraw_ = true;

  return result;
}

UpdateResult AppGui::selectCurrentChannel() {
  UpdateResult result;

  selectedChannelIndex_ = highlightedChannelIndex_;

  result.hasChannelSelection = true;
  result.selectedChannel = channelStats[selectedChannelIndex_].channelNumber;

  activeScreen_ = ScreenId::ChannelJoinPreview;
  needsRedraw_ = true;

  return result;
}

void AppGui::render() {
  display.clearDisplay();

  switch (activeScreen_) {
    case ScreenId::MainMenu:
      renderMainMenu();
      break;

    case ScreenId::SelectChannels:
      renderChannelList();
      break;

    case ScreenId::ChannelJoinPreview:
      renderChannelJoinPreview();
      break;

    case ScreenId::ViewUsers:
    case ScreenId::ViewStatistics:
    case ScreenId::ViewSettings:
    default:
      renderPlaceholder();
      break;
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

void AppGui::renderChannelList() {
  drawHeader("Select channel");

  display.setTextSize(1);
  display.setTextColor(SSD1306_WHITE);

  const uint8_t pageStart =
    (highlightedChannelIndex_ / CHANNELS_VISIBLE_ON_SCREEN) *
    CHANNELS_VISIBLE_ON_SCREEN;

  for (uint8_t row = 0; row < CHANNELS_VISIBLE_ON_SCREEN; ++row) {
    const uint8_t index = pageStart + row;

    if (index >= CHANNEL_COUNT) {
      break;
    }

    const Gui::ChannelStats& stats = channelStats[index];
    const int y = CHANNEL_LIST_FIRST_Y + row * CHANNEL_LIST_ROW_HEIGHT;

    display.setCursor(0, y);
    display.print(index == highlightedChannelIndex_ ? ">" : " ");

    display.setCursor(10, y);
    display.print("CH");
    printTwoDigit(stats.channelNumber);

    display.print(" V:");
    display.print(stats.voipUsers);

    display.print(" P:");
    display.print(stats.p2pUsers);
  }

  drawFooter("Next", "Join");
}

void AppGui::renderChannelJoinPreview() {
  const Gui::ChannelStats& stats = channelStats[selectedChannelIndex_];

  drawHeader("Channel selected");

  display.setTextSize(1);
  display.setTextColor(SSD1306_WHITE);

  display.setCursor(0, 16);
  display.print("Selected: CH");
  printTwoDigit(stats.channelNumber);

  display.setCursor(0, 28);
  display.print("Join action: TODO");

  display.setCursor(0, 40);
  display.print("VOIP:");
  display.print(stats.voipUsers);
  display.print("  P2P:");
  display.print(stats.p2pUsers);

  drawFooter("Back", "Menu");
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

    case ScreenId::ChannelJoinPreview:
      return "Channel selected";

    default:
      return "Unknown";
  }
}

}  // namespace Gui