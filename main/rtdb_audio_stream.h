#pragma once

#include <Arduino.h>

struct RtdbPcmChunk {
  uint32_t seq = 0;
  uint32_t sampleRate = 0;
  uint16_t chunkMs = 0;
  size_t sampleCount = 0;
  String sessionId;
};

struct RtdbUploadChunk {
  const char* sessionId = nullptr;
  uint32_t seq = 0;
  const int16_t* samples = nullptr;
  size_t sampleCount = 0;
};

namespace RtdbAudioStream {

bool begin();
void loopMaintenance();

bool beginTransmission(uint8_t channel, const char* sessionId);
bool endTransmission(uint8_t channel, const char* sessionId, uint32_t lastSeq);

bool uploadPcmChunk(
    uint8_t channel,
    const char* sessionId,
    uint32_t seq,
    const int16_t* samples,
    size_t sampleCount);

bool uploadPcmChunkBatch(
    uint8_t channel,
    const RtdbUploadChunk* chunks,
    size_t chunkCount);

bool startListening(uint8_t channel);
void stopListening();
bool isListening();

// Non-blocking. Returns true only when a new decoded PCM chunk was received.
bool pollListening(
    int16_t* outSamples,
    size_t maxSamples,
    RtdbPcmChunk& outChunk);

}  // namespace RtdbAudioStream
