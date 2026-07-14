#pragma once

#include <Arduino.h>

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

bool begin();
bool isRunning();

bool scheduleSessionStart(uint8_t channel, const char* sessionId);
Result endSession(
    uint8_t channel,
    const char* sessionId,
    uint32_t lastSeq,
    uint32_t timeoutMs);

void setAudioPriorityActive(bool active);
void setRecordingActive(bool active);
bool audioPriorityActive();

bool acquireAudioBlock(uint8_t& outIndex, int16_t*& outSamples);
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
void clearAudioUploadFailure();
uint32_t discardPendingAudio(const char* reason);

bool scheduleAvailabilityHeartbeat(const char* jsonPayload);
bool scheduleAvailabilityUsersRead();
bool takeAvailabilityHeartbeatResult(bool& outSuccess);
bool takeAvailabilityUsersResult(bool& outSuccess, String& outJson);

const char* outcomeName(Outcome outcome);

}  // namespace RtdbRequestService
