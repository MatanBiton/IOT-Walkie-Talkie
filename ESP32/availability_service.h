#pragma once

#include <Arduino.h>

namespace AvailabilityService {

enum class EvidenceState : uint8_t {
  Unknown,
  Available,
  Stale,
};

enum class RefreshState : uint8_t {
  Idle,
  Refreshing,
  Complete,
  Failed,
};

struct UserStatus {
  uint8_t userNumber = 0;       // 1..USER_COUNT
  EvidenceState voip = EvidenceState::Unknown;
  EvidenceState p2p = EvidenceState::Unknown;
  uint32_t voipAgeSeconds = 0xffffffffUL;
  uint32_t p2pAgeSeconds = 0xffffffffUL;
};

// Initializes fixed state and registers one handler with the shared ESP-NOW
// dispatcher. No task, periodic RTDB poll, or periodic ESP-NOW beacon is used.
bool begin();
bool isRunning();

// Called from loop(). Context changes are coalesced into an event-driven lease.
void poll(
    uint8_t logicalChannel,
    bool communicationBusy,
    uint8_t transportValue);

// Starts one RTDB lease+snapshot cycle and one ESP-NOW discovery scan.
bool requestRefresh();
void cancelRefresh();
RefreshState refreshState();
uint32_t lastSnapshotAgeSeconds();

// Optional phase-5 maintenance hook. The caller must pause SSE first. This
// method waits only for the already-existing RTDB worker; it creates no client.
bool maintenanceLeaseDue(uint8_t logicalChannel, bool communicationBusy);
bool performMaintenanceLeaseSync(uint32_t timeoutMs);

// Copies fixed state into caller-owned storage.
size_t copyUserStatuses(UserStatus* outStatuses, size_t maxStatuses);

}  // namespace AvailabilityService
