#pragma once

#include <Arduino.h>

namespace EspNowTransport {

enum class SendClass : uint8_t {
  Audio,
  Availability,
};

using ReceiveHandler = void (*)(
    const uint8_t* sourceMac,
    const uint8_t* data,
    size_t length);

// Initializes one shared ESP-NOW instance and one broadcast peer. Audio and
// availability register small receive handlers on this shared dispatcher so
// neither feature replaces the other's ESP-NOW callback.
bool begin();
bool isReady();
bool registerReceiveHandler(ReceiveHandler handler);

// Serializes esp_now_send(). Availability sends are non-blocking and are
// rejected whenever P2P audio owns the transport. Audio/control sends may wait
// briefly for the shared send lock.
bool sendBroadcast(
    const void* data,
    size_t length,
    SendClass sendClass = SendClass::Audio);
void setAudioTrafficActive(bool active);
bool audioTrafficActive();

}  // namespace EspNowTransport
