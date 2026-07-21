#include "gui.h"

#include <Arduino.h>
#include <Wire.h>
#include <Adafruit_GFX.h>
#include <Adafruit_SSD1306.h>
#include <WiFi.h>

#include "app_config.h"
#include "audio_io.h"
#include "communication_state.h"
#include "consts.h"
#include "runtime_statistics.h"
#include "wifi_connection.h"

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
DebouncedButton pttButton;

constexpr uint32_t PTT_DOUBLE_CLICK_MS = 300;
bool pttSingleClickPending = false;
uint32_t pttFirstClickMs = 0;

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
constexpr uint8_t STATISTICS_PAGE_COUNT = 4;
constexpr uint32_t STATISTICS_REFRESH_MS = 500;

constexpr uint8_t SETTINGS_COUNT = 5;
constexpr uint8_t SETTINGS_AUDIO_PAGE_COUNT = 3;
constexpr uint8_t SETTINGS_PAGE_COUNT = 2;
constexpr uint8_t SETTING_SPEAKER_VOLUME = 0;
constexpr uint8_t SETTING_MICROPHONE_GAIN = 1;
constexpr uint8_t SETTING_NOISE_GATE = 2;
constexpr uint8_t SETTING_WIFI_ENABLED = 3;
constexpr uint8_t SETTING_AUTOMATIC_P2P_DOWNGRADE = 4;

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

Gui::CommunicationMethod communicationMethod = Gui::CommunicationMethod::Voip;
Gui::AvailabilityRefreshIndicator availabilityRefreshIndicator =
    Gui::AvailabilityRefreshIndicator::Idle;

Gui::UserStatus userStats[Gui::USER_COUNT] = {
  {1, Gui::AvailabilityIndicator::Unknown, Gui::AvailabilityIndicator::Unknown, 0xffffffffUL, 0xffffffffUL},
  {2, Gui::AvailabilityIndicator::Unknown, Gui::AvailabilityIndicator::Unknown, 0xffffffffUL, 0xffffffffUL},
  {3, Gui::AvailabilityIndicator::Unknown, Gui::AvailabilityIndicator::Unknown, 0xffffffffUL, 0xffffffffUL},
  {4, Gui::AvailabilityIndicator::Unknown, Gui::AvailabilityIndicator::Unknown, 0xffffffffUL, 0xffffffffUL},
  {5, Gui::AvailabilityIndicator::Unknown, Gui::AvailabilityIndicator::Unknown, 0xffffffffUL, 0xffffffffUL},
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

void drawFooter(const char* leftHint, const char* rightHint, const char* pttHint) {
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

  display.setCursor(37, 56);
  display.print("R:");
  display.print(rightHint);

  display.setCursor(74, 56);
  display.print("P:");
  display.print(pttHint);
}

void printTwoDigit(uint8_t value) {
  if (value < 10) {
    display.print('0');
  }
  display.print(value);
}

void printAvailability(Gui::AvailabilityIndicator indicator) {
  switch (indicator) {
    case Gui::AvailabilityIndicator::Available:
      display.print("ON");
      break;
    case Gui::AvailabilityIndicator::Stale:
      display.print("--");
      break;
    case Gui::AvailabilityIndicator::Unknown:
    default:
      display.print(" ?");
      break;
  }
}

void formatDurationFromSamples(
    uint64_t samples,
    bool includeTenths,
    char* output,
    size_t outputSize) {
  if (output == nullptr || outputSize == 0) {
    return;
  }

  const uint64_t totalTenths =
      (samples * 10ULL) / AudioConfig::SAMPLE_RATE;
  const uint64_t totalSeconds = totalTenths / 10ULL;
  const uint64_t hours = totalSeconds / 3600ULL;
  const uint64_t minutes = (totalSeconds / 60ULL) % 60ULL;
  const uint64_t seconds = totalSeconds % 60ULL;

  if (hours > 999ULL) {
    snprintf(output, outputSize, "999:59:59+");
  } else if (hours > 0ULL) {
    snprintf(
        output,
        outputSize,
        "%llu:%02llu:%02llu",
        static_cast<unsigned long long>(hours),
        static_cast<unsigned long long>(minutes),
        static_cast<unsigned long long>(seconds));
  } else if (includeTenths) {
    snprintf(
        output,
        outputSize,
        "%02llu:%02llu.%llu",
        static_cast<unsigned long long>(minutes),
        static_cast<unsigned long long>(seconds),
        static_cast<unsigned long long>(totalTenths % 10ULL));
  } else {
    snprintf(
        output,
        outputSize,
        "%02llu:%02llu",
        static_cast<unsigned long long>(minutes),
        static_cast<unsigned long long>(seconds));
  }
}

void printPercentageTenths(uint64_t numerator, uint64_t denominator) {
  if (denominator == 0) {
    display.print("--");
    return;
  }

  uint64_t tenths =
      (numerator * 1000ULL + denominator / 2ULL) / denominator;
  if (tenths > 1000ULL) {
    tenths = 1000ULL;
  }
  display.print(static_cast<unsigned long>(tenths / 10ULL));
  display.print('.');
  display.print(static_cast<unsigned long>(tenths % 10ULL));
  display.print('%');
}

uint8_t countSetBits(uint16_t value) {
  uint8_t count = 0;
  while (value != 0) {
    count += value & 1U;
    value >>= 1U;
  }
  return count;
}

uint8_t topChannel(const RuntimeStatistics::Snapshot& stats) {
  uint8_t result = 0;
  uint64_t highestSamples = 0;
  for (uint8_t index = 0;
       index < RuntimeStatistics::CHANNEL_COUNT;
       ++index) {
    if (stats.channelTalkSamples[index] > highestSamples) {
      highestSamples = stats.channelTalkSamples[index];
      result = index + 1;
    }
  }
  return result;
}

}  // namespace

namespace Gui {

AppGui appGui;

bool AppGui::begin() {
  Wire.begin(Pins::OLED_SDA, Pins::OLED_SCL);

  leftButton.begin(Pins::GUI_BUTTON_LEFT);
  rightButton.begin(Pins::GUI_BUTTON_RIGHT);
  pttButton.begin(Pins::MAIN_BUTTON);

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
  const bool pttPressed = pttButton.pressedEdge();
  const uint32_t nowMs = millis();

  bool pttSingleClick = false;
  bool pttDoubleClick = false;

  if (activeScreen_ == ScreenId::ChannelJoinPreview) {
    // Inside a channel the PTT button belongs exclusively to communication.
    pttSingleClickPending = false;
  } else {
    if (pttPressed) {
      if (pttSingleClickPending &&
          (nowMs - pttFirstClickMs) <= PTT_DOUBLE_CLICK_MS) {
        pttSingleClickPending = false;
        pttDoubleClick = true;
      } else {
        pttSingleClickPending = true;
        pttFirstClickMs = nowMs;
      }
    }

    if (pttSingleClickPending &&
        (nowMs - pttFirstClickMs) > PTT_DOUBLE_CLICK_MS) {
      pttSingleClickPending = false;
      pttSingleClick = true;
    }
  }

  if (pttDoubleClick) {
    result = goBack();
    result.consumedPttNavigation = true;
  } else {
    switch (activeScreen_) {
      case ScreenId::MainMenu:
        if (leftPressed) {
          scrollMainMenuItem(false);
        }
        if (rightPressed) {
          scrollMainMenuItem(true);
        }
        if (pttSingleClick) {
          result = selectCurrentMainMenuItem();
          result.consumedPttNavigation = true;
        }
        break;

      case ScreenId::SelectChannels:
        if (leftPressed) {
          scrollChannel(false);
        }
        if (rightPressed) {
          scrollChannel(true);
        }
        if (pttSingleClick) {
          result = selectCurrentChannel();
          result.consumedPttNavigation = true;
        }
        break;

      case ScreenId::ChannelJoinPreview:
        // PTT remains immediate push-to-talk. Left is the dedicated Back key.
        if (leftPressed) {
          showChannelList();
          result.hasChannelLeave = true;
        }
        // Right is intentionally reserved on this screen.
        break;

      case ScreenId::ViewUsers:
        if (pttSingleClick) {
          result.availabilityRefreshRequested = true;
          result.consumedPttNavigation = true;
          needsRedraw_ = true;
        }
        break;

      case ScreenId::ViewSettings:
        if (leftPressed) {
          scrollSetting(false);
        }
        if (rightPressed) {
          scrollSetting(true);
        }
        if (pttSingleClick) {
          enterCurrentSetting();
          result.consumedPttNavigation = true;
        }
        break;

      case ScreenId::EditSetting:
        if (leftPressed) {
          adjustCurrentSetting(false);
        }
        if (rightPressed) {
          adjustCurrentSetting(true);
        }
        break;

      case ScreenId::ViewStatistics:
        if (leftPressed) {
          scrollStatisticsPage(false);
        }
        if (rightPressed) {
          scrollStatisticsPage(true);
        }
        if ((millis() - statisticsLastRefreshMs_) >= STATISTICS_REFRESH_MS) {
          needsRedraw_ = true;
        }
        break;

      default:
        break;
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

bool AppGui::consumesPttForNavigation() const {
  return activeScreen_ != ScreenId::ChannelJoinPreview;
}

bool AppGui::blocksCommunication() const {
  return activeScreen_ == ScreenId::ViewStatistics;
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

void AppGui::setCommunicationMethod(CommunicationMethod method) {
  if (communicationMethod == method) {
    return;
  }
  communicationMethod = method;
  if (activeScreen_ == ScreenId::ChannelJoinPreview) {
    needsRedraw_ = true;
  }
}

void AppGui::setUserStatus(
    uint8_t userNumber,
    AvailabilityIndicator voip,
    AvailabilityIndicator p2p,
    uint32_t voipAgeSeconds,
    uint32_t p2pAgeSeconds) {
  if (userNumber < 1 || userNumber > USER_COUNT) {
    return;
  }

  const uint8_t index = userNumber - 1;
  Gui::UserStatus& status = userStats[index];

  const bool changed =
      status.voip != voip ||
      status.p2p != p2p ||
      status.voipAgeSeconds != voipAgeSeconds ||
      status.p2pAgeSeconds != p2pAgeSeconds;

  status.userNumber = userNumber;
  status.voip = voip;
  status.p2p = p2p;
  status.voipAgeSeconds = voipAgeSeconds;
  status.p2pAgeSeconds = p2pAgeSeconds;

  if (changed && activeScreen_ == ScreenId::ViewUsers) {
    needsRedraw_ = true;
  }
}

void AppGui::setAvailabilityRefreshIndicator(
    AvailabilityRefreshIndicator indicator) {
  if (availabilityRefreshIndicator == indicator) {
    return;
  }
  availabilityRefreshIndicator = indicator;
  if (activeScreen_ == ScreenId::ViewUsers) {
    needsRedraw_ = true;
  }
}

void AppGui::scrollMainMenuItem(bool forward) {
  highlightedIndex_ = forward
      ? static_cast<uint8_t>((highlightedIndex_ + 1) % MAIN_MENU_COUNT)
      : static_cast<uint8_t>((highlightedIndex_ + MAIN_MENU_COUNT - 1) % MAIN_MENU_COUNT);
  needsRedraw_ = true;
}

void AppGui::scrollChannel(bool forward) {
  highlightedChannelIndex_ = forward
      ? static_cast<uint8_t>((highlightedChannelIndex_ + 1) % CHANNEL_COUNT)
      : static_cast<uint8_t>((highlightedChannelIndex_ + CHANNEL_COUNT - 1) % CHANNEL_COUNT);
  needsRedraw_ = true;
}

void AppGui::scrollSetting(bool forward) {
  highlightedSettingIndex_ = forward
      ? static_cast<uint8_t>((highlightedSettingIndex_ + 1) % SETTINGS_COUNT)
      : static_cast<uint8_t>((highlightedSettingIndex_ + SETTINGS_COUNT - 1) % SETTINGS_COUNT);
  needsRedraw_ = true;
}

void AppGui::scrollStatisticsPage(bool forward) {
  statisticsPageIndex_ = forward
      ? static_cast<uint8_t>((statisticsPageIndex_ + 1) % STATISTICS_PAGE_COUNT)
      : static_cast<uint8_t>((statisticsPageIndex_ + STATISTICS_PAGE_COUNT - 1) % STATISTICS_PAGE_COUNT);
  needsRedraw_ = true;
}

UpdateResult AppGui::goBack() {
  UpdateResult result;

  switch (activeScreen_) {
    case ScreenId::MainMenu:
      break;

    case ScreenId::SelectChannels:
    case ScreenId::ViewStatistics:
    case ScreenId::ViewSettings:
      showMainMenu();
      break;

    case ScreenId::ViewUsers:
      showMainMenu();
      result.availabilityScreenClosed = true;
      break;

    case ScreenId::EditSetting:
      activeScreen_ = ScreenId::ViewSettings;
      needsRedraw_ = true;
      break;

    case ScreenId::ChannelJoinPreview:
      // This case is unreachable because PTT gestures are disabled in-channel.
      break;

    default:
      showMainMenu();
      break;
  }

  return result;
}

void AppGui::enterCurrentSetting() {
  activeScreen_ = ScreenId::EditSetting;
  needsRedraw_ = true;
}

void AppGui::adjustCurrentSetting(bool increase) {
  if (highlightedSettingIndex_ == SETTING_WIFI_ENABLED) {
    WifiConnection::setEnabled(increase);
    needsRedraw_ = true;
    return;
  }

  if (highlightedSettingIndex_ == SETTING_AUTOMATIC_P2P_DOWNGRADE) {
    Communication::setAutomaticP2pDowngradeEnabled(increase);
    needsRedraw_ = true;
    return;
  }

  const uint16_t step = AudioSettingsConfig::ADJUST_STEP_PERCENT;
  uint16_t value = currentSettingValue();
  uint16_t minimum = 0;
  uint16_t maximum = 100;

  switch (highlightedSettingIndex_) {
    case SETTING_SPEAKER_VOLUME:
      minimum = AudioSettingsConfig::SPEAKER_VOLUME_MIN_PERCENT;
      maximum = AudioSettingsConfig::SPEAKER_VOLUME_MAX_PERCENT;
      break;

    case SETTING_MICROPHONE_GAIN:
      minimum = AudioSettingsConfig::MICROPHONE_GAIN_MIN_PERCENT;
      maximum = AudioSettingsConfig::MICROPHONE_GAIN_MAX_PERCENT;
      break;

    case SETTING_NOISE_GATE:
    default:
      minimum = AudioSettingsConfig::MICROPHONE_NOISE_GATE_MIN_PERCENT;
      maximum = AudioSettingsConfig::MICROPHONE_NOISE_GATE_MAX_PERCENT;
      break;
  }

  if (increase) {
    const uint32_t increased = static_cast<uint32_t>(value) + step;
    value = increased > maximum ? maximum : static_cast<uint16_t>(increased);
  } else {
    value = value <= minimum || (value - minimum) <= step
                ? minimum
                : static_cast<uint16_t>(value - step);
  }

  switch (highlightedSettingIndex_) {
    case SETTING_SPEAKER_VOLUME:
      AudioIO::setSpeakerVolumePercent(value);
      Serial.printf("[SETTINGS] speakerVolume=%u%%\n", value);
      break;

    case SETTING_MICROPHONE_GAIN:
      AudioIO::setMicrophoneGainPercent(value);
      Serial.printf("[SETTINGS] microphoneGain=%u%%\n", value);
      break;

    case SETTING_NOISE_GATE:
    default:
      AudioIO::setMicrophoneNoiseGatePercent(value);
      Serial.printf("[SETTINGS] microphoneNoiseGate=%u%%\n", value);
      break;
  }

  needsRedraw_ = true;
}

uint16_t AppGui::currentSettingValue() const {
  switch (highlightedSettingIndex_) {
    case SETTING_SPEAKER_VOLUME:
      return AudioIO::speakerVolumePercent();

    case SETTING_MICROPHONE_GAIN:
      return AudioIO::microphoneGainPercent();

    case SETTING_NOISE_GATE:
      return AudioIO::microphoneNoiseGatePercent();

    case SETTING_WIFI_ENABLED:
      return WifiConnection::isEnabled() ? 1 : 0;

    case SETTING_AUTOMATIC_P2P_DOWNGRADE:
    default:
      return Communication::automaticP2pDowngradeEnabled() ? 1 : 0;
  }
}

const char* AppGui::currentSettingLabel() const {
  switch (highlightedSettingIndex_) {
    case SETTING_SPEAKER_VOLUME:
      return "Speaker";

    case SETTING_MICROPHONE_GAIN:
      return "Mic gain";

    case SETTING_NOISE_GATE:
      return "Noise gate";

    case SETTING_WIFI_ENABLED:
      return "WiFi";

    case SETTING_AUTOMATIC_P2P_DOWNGRADE:
    default:
      return "Auto P2P downgrade";
  }
}

UpdateResult AppGui::selectCurrentMainMenuItem() {
  UpdateResult result;

  result.hasSelection = true;
  result.selectedScreen = MAIN_MENU_ITEMS[highlightedIndex_].screen;

  activeScreen_ = result.selectedScreen;
  needsRedraw_ = true;
  if (activeScreen_ == ScreenId::ViewStatistics) {
    statisticsPageIndex_ = 0;
    statisticsLastRefreshMs_ = 0;
  }
  if (activeScreen_ == ScreenId::ViewUsers) {
    result.availabilityRefreshRequested = true;
  }

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
      renderViewUsers();
      break;

    case ScreenId::ViewSettings:
      renderSettingsList();
      break;

    case ScreenId::EditSetting:
      renderSettingEditor();
      break;

    case ScreenId::ViewStatistics:
      renderStatistics();
      break;

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

  drawFooter("<", ">", "OK/2B");
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

  drawFooter("<", ">", "OK/2B");
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
  display.print("Method: ");
  display.print(
      communicationMethod == Gui::CommunicationMethod::P2p
          ? "P2P"
          : "VoIP");

  display.setCursor(0, 40);
  display.print("VOIP:");
  display.print(stats.voipUsers);
  display.print("  P2P:");
  display.print(stats.p2pUsers);

  drawFooter("Back", "-", "Talk");
}

void AppGui::renderViewUsers() {
  drawHeader("View users");

  display.setTextSize(1);
  display.setTextColor(SSD1306_WHITE);
  display.setCursor(104, 0);
  switch (availabilityRefreshIndicator) {
    case AvailabilityRefreshIndicator::Refreshing:
      display.print("...");
      break;
    case AvailabilityRefreshIndicator::Failed:
      display.print("!");
      break;
    case AvailabilityRefreshIndicator::Complete:
      display.print("OK");
      break;
    case AvailabilityRefreshIndicator::Idle:
    default:
      break;
  }

  display.setTextSize(1);
  display.setTextColor(SSD1306_WHITE);

  for (uint8_t i = 0; i < USER_COUNT; ++i) {
    const Gui::UserStatus& status = userStats[i];
    const int y = 14 + i * 8;

    display.setCursor(0, y);
    display.print("U");
    printTwoDigit(status.userNumber);

    display.setCursor(26, y);
    display.print("V:");
    printAvailability(status.voip);

    display.setCursor(67, y);
    display.print("P:");
    printAvailability(status.p2p);
  }

  drawFooter("-", "-", "Ref/2B");
}

void AppGui::renderStatistics() {
  statisticsLastRefreshMs_ = millis();
  const RuntimeStatistics::Snapshot stats = RuntimeStatistics::snapshot();

  char title[20];
  snprintf(
      title,
      sizeof(title),
      "Statistics %u/%u",
      static_cast<unsigned int>(statisticsPageIndex_ + 1),
      static_cast<unsigned int>(STATISTICS_PAGE_COUNT));
  drawHeader(title);

  display.setTextSize(1);
  display.setTextColor(SSD1306_WHITE);

  if (statisticsPageIndex_ == 0) {
    char talkDuration[16];
    char heardDuration[16];
    char lastDuration[16];
    formatDurationFromSamples(
        stats.talkSamples, false, talkDuration, sizeof(talkDuration));
    formatDurationFromSamples(
        stats.heardSamples, false, heardDuration, sizeof(heardDuration));
    formatDurationFromSamples(
        stats.lastTransmissionSamples,
        false,
        lastDuration,
        sizeof(lastDuration));

    display.setCursor(0, 14);
    display.print("Sent: ");
    display.print(stats.sentSessions);
    display.setCursor(0, 24);
    display.print("Talk: ");
    display.print(talkDuration);
    display.setCursor(0, 34);
    display.print("Heard: ");
    display.print(heardDuration);
    display.setCursor(0, 44);
    display.print("Last: ");
    display.print(lastDuration);
  } else if (statisticsPageIndex_ == 1) {
    char lostDuration[16];
    formatDurationFromSamples(
        stats.voipDiscardedSamples + stats.localDiscardedSamples,
        true,
        lostDuration,
        sizeof(lostDuration));

    display.setCursor(0, 14);
    display.print("VoIP fails: ");
    display.print(stats.voipUploadFailures);
    display.setCursor(0, 24);
    display.print("Audio lost: ");
    display.print(lostDuration);
    display.setCursor(0, 34);
    display.print("P2P missed: ");
    display.print(stats.p2pPacketsMissed);
    display.setCursor(0, 44);
    display.print("Fallbacks: ");
    display.print(stats.fallbackCount);
  } else if (statisticsPageIndex_ == 2) {
    const uint64_t transportSamples =
        stats.voipTalkSamples + stats.p2pTalkSamples;
    uint8_t voipPercent = 0;
    uint8_t p2pPercent = 0;
    if (transportSamples > 0) {
      voipPercent = static_cast<uint8_t>(
          (stats.voipTalkSamples * 100ULL + transportSamples / 2ULL) /
          transportSamples);
      if (voipPercent > 100) {
        voipPercent = 100;
      }
      p2pPercent = 100 - voipPercent;
    }

    const uint8_t mostUsedChannel = topChannel(stats);
    display.setCursor(0, 14);
    display.print("VoIP time: ");
    display.print(voipPercent);
    display.print('%');
    display.setCursor(0, 24);
    display.print("P2P time: ");
    display.print(p2pPercent);
    display.print('%');
    display.setCursor(0, 34);
    display.print("Top channel: ");
    if (mostUsedChannel == 0) {
      display.print("--");
    } else {
      printTwoDigit(mostUsedChannel);
    }
    display.setCursor(0, 44);
    display.print("Users heard: ");
    display.print(countSetBits(stats.heardUsersMask));
  } else {
    const bool wifiConnected = WifiConnection::isConnected();

    display.setCursor(0, 14);
    display.print("SSID: ");
    char ssid[16] = {0};
    snprintf(ssid, sizeof(ssid), "%.15s", WifiConfig::SSID);
    display.print(ssid);

    display.setCursor(0, 24);
    display.print("RSSI: ");
    if (wifiConnected) {
      display.print(WiFi.RSSI());
      display.print(" dBm");
    } else {
      display.print("--");
    }

    display.setCursor(0, 34);
    display.print("P2P TX reject: ");
    printPercentageTenths(
        stats.p2pPacketsSendRejected,
        stats.p2pPacketsSentAttempted);

    display.setCursor(0, 44);
    display.print("P2P RX loss: ");
    const uint64_t p2pObservedPackets =
        static_cast<uint64_t>(stats.p2pPacketsReceived) +
        static_cast<uint64_t>(stats.p2pDiagnosticPacketsMissed);
    printPercentageTenths(
        stats.p2pDiagnosticPacketsMissed,
        p2pObservedPackets);
  }

  drawFooter("<", ">", "2xBack");
}

void AppGui::renderSettingsList() {
  const uint8_t pageIndex =
      highlightedSettingIndex_ < SETTINGS_AUDIO_PAGE_COUNT ? 0 : 1;
  const uint8_t pageStart = pageIndex == 0 ? 0 : SETTINGS_AUDIO_PAGE_COUNT;
  const uint8_t pageEnd = pageIndex == 0 ? SETTINGS_AUDIO_PAGE_COUNT
                                         : SETTINGS_COUNT;

  char title[20];
  snprintf(
      title,
      sizeof(title),
      "Settings %u/%u",
      static_cast<unsigned int>(pageIndex + 1),
      static_cast<unsigned int>(SETTINGS_PAGE_COUNT));
  drawHeader(title);

  display.setTextSize(1);
  display.setTextColor(SSD1306_WHITE);

  uint8_t row = 0;
  for (uint8_t i = pageStart; i < pageEnd; ++i, ++row) {
    const int y = pageIndex == 0 ? 16 + row * 11 : 20 + row * 15;
    display.setCursor(0, y);
    display.print(i == highlightedSettingIndex_ ? ">" : " ");

    display.setCursor(10, y);
    switch (i) {
      case SETTING_SPEAKER_VOLUME:
        display.print("Speaker");
        display.setCursor(91, y);
        display.print(AudioIO::speakerVolumePercent());
        display.print('%');
        break;

      case SETTING_MICROPHONE_GAIN:
        display.print("Mic gain");
        display.setCursor(91, y);
        display.print(AudioIO::microphoneGainPercent());
        display.print('%');
        break;

      case SETTING_NOISE_GATE:
        display.print("Noise gate");
        display.setCursor(91, y);
        display.print(AudioIO::microphoneNoiseGatePercent());
        display.print('%');
        break;

      case SETTING_WIFI_ENABLED:
        display.print("WiFi");
        display.setCursor(91, y);
        display.print(WifiConnection::isEnabled() ? "ON" : "OFF");
        break;

      case SETTING_AUTOMATIC_P2P_DOWNGRADE:
      default:
        display.print("Auto P2P");
        display.setCursor(91, y);
        display.print(
            Communication::automaticP2pDowngradeEnabled() ? "ON" : "OFF");
        break;
    }
  }

  drawFooter("<", ">", "OK/2B");
}

void AppGui::renderSettingEditor() {
  drawHeader("Edit setting");

  const bool booleanSetting =
      highlightedSettingIndex_ == SETTING_WIFI_ENABLED ||
      highlightedSettingIndex_ == SETTING_AUTOMATIC_P2P_DOWNGRADE;

  display.setTextSize(1);
  display.setTextColor(SSD1306_WHITE);
  display.setCursor(0, 16);
  display.print(currentSettingLabel());

  display.setTextSize(2);
  display.setCursor(booleanSetting ? 42 : 28, 28);
  if (booleanSetting) {
    display.print(currentSettingValue() != 0 ? "ON" : "OFF");
  } else {
    display.print(currentSettingValue());
    display.print('%');
  }

  display.setTextSize(1);
  display.setCursor(0, 44);
  if (booleanSetting) {
    display.print("Left OFF / Right ON");
    drawFooter("OFF", "ON", "2xBack");
    return;
  }

  display.print("Step: ");
  display.print(AudioSettingsConfig::ADJUST_STEP_PERCENT);
  display.print("%  PTT: back");

  char decreaseHint[8];
  char increaseHint[8];
  snprintf(
      decreaseHint,
      sizeof(decreaseHint),
      "-%u",
      AudioSettingsConfig::ADJUST_STEP_PERCENT);
  snprintf(
      increaseHint,
      sizeof(increaseHint),
      "+%u",
      AudioSettingsConfig::ADJUST_STEP_PERCENT);
  drawFooter(decreaseHint, increaseHint, "2xBack");
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

  drawFooter("-", "-", "2xBack");
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

    case ScreenId::EditSetting:
      return "Edit setting";

    case ScreenId::ChannelJoinPreview:
      return "Channel selected";

    default:
      return "Unknown";
  }
}

}  // namespace Gui