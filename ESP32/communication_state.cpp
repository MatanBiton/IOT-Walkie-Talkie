#include "communication_state.h"

#include <freertos/FreeRTOS.h>

#include "app_config.h"

namespace {

portMUX_TYPE stateMux = portMUX_INITIALIZER_UNLOCKED;
Communication::State currentState = Communication::State::Listening;
Communication::Transport currentTransport = Communication::Transport::Voip;
bool automaticP2pDowngrade =
    RuntimeSettingsConfig::AUTOMATIC_P2P_DOWNGRADE_DEFAULT;

}  // namespace

namespace Communication {

const char* transportName(Transport value) {
  switch (value) {
    case Transport::Voip: return "VoIP";
    case Transport::P2p: return "P2P";
    default: return "Unknown";
  }
}

Transport transport() {
  portENTER_CRITICAL(&stateMux);
  const Transport snapshot = currentTransport;
  portEXIT_CRITICAL(&stateMux);
  return snapshot;
}

void setTransport(Transport next, const char* reason) {
  Transport previous;
  portENTER_CRITICAL(&stateMux);
  previous = currentTransport;
  currentTransport = next;
  portEXIT_CRITICAL(&stateMux);

  if (previous != next) {
    Serial.printf(
        "[TRANSPORT] old=%s new=%s reason=%s\n",
        transportName(previous),
        transportName(next),
        (reason == nullptr || reason[0] == '\0') ? "unspecified" : reason);
  }
}

bool automaticP2pDowngradeEnabled() {
  portENTER_CRITICAL(&stateMux);
  const bool snapshot = automaticP2pDowngrade;
  portEXIT_CRITICAL(&stateMux);
  return snapshot;
}

void setAutomaticP2pDowngradeEnabled(bool enabled) {
  bool previous;
  portENTER_CRITICAL(&stateMux);
  previous = automaticP2pDowngrade;
  automaticP2pDowngrade = enabled;
  portEXIT_CRITICAL(&stateMux);

  if (previous != enabled) {
    Serial.printf(
        "[SETTINGS] automaticP2pDowngrade=%s\n",
        enabled ? "enabled" : "disabled");
  }
}

const char* stateName(State value) {
  switch (value) {
    case State::Listening: return "Listening";
    case State::StartingSession: return "StartingSession";
    case State::Transmitting: return "Transmitting";
    case State::DrainingUploads: return "DrainingUploads";
    case State::EndingSession: return "EndingSession";
    case State::Reconnecting: return "Reconnecting";
    default: return "Unknown";
  }
}

void begin() {
  portENTER_CRITICAL(&stateMux);
  currentState = State::Listening;
  portEXIT_CRITICAL(&stateMux);
  Serial.println("[STATE] old=Boot new=Listening reason=initialization_complete");
}

State state() {
  portENTER_CRITICAL(&stateMux);
  const State snapshot = currentState;
  portEXIT_CRITICAL(&stateMux);
  return snapshot;
}

bool isTransmitting() {
  const State snapshot = state();
  return snapshot == State::StartingSession ||
         snapshot == State::Transmitting ||
         snapshot == State::DrainingUploads ||
         snapshot == State::EndingSession;
}

bool permitsPlayback() {
  return state() == State::Listening;
}

void transitionTo(State next, const char* reason) {
  State previous;
  portENTER_CRITICAL(&stateMux);
  previous = currentState;
  currentState = next;
  portEXIT_CRITICAL(&stateMux);

  if (previous != next) {
    Serial.printf(
        "[STATE] old=%s new=%s reason=%s\n",
        stateName(previous),
        stateName(next),
        (reason == nullptr || reason[0] == '\0') ? "unspecified" : reason);
  }
}

}  // namespace Communication
