#pragma once

#include <Arduino.h>

/*
 * Display GUI module.
 *
 * Controls:
 * - Main menu:
 *   - Left GUI button  : scroll through menu options
 *   - Right GUI button : open highlighted screen
 *
 * - Channel screen:
 *   - Left GUI button  : scroll through channels
 *   - Right GUI button : select/join highlighted channel
 *
 * Current channel behavior:
 * - Shows 10 channels.
 * - Shows static VOIP/P2P usage counters near each channel.
 * - Selecting a channel only opens a temporary placeholder screen.
 */

namespace Gui {

constexpr uint8_t CHANNEL_COUNT = 10;
constexpr uint8_t USER_COUNT = 5;

enum class ScreenId : uint8_t {
  MainMenu = 0,
  SelectChannels,
  ViewUsers,
  ViewStatistics,
  ViewSettings,
  ChannelJoinPreview
};

struct ChannelStats {
  uint8_t channelNumber;
  uint8_t voipUsers;
  uint8_t p2pUsers;
};

struct UserStatus {
  uint8_t userNumber;
  bool voipAvailable;
  bool p2pAvailable;
  uint32_t voipAgeSeconds;
  uint32_t p2pAgeSeconds;
};

struct UpdateResult {
  bool hasSelection = false;
  ScreenId selectedScreen = ScreenId::MainMenu;

  bool hasChannelSelection = false;
  uint8_t selectedChannel = 0;
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

  // Future Firebase/RTDB integration point.
  // For now the channel GUI uses static values initialized in gui.cpp.
  void setChannelStats(uint8_t channelNumber, uint8_t voipUsers, uint8_t p2pUsers);

  // Live user availability. Updated from the background availability task.
  void setUserStatus(
      uint8_t userNumber,
      bool voipAvailable,
      bool p2pAvailable,
      uint32_t voipAgeSeconds,
      uint32_t p2pAgeSeconds);

 private:
  void render();
  void renderMainMenu();
  void renderChannelList();
  void renderChannelJoinPreview();
  void renderViewUsers();
  void renderPlaceholder();

  void scrollNextMainMenuItem();
  void scrollNextChannel();

  UpdateResult selectCurrentMainMenuItem();
  UpdateResult selectCurrentChannel();

  void showChannelList();

  const char* screenTitle(ScreenId screen) const;

  ScreenId activeScreen_ = ScreenId::MainMenu;

  uint8_t highlightedIndex_ = 0;
  uint8_t highlightedChannelIndex_ = 0;
  uint8_t selectedChannelIndex_ = 0;

  bool needsRedraw_ = true;
};

extern AppGui appGui;

}  // namespace Gui