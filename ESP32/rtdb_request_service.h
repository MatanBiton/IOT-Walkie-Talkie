#pragma once

#include <Arduino.h>

#include "app_config.h"

namespace RtdbRequestService {

enum class Outcome : uint8_t {
  Success,
  Failed,
  Busy,
  TimedOut,
  WifiDisconnected,
};

struct Result {
  Outcome outcome = Outcome::Failed;
  int httpCode = 0;

  bool succeeded() const { return outcome == Outcome::Success; }
};

struct PresenceRecord {
  uint8_t userNumber = 0;
  bool present = false;
  uint64_t lastSeenServerMs = 0;
  uint32_t bootId = 0;
  uint8_t state = 0;
  uint8_t logicalChannel = 0;
  uint8_t transport = 0;
};

struct PresenceSnapshot {
  PresenceRecord users[AvailabilityConfig::USER_COUNT];
};

bool begin();
bool isRunning();

bool scheduleSessionStart(uint8_t channel, const char* sessionId);

// FirebaseClient owns the single RTDB/SSE connection. These calls are routed
// through the RTDB worker so no FirebaseClient object is touched concurrently.
bool startAudioStream(uint8_t channel, uint32_t timeoutMs);
void stopAudioStream(uint32_t timeoutMs);
Result endSession(
    uint8_t channel,
    const char* sessionId,
    uint32_t lastSeq,
    uint32_t timeoutMs);

void setAudioPriorityActive(bool active);
void setRecordingActive(bool active);
bool audioPriorityActive();

bool acquireAudioBlock(
    uint8_t& outIndex,
    int16_t*& outSamples,
    bool& outDroppedOldest);
void releaseAudioBlock(uint8_t index);
bool submitAudioBlock(
    uint8_t index,
    uint8_t channel,
    const char* sessionId,
    uint32_t sequence,
    size_t sampleCount,
    uint32_t recordedAtMs);
uint32_t audioQueueDepth();
bool audioUploadsIdle();
bool audioUploadFailed();
// Monotonic count of failed audio upload requests since boot. Channel-level
// fallback code stores a baseline so failures accumulate across PTT presses.
uint32_t totalAudioUploadFailures();
// Monotonic PCM sample count discarded before successful RTDB delivery.
uint64_t totalDiscardedAudioSamples();
void clearAudioUploadFailure();
void requestAudioUploadAbort(const char* reason);
// Clears the temporary transport-abort/backoff state after Wi-Fi obtains a new
// IP. Queued PCM remains owned by the service and can continue draining.
void resumeAudioUploadsAfterReconnect();
uint32_t discardPendingAudio(const char* reason);

// Coalesced, low-priority presence operations. They run only while no stream,
// control request, queued audio, or upload owns the shared Firebase client.
// Requests expire instead of being requeued in a busy loop.
bool schedulePresenceLease(const char* jsonPayload, uint32_t validForMs);
bool schedulePresenceUsersRead(uint32_t validForMs);
void cancelPresenceUsersRead();
bool takePresenceLeaseResult(bool& outSuccess);
bool takePresenceUsersResult(bool& outSuccess, PresenceSnapshot& outSnapshot);

const char* outcomeName(Outcome outcome);

}  // namespace RtdbRequestService
