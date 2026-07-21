#pragma once

#include <Arduino.h>

#include "communication_state.h"

namespace RuntimeStatistics {

constexpr uint8_t CHANNEL_COUNT = 10;

struct Snapshot {
  uint32_t sentSessions = 0;
  uint64_t talkSamples = 0;
  uint64_t heardSamples = 0;
  uint64_t lastTransmissionSamples = 0;

  uint32_t voipUploadFailures = 0;
  uint64_t voipDiscardedSamples = 0;
  uint64_t localDiscardedSamples = 0;

  // Current-boot P2P diagnostics used by the network diagnostics page.
  // These intentionally remain non-persistent.
  uint32_t p2pPacketsSentAttempted = 0;
  uint32_t p2pPacketsSendRejected = 0;
  uint32_t p2pPacketsReceived = 0;
  uint32_t p2pDiagnosticPacketsMissed = 0;

  // Cumulative reliability statistic shown on page 2. Unlike the diagnostic
  // counters above, this value is persisted.
  uint32_t p2pPacketsMissed = 0;
  uint32_t fallbackCount = 0;

  uint64_t voipTalkSamples = 0;
  uint64_t p2pTalkSamples = 0;
  uint64_t channelTalkSamples[CHANNEL_COUNT] = {};
  uint16_t heardUsersMask = 0;
};

// Initializes the counters and restores the persistent statistics from NVS.
// If NVS is unavailable, statistics continue to work in RAM for the boot.
void begin();

// Called from the Arduino loop. A dirty persistent snapshot is saved after the
// configured interval, or immediately after requestPersistenceSave(). NVS I/O
// is deliberately kept out of the audio/communication tasks.
void maintainPersistence();

// Requests a save on the next maintainPersistence() call. Channel-exit code
// uses this instead of writing NVS directly from the communication task.
void requestPersistenceSave();

// Records microphone PCM captured while a transport was active. The same
// samples contribute to total talk time, the VoIP/P2P split, and channel usage.
void recordCapturedSamples(
    Communication::Transport transport,
    uint8_t channel,
    size_t sampleCount);

// Records audio that never reached the outgoing transport. VoIP queue/upload
// losses maintained by RtdbRequestService are synchronized separately.
void recordLocalDiscardedSamples(size_t sampleCount);

// A sent session is counted only when at least one microphone sample was
// captured. The last-session duration is sample based, so upload drain time is
// not included.
void recordCompletedSession(uint64_t capturedSamples);

// Played audio is counted only after AudioIO reports successful speaker output.
void recordPlayedSamples(size_t sampleCount, uint8_t sourceUserNumber);

// Counts local P2P AUDIO packet submissions. A rejected packet never entered
// the ESP-NOW send queue; these current-boot counters are not persisted.
void recordP2pPacketSendResult(bool acceptedForSend);
void recordP2pPacketReceived();

// Sequence gaps update both the cumulative reliability statistic and the
// current-boot diagnostic loss counter.
void recordP2pPacketsMissed(uint32_t missingPacketCount);
void recordFallback();

// Mirrors monotonic, current-boot RTDB service counters into cumulative values
// by adding them to the NVS values restored during begin().
void setVoipNetworkCounters(
    uint32_t uploadFailures,
    uint64_t discardedSamples);

Snapshot snapshot();

}  // namespace RuntimeStatistics
