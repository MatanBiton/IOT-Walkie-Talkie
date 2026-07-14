#pragma once

#include <Arduino.h>

namespace AvailabilityService {

struct UserStatus {
  uint8_t userNumber = 0;       // 1..5
  bool voipAvailable = false;   // RTDB heartbeat is fresh
  bool p2pAvailable = false;    // ESP-NOW heartbeat was heard recently
  uint32_t voipAgeSeconds = 0;  // UINT32_MAX when unknown
  uint32_t p2pAgeSeconds = 0;   // UINT32_MAX when unknown
};

// Starts NTP/ESP-NOW maintenance and asynchronous, low-priority RTDB
// availability scheduling. The RTDB request service must be started first.
bool begin();
bool isRunning();

// Copies the latest background-task state into caller-owned storage.
// Returns the number of copied users.
size_t copyUserStatuses(UserStatus* outStatuses, size_t maxStatuses);

}  // namespace AvailabilityService
