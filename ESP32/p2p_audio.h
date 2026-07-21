#pragma once

#include <Arduino.h>

#include "app_config.h"

namespace P2pAudio {

enum class PacketType : uint8_t {
  Start = 1,
  Audio = 2,
  End = 3,
};

struct ReceivedPacket {
  PacketType type = PacketType::Audio;
  uint8_t logicalChannel = 0;
  uint8_t senderUser = 0;
  uint32_t streamId = 0;
  uint32_t sequence = 0;
  uint16_t sampleCount = 0;
  int16_t samples[P2pAudioConfig::SAMPLES_PER_PACKET] = {};
};

struct SwitchRequest {
  uint8_t logicalChannel = 0;
  uint8_t senderUser = 0;
  uint32_t failedUploads = 0;
};

bool begin();
bool isReady();
bool isAudioBusy();

// Announces a channel-wide transport change. START and AUDIO packets also act
// as implicit switch metadata in case all explicit announcements are lost.
bool announceSwitch(uint8_t logicalChannel, uint32_t failedUploads);

bool startStream(uint8_t logicalChannel);
bool sendAudio(const int16_t* samples, size_t sampleCount);
void endStream();

bool takeReceivedPacket(ReceivedPacket& outPacket, uint32_t timeoutMs = 0);
bool takeSwitchRequest(SwitchRequest& outRequest);
void clearReceiveQueue();

}  // namespace P2pAudio
