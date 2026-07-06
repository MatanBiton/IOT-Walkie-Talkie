#pragma once

#include <Arduino.h>

/*
 * Display GUI module.
 *
 * Current behavior:
 *   - Left GUI button  : scroll through main menu options.
 *   - Right GUI button : select the highlighted option.
 *   - In a placeholder screen, left returns to the main menu.
 *
 * Later integration:
 *   - Keep AppGui as the navigation owner.
 *   - Replace placeholder screens with real screen modules.
 *   - Use UpdateResult.selectedScreen in main.cpp/main.ino to trigger
 *     channel/user/statistics/settings logic.
 */

namespace Gui {

enum class ScreenId : uint8_t {
  MainMenu = 0,
  SelectChannels,
  ViewUsers,
  ViewStatistics,
  ViewSettings
};

struct UpdateResult {
  bool hasSelection = false;
  ScreenId selectedScreen = ScreenId::MainMenu;
};

class AppGui {
public:
  bool begin();

  // Call once per loop().
  // Returns hasSelection=true only on a new menu selection event.
  UpdateResult update();

  void showMainMenu();
  void forceRedraw();

  ScreenId activeScreen() const;
  uint8_t highlightedIndex() const;

private:
  void render();
  void renderMainMenu();
  void renderPlaceholder();

  void scrollNext();
  UpdateResult selectCurrent();

  const char* screenTitle(ScreenId screen) const;

  ScreenId activeScreen_ = ScreenId::MainMenu;
  uint8_t highlightedIndex_ = 0;
  bool needsRedraw_ = true;
};

extern AppGui appGui;

}  // namespace Gui
