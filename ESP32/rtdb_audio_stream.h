#pragma once

#include <Arduino.h>

struct RtdbPcmChunk {
  uint32_t seq = 0;
  uint32_t sampleRate = 0;
  uint16_t chunkMs = 0;
  size_t sampleCount = 0;
  String sessionId;
  String deviceId;
};

namespace RtdbAudioStream {

bool begin();
void loopMaintenance();

bool startListening(uint8_t channel);
void stopListening();
bool isListening();

// Internal bridge used only by the single FirebaseClient worker. The event is
// copied into one pre-reserved parser slot; older unconsumed events may be
// replaced to keep RAM bounded.
void ingestFirebaseEvent(
    const char* eventName,
    const char* eventPath,
    const char* eventData);

// Non-blocking. Returns true only when a new decoded PCM chunk was received.
bool pollListening(
    int16_t* outSamples,
    size_t maxSamples,
    RtdbPcmChunk& outChunk);

}  // namespace RtdbAudioStream
