#include "consts.h"
#include "gui.h"

void setup() {
  Serial.begin(115200);
  delay(500);

  if (!Gui::appGui.begin()) {
    Serial.println("[ERROR] OLED display initialization failed");

    while (true) {
      delay(1000);
    }
  }

  Serial.println("[READY] GUI initialized");
}

void loop() {
  Gui::UpdateResult guiResult = Gui::appGui.update();

  if (guiResult.hasSelection) {
    switch (guiResult.selectedScreen) {
      case Gui::ScreenId::SelectChannels:
        Serial.println("[GUI] Opened: Select channels");
        break;

      case Gui::ScreenId::ViewUsers:
        Serial.println("[GUI] Selected: View users");
        // TODO: connect to users screen/controller.
        break;

      case Gui::ScreenId::ViewStatistics:
        Serial.println("[GUI] Selected: View statistics");
        // TODO: connect to statistics screen/controller.
        break;

      case Gui::ScreenId::ViewSettings:
        Serial.println("[GUI] Selected: View settings");
        // TODO: connect to settings screen/controller.
        break;

      case Gui::ScreenId::MainMenu:
      case Gui::ScreenId::ChannelJoinPreview:
      default:
        break;
    }
  }

  if (guiResult.hasChannelSelection) {
    Serial.print("[GUI] Selected channel ");
    Serial.print(guiResult.selectedChannel);
    Serial.println(" - join action is not implemented yet");
  }
}