#pragma once

#include <Arduino.h>

/*
 * Display GUI module.
 *
 * Controls outside an active channel:
 * - Left / Right GUI buttons: previous / next (or decrease / increase).
 * - PTT single click: select the highlighted option when one exists.
 * - PTT double click: go back.
 *
 * Controls inside an active channel:
 * - PTT keeps its original immediate push-to-talk behavior.
 * - Left GUI button leaves the channel and returns to channel selection.
 * - Right GUI button is reserved.
 *
 * The statistics screen blocks both outgoing and incoming audio.
 *
 * Settings behavior:
 * - Page 1: speaker volume, microphone gain, and microphone noise gate.
 * - Page 2: infrastructure Wi-Fi and automatic P2P downgrade toggles.
 *
 * Current channel behavior:
 * - Shows 10 channels.
 * - Shows static VOIP/P2P usage counters near each channel.
 * - Selecting a channel joins it and opens the channel screen.
 * - Leaving the channel screen reports a channel-leave event so networking can
 *   stop the SSE listener and release its memory.
 */

namespace Gui {

constexpr uint8_t CHANNEL_COUNT = 10;
constexpr uint8_t USER_COUNT = 5;

enum class CommunicationMethod : uint8_t {
  Voip,
  P2p,
};

enum class AvailabilityIndicator : uint8_t {
  Unknown,
  Available,
  Stale,
};

enum class AvailabilityRefreshIndicator : uint8_t {
  Idle,
  Refreshing,
  Complete,
  Failed,
};

enum class ScreenId : uint8_t {
  MainMenu = 0,
  SelectChannels,
  ViewUsers,
  ViewStatistics,
  ViewSettings,
  EditSetting,
  ChannelJoinPreview
};

struct ChannelStats {
  uint8_t channelNumber;
  uint8_t voipUsers;
  uint8_t p2pUsers;
};

struct UserStatus {
  uint8_t userNumber;
  AvailabilityIndicator voip;
  AvailabilityIndicator p2p;
  uint32_t voipAgeSeconds;
  uint32_t p2pAgeSeconds;
};

struct UpdateResult {
  bool hasSelection = false;
  ScreenId selectedScreen = ScreenId::MainMenu;

  bool hasChannelSelection = false;
  uint8_t selectedChannel = 0;

  // True when the user exits the joined-channel screen. Channel 0 is reserved
  // by the application as the explicit "not joined" state.
  bool hasChannelLeave = false;

  // True when the PTT press was used as GUI Back navigation. The application
  // suppresses transmission until that physical press has been released.
  bool consumedPttNavigation = false;

  bool availabilityRefreshRequested = false;
  bool availabilityScreenClosed = false;
};

class AppGui {
 public:
  bool begin();

  // Call once per loop().
  UpdateResult update();

  void showMainMenu();
  void forceRedraw();

  ScreenId activeScreen() const;
  uint8_t highlightedIndex() const;
  uint8_t highlightedChannel() const;
  uint8_t selectedChannel() const;

  // The communication task uses this to prevent PTT transmission while a GUI
  // screen owns or disables the main button.
  bool consumesPttForNavigation() const;

  // True while the GUI intentionally suspends both sending and receiving audio.
  bool blocksCommunication() const;

  // Future Firebase/RTDB integration point.
  // For now the channel GUI uses static values initialized in gui.cpp.
  void setChannelStats(uint8_t channelNumber, uint8_t voipUsers, uint8_t p2pUsers);

  // Current channel transport, shown on the joined-channel screen.
  void setCommunicationMethod(CommunicationMethod method);

  // Cached user availability from event-driven RTDB/P2P refreshes.
  void setUserStatus(
      uint8_t userNumber,
      AvailabilityIndicator voip,
      AvailabilityIndicator p2p,
      uint32_t voipAgeSeconds,
      uint32_t p2pAgeSeconds);
  void setAvailabilityRefreshIndicator(
      AvailabilityRefreshIndicator indicator);

 private:
  void render();
  void renderMainMenu();
  void renderChannelList();
  void renderChannelJoinPreview();
  void renderViewUsers();
  void renderStatistics();
  void renderSettingsList();
  void renderSettingEditor();
  void renderPlaceholder();

  void scrollMainMenuItem(bool forward);
  void scrollChannel(bool forward);
  void scrollSetting(bool forward);
  void scrollStatisticsPage(bool forward);
  UpdateResult goBack();
  void enterCurrentSetting();
  void adjustCurrentSetting(bool increase);
  uint16_t currentSettingValue() const;
  const char* currentSettingLabel() const;

  UpdateResult selectCurrentMainMenuItem();
  UpdateResult selectCurrentChannel();

  void showChannelList();

  const char* screenTitle(ScreenId screen) const;

  ScreenId activeScreen_ = ScreenId::MainMenu;

  uint8_t highlightedIndex_ = 0;
  uint8_t highlightedChannelIndex_ = 0;
  uint8_t selectedChannelIndex_ = 0;
  uint8_t highlightedSettingIndex_ = 0;
  uint8_t statisticsPageIndex_ = 0;
  uint32_t statisticsLastRefreshMs_ = 0;

  bool needsRedraw_ = true;
};

extern AppGui appGui;

}  // namespace Gui