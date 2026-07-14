#pragma once

#include <Arduino.h>

namespace Communication {

enum class State : uint8_t {
  Listening,
  StartingSession,
  Transmitting,
  DrainingUploads,
  EndingSession,
  Reconnecting,
};

void begin();
State state();
bool isTransmitting();
bool permitsPlayback();
void transitionTo(State next, const char* reason);
const char* stateName(State value);

}  // namespace Communication
