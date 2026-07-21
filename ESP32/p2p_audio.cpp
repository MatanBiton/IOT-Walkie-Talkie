#include "p2p_audio.h"

#include <esp_system.h>
#include <freertos/FreeRTOS.h>
#include <freertos/queue.h>
#include <freertos/task.h>
#include <string.h>

#include "esp_now_transport.h"

namespace {

constexpr uint16_t PACKET_MAGIC = 0xB17B;
constexpr uint8_t PACKET_VERSION = 1;
constexpr uint8_t WIRE_SWITCH = 1;
constexpr uint8_t WIRE_START = 2;
constexpr uint8_t WIRE_AUDIO = 3;
constexpr uint8_t WIRE_END = 4;

struct WirePacket {
  uint16_t magic;
  uint8_t version;
  uint8_t type;
  uint8_t logicalChannel;
  uint8_t senderUser;
  uint32_t streamId;
  uint32_t sequence;
  uint16_t sampleCount;
  uint32_t failedUploads;
  int16_t samples[P2pAudioConfig::SAMPLES_PER_PACKET];
} __attribute__((packed));

static_assert(
    sizeof(WirePacket) <= 250,
    "P2P audio packet exceeds the classic ESP-NOW payload limit");

QueueHandle_t receiveQueue = nullptr;
portMUX_TYPE p2pMux = portMUX_INITIALIZER_UNLOCKED;
bool ready = false;
bool switchPending = false;
P2pAudio::SwitchRequest pendingSwitch;
uint8_t lastSwitchChannel = 0;
uint32_t lastSwitchStreamId = 0;
uint8_t selfUser = 0;
uint8_t txChannel = 0;
uint32_t txStreamId = 0;
uint32_t txSequence = 0;
bool txActive = false;
uint32_t lastRxAudioMs = 0;

uint8_t userNumberFromId(const char* id) {
  if (id == nullptr || id[0] < '0' || id[0] > '9' ||
      id[1] < '0' || id[1] > '9') {
    return 0;
  }
  return static_cast<uint8_t>((id[0] - '0') * 10 + (id[1] - '0'));
}

bool validChannel(uint8_t channel) {
  return channel >= 1 && channel <= 10;
}

bool sendPacket(const WirePacket& packet) {
  return EspNowTransport::sendBroadcast(
      &packet,
      sizeof(packet),
      EspNowTransport::SendClass::Audio);
}

void setPendingSwitch(const WirePacket& packet) {
  portENTER_CRITICAL(&p2pMux);
  const bool duplicate =
      lastSwitchChannel == packet.logicalChannel &&
      lastSwitchStreamId == packet.streamId;
  if (!duplicate) {
    lastSwitchChannel = packet.logicalChannel;
    lastSwitchStreamId = packet.streamId;
    pendingSwitch.logicalChannel = packet.logicalChannel;
    pendingSwitch.senderUser = packet.senderUser;
    pendingSwitch.failedUploads = packet.failedUploads;
    switchPending = true;
  }
  portEXIT_CRITICAL(&p2pMux);
}

void queuePacket(const WirePacket& packet) {
  if (receiveQueue == nullptr) {
    return;
  }
  if (xQueueSend(receiveQueue, &packet, 0) == pdTRUE) {
    return;
  }

  // Keep latency bounded: discard one old packet and retain the newest packet.
  WirePacket discarded = {};
  xQueueReceive(receiveQueue, &discarded, 0);
  xQueueSend(receiveQueue, &packet, 0);
}

void onEspNowPacket(
    const uint8_t*,
    const uint8_t* data,
    size_t length) {
  if (data == nullptr || length != sizeof(WirePacket)) {
    return;
  }

  WirePacket packet = {};
  memcpy(&packet, data, sizeof(packet));
  if (packet.magic != PACKET_MAGIC ||
      packet.version != PACKET_VERSION ||
      !validChannel(packet.logicalChannel) ||
      packet.senderUser == 0 ||
      packet.senderUser == selfUser ||
      packet.sampleCount > P2pAudioConfig::SAMPLES_PER_PACKET) {
    return;
  }

  if (packet.type == WIRE_SWITCH) {
    setPendingSwitch(packet);
    return;
  }

  if (packet.type != WIRE_START &&
      packet.type != WIRE_AUDIO &&
      packet.type != WIRE_END) {
    return;
  }

  portENTER_CRITICAL(&p2pMux);
  lastRxAudioMs = millis();
  portEXIT_CRITICAL(&p2pMux);

  // START/AUDIO is enough to recover if the repeated SWITCH packet was missed.
  if (packet.type == WIRE_START || packet.type == WIRE_AUDIO) {
    setPendingSwitch(packet);
  }
  queuePacket(packet);
}

WirePacket makeControlPacket(uint8_t type, uint8_t channel) {
  WirePacket packet = {};
  packet.magic = PACKET_MAGIC;
  packet.version = PACKET_VERSION;
  packet.type = type;
  packet.logicalChannel = channel;
  packet.senderUser = selfUser;
  packet.streamId = txStreamId;
  packet.sequence = txSequence++;
  packet.sampleCount = 0;
  packet.failedUploads = 0;
  return packet;
}

}  // namespace

namespace P2pAudio {

bool begin() {
  portENTER_CRITICAL(&p2pMux);
  const bool alreadyReady = ready;
  portEXIT_CRITICAL(&p2pMux);
  if (alreadyReady) {
    return true;
  }

  selfUser = userNumberFromId(AppConfig::USER_ID);
  if (selfUser == 0) {
    Serial.println("[P2P] invalid USER_ID; expected two digits");
    return false;
  }

  receiveQueue = xQueueCreate(
      P2pAudioConfig::RX_QUEUE_LENGTH,
      sizeof(WirePacket));
  if (receiveQueue == nullptr) {
    Serial.println("[P2P] receive_queue_create_failed");
    return false;
  }

  if (!EspNowTransport::registerReceiveHandler(onEspNowPacket)) {
    Serial.println("[P2P] espnow_handler_register_failed");
    return false;
  }

  portENTER_CRITICAL(&p2pMux);
  ready = true;
  portEXIT_CRITICAL(&p2pMux);
  Serial.printf(
      "[P2P] ready user=%u packetSamples=%u packetBytes=%u rxDepth=%u\n",
      static_cast<unsigned int>(selfUser),
      static_cast<unsigned int>(P2pAudioConfig::SAMPLES_PER_PACKET),
      static_cast<unsigned int>(sizeof(WirePacket)),
      static_cast<unsigned int>(P2pAudioConfig::RX_QUEUE_LENGTH));
  return true;
}

bool isReady() {
  portENTER_CRITICAL(&p2pMux);
  const bool snapshot = ready;
  portEXIT_CRITICAL(&p2pMux);
  return snapshot;
}

bool isAudioBusy() {
  bool transmitting = false;
  uint32_t lastRx = 0;
  portENTER_CRITICAL(&p2pMux);
  transmitting = txActive;
  lastRx = lastRxAudioMs;
  portEXIT_CRITICAL(&p2pMux);
  const bool receivingRecently =
      lastRx != 0 &&
      (millis() - lastRx) < P2pAudioConfig::AVAILABILITY_GUARD_MS;
  return transmitting || receivingRecently ||
         EspNowTransport::audioTrafficActive();
}

bool announceSwitch(uint8_t logicalChannel, uint32_t failedUploads) {
  if (!isReady() || !validChannel(logicalChannel)) {
    return false;
  }

  WirePacket packet = {};
  packet.magic = PACKET_MAGIC;
  packet.version = PACKET_VERSION;
  packet.type = WIRE_SWITCH;
  packet.logicalChannel = logicalChannel;
  packet.senderUser = selfUser;
  packet.streamId = esp_random();
  packet.failedUploads = failedUploads;

  bool anySent = false;
  for (uint8_t i = 0; i < P2pAudioConfig::SWITCH_ANNOUNCE_REPEATS; ++i) {
    anySent = sendPacket(packet) || anySent;
    if (i + 1 < P2pAudioConfig::SWITCH_ANNOUNCE_REPEATS) {
      vTaskDelay(pdMS_TO_TICKS(P2pAudioConfig::SWITCH_ANNOUNCE_GAP_MS));
    }
  }

  Serial.printf(
      "[P2P] switch_announce channel=%u failures=%lu sent=%s repeats=%u\n",
      static_cast<unsigned int>(logicalChannel),
      static_cast<unsigned long>(failedUploads),
      anySent ? "true" : "false",
      static_cast<unsigned int>(P2pAudioConfig::SWITCH_ANNOUNCE_REPEATS));
  return anySent;
}

bool startStream(uint8_t logicalChannel) {
  if (!isReady() || !validChannel(logicalChannel)) {
    return false;
  }

  portENTER_CRITICAL(&p2pMux);
  txChannel = logicalChannel;
  txStreamId = esp_random();
  txSequence = 0;
  txActive = true;
  portEXIT_CRITICAL(&p2pMux);
  EspNowTransport::setAudioTrafficActive(true);
  const WirePacket packet = makeControlPacket(WIRE_START, txChannel);
  const bool sent = sendPacket(packet);
  Serial.printf(
      "[P2P] stream_start channel=%u stream=%lu sent=%s\n",
      static_cast<unsigned int>(txChannel),
      static_cast<unsigned long>(txStreamId),
      sent ? "true" : "false");
  return sent;
}

bool sendAudio(const int16_t* samples, size_t sampleCount) {
  if (!txActive || samples == nullptr || sampleCount == 0 ||
      sampleCount > P2pAudioConfig::SAMPLES_PER_PACKET) {
    return false;
  }

  WirePacket packet = makeControlPacket(WIRE_AUDIO, txChannel);
  packet.sampleCount = static_cast<uint16_t>(sampleCount);
  memcpy(packet.samples, samples, sampleCount * sizeof(int16_t));
  return sendPacket(packet);
}

void endStream() {
  if (!txActive) {
    return;
  }
  const WirePacket packet = makeControlPacket(WIRE_END, txChannel);
  const bool sent = sendPacket(packet);
  Serial.printf(
      "[P2P] stream_end channel=%u stream=%lu packets=%lu sent=%s\n",
      static_cast<unsigned int>(txChannel),
      static_cast<unsigned long>(txStreamId),
      static_cast<unsigned long>(txSequence),
      sent ? "true" : "false");
  portENTER_CRITICAL(&p2pMux);
  txActive = false;
  txChannel = 0;
  portEXIT_CRITICAL(&p2pMux);
  EspNowTransport::setAudioTrafficActive(false);
}

bool takeReceivedPacket(ReceivedPacket& outPacket, uint32_t timeoutMs) {
  if (receiveQueue == nullptr) {
    return false;
  }

  WirePacket packet = {};
  if (xQueueReceive(
          receiveQueue,
          &packet,
          pdMS_TO_TICKS(timeoutMs)) != pdTRUE) {
    return false;
  }

  outPacket.logicalChannel = packet.logicalChannel;
  outPacket.senderUser = packet.senderUser;
  outPacket.streamId = packet.streamId;
  outPacket.sequence = packet.sequence;
  outPacket.sampleCount = packet.sampleCount;
  if (packet.type == WIRE_START) {
    outPacket.type = PacketType::Start;
  } else if (packet.type == WIRE_END) {
    outPacket.type = PacketType::End;
  } else {
    outPacket.type = PacketType::Audio;
  }
  if (packet.sampleCount > 0) {
    memcpy(
        outPacket.samples,
        packet.samples,
        packet.sampleCount * sizeof(int16_t));
  }
  return true;
}

bool takeSwitchRequest(SwitchRequest& outRequest) {
  bool available = false;
  portENTER_CRITICAL(&p2pMux);
  if (switchPending) {
    outRequest = pendingSwitch;
    switchPending = false;
    available = true;
  }
  portEXIT_CRITICAL(&p2pMux);
  return available;
}

void clearReceiveQueue() {
  if (receiveQueue != nullptr) {
    xQueueReset(receiveQueue);
  }
  portENTER_CRITICAL(&p2pMux);
  switchPending = false;
  pendingSwitch = SwitchRequest{};
  lastSwitchChannel = 0;
  lastSwitchStreamId = 0;
  portEXIT_CRITICAL(&p2pMux);
}

}  // namespace P2pAudio
