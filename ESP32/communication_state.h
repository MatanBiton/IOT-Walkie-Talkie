#pragma once

#include <Arduino.h>

namespace Communication {

enum class Transport : uint8_t {
  Voip,
  P2p,
};

enum class State : uint8_t {
  Listening,
  StartingSession,
  Transmitting,
  DrainingUploads,
  EndingSession,
  Reconnecting,
};

void begin();

Transport transport();
void setTransport(Transport next, const char* reason);
const char* transportName(Transport value);

// Runtime setting controlling only locally initiated fallback after failed
// VoIP uploads. Explicit Wi-Fi disable and a peer's coordinated switch request
// may still select P2P so the two devices remain interoperable.
bool automaticP2pDowngradeEnabled();
void setAutomaticP2pDowngradeEnabled(bool enabled);

State state();
bool isTransmitting();
bool permitsPlayback();
void transitionTo(State next, const char* reason);
const char* stateName(State value);

}  // namespace Communication
