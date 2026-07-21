#include "app_config.h"
#include "availability_service.h"
#include "audio_io.h"
#include "communication_state.h"
#include "consts.h"
#include "gui.h"
#include "p2p_audio.h"
#include "rtdb_audio_stream.h"
#include "rtdb_request_service.h"
#include "runtime_statistics.h"
#include "wifi_connection.h"

#include <esp_system.h>
#include <freertos/FreeRTOS.h>
#include <freertos/queue.h>
#include <freertos/task.h>
#include <string.h>
#include <sys/time.h>
#include <time.h>
#include <esp_heap_caps.h>

namespace {

constexpr uint32_t SSE_TASK_STACK_BYTES = 12288;
constexpr uint8_t SSE_TASK_PRIORITY = 2;
constexpr uint8_t SSE_TASK_CORE = 0;
constexpr uint32_t SSE_IDLE_DELAY_MS = 2;

struct PlaybackBlock {
  int16_t samples[AudioConfig::CHUNK_SAMPLES];
  size_t sampleCount = 0;
  uint32_t sequence = 0;
  // P2P aggregates are consecutive sections of one live stream and therefore
  // skip the per-block DMA drain. VoIP chunks retain the existing behavior.
  bool continuousPlayback = false;
  char sessionId[64] = {0};
  char deviceId[48] = {0};
};

static_assert(
    P2pAudioConfig::PLAYBACK_SAMPLES_PER_BLOCK <= AudioConfig::CHUNK_SAMPLES,
    "P2P playback aggregate exceeds PlaybackBlock capacity");

PlaybackBlock playbackBlocks[PlaybackConfig::BLOCK_COUNT];
QueueHandle_t playbackFreeQueue = nullptr;
QueueHandle_t playbackReadyQueue = nullptr;
TaskHandle_t playbackTaskHandle = nullptr;
TaskHandle_t sseTaskHandle = nullptr;
TaskHandle_t communicationTaskHandle = nullptr;
TaskHandle_t activityLedTaskHandle = nullptr;

portMUX_TYPE playbackMux = portMUX_INITIALIZER_UNLOCKED;
portMUX_TYPE channelMux = portMUX_INITIALIZER_UNLOCKED;
bool playbackInFlight = false;
constexpr uint8_t NO_CHANNEL = 0;
uint8_t listeningChannel = NO_CHANNEL;
uint8_t pendingChannel = NO_CHANNEL;
bool channelChangePending = false;
portMUX_TYPE sseControlMux = portMUX_INITIALIZER_UNLOCKED;
bool ssePauseRequested = false;
bool ssePauseAcknowledged = false;

portMUX_TYPE guiPttMux = portMUX_INITIALIZER_UNLOCKED;
bool guiPttCaptureActive = false;
bool guiPttSuppressedUntilRelease = false;
bool guiCommunicationBlocked = false;

enum class ActivityLedMode : uint8_t {
  Off,
  Recording,
  Busy,
};

portMUX_TYPE activityLedMux = portMUX_INITIALIZER_UNLOCKED;
ActivityLedMode currentActivityLedMode = ActivityLedMode::Off;

AvailabilityService::UserStatus availabilitySnapshot[AvailabilityConfig::USER_COUNT];
int16_t droppedMicBuffer[AudioConfig::CHUNK_SAMPLES];
int16_t p2pMicBuffer[P2pAudioConfig::SAMPLES_PER_PACKET];

portMUX_TYPE transportSessionMux = portMUX_INITIALIZER_UNLOCKED;
uint32_t channelFailureBaseline = 0;

void setGuiPttCaptureActive(bool active) {
  portENTER_CRITICAL(&guiPttMux);
  guiPttCaptureActive = active;
  portEXIT_CRITICAL(&guiPttMux);
}

void setGuiCommunicationBlocked(bool blocked) {
  bool changed = false;
  portENTER_CRITICAL(&guiPttMux);
  changed = guiCommunicationBlocked != blocked;
  guiCommunicationBlocked = blocked;
  portEXIT_CRITICAL(&guiPttMux);

  if (changed) {
    Serial.printf(
        "[GUI] communication_blocked=%s screen=statistics\n",
        blocked ? "true" : "false");
  }
  AudioIO::setSpeakerPlaybackEnabled(!blocked);
}

bool isGuiCommunicationBlocked() {
  portENTER_CRITICAL(&guiPttMux);
  const bool blocked = guiCommunicationBlocked;
  portEXIT_CRITICAL(&guiPttMux);
  return blocked;
}

void suppressPttUntilRelease() {
  portENTER_CRITICAL(&guiPttMux);
  guiPttSuppressedUntilRelease = true;
  portEXIT_CRITICAL(&guiPttMux);
}

void clearPttReleaseSuppression() {
  portENTER_CRITICAL(&guiPttMux);
  guiPttSuppressedUntilRelease = false;
  portEXIT_CRITICAL(&guiPttMux);
}

void readGuiPttState(bool& captureActive, bool& suppressedUntilRelease) {
  portENTER_CRITICAL(&guiPttMux);
  captureActive = guiPttCaptureActive;
  suppressedUntilRelease = guiPttSuppressedUntilRelease;
  portEXIT_CRITICAL(&guiPttMux);
}

const char* activityLedModeName(ActivityLedMode mode) {
  switch (mode) {
    case ActivityLedMode::Off: return "off";
    case ActivityLedMode::Recording: return "recording_solid";
    case ActivityLedMode::Busy: return "busy_blink";
    default: return "unknown";
  }
}

ActivityLedMode activityLedMode() {
  portENTER_CRITICAL(&activityLedMux);
  const ActivityLedMode mode = currentActivityLedMode;
  portEXIT_CRITICAL(&activityLedMux);
  return mode;
}

void setActivityLedMode(ActivityLedMode mode) {
  bool changed = false;
  ActivityLedMode previous = ActivityLedMode::Off;

  portENTER_CRITICAL(&activityLedMux);
  previous = currentActivityLedMode;
  if (previous != mode) {
    currentActivityLedMode = mode;
    changed = true;
  }
  portEXIT_CRITICAL(&activityLedMux);

  if (!changed) {
    return;
  }

  Serial.printf(
      "[LED] old=%s new=%s\n",
      activityLedModeName(previous),
      activityLedModeName(mode));

  if (activityLedTaskHandle != nullptr) {
    xTaskNotifyGive(activityLedTaskHandle);
  }
}

void activityLedTask(void*) {
  ActivityLedMode appliedMode = ActivityLedMode::Off;
  bool initialized = false;
  bool ledOn = false;

  for (;;) {
    const ActivityLedMode requestedMode = activityLedMode();
    if (!initialized || requestedMode != appliedMode) {
      appliedMode = requestedMode;
      initialized = true;

      // Enter the busy state with the LED on, then alternate at the configured
      // interval. Recording remains continuously lit and idle remains off.
      ledOn = appliedMode != ActivityLedMode::Off;
      digitalWrite(Pins::RECORDING_LED, ledOn ? HIGH : LOW);
    }

    const TickType_t waitTicks =
        appliedMode == ActivityLedMode::Busy
            ? pdMS_TO_TICKS(ActivityLedConfig::BUSY_BLINK_INTERVAL_MS)
            : portMAX_DELAY;

    const uint32_t notificationCount = ulTaskNotifyTake(pdTRUE, waitTicks);
    if (notificationCount == 0 && appliedMode == ActivityLedMode::Busy) {
      ledOn = !ledOn;
      digitalWrite(Pins::RECORDING_LED, ledOn ? HIGH : LOW);
    }
  }
}

class DebouncedPtt {
 public:
  void begin() {
    lastRawPressed_ = rawPressed();
    stablePressed_ = lastRawPressed_;
    lastChangeMs_ = millis();
  }

  void update() {
    const bool raw = rawPressed();
    const uint32_t now = millis();
    if (raw != lastRawPressed_) {
      lastRawPressed_ = raw;
      lastChangeMs_ = now;
    }
    if (raw != stablePressed_ && (now - lastChangeMs_) >= ButtonLogic::DEBOUNCE_MS) {
      stablePressed_ = raw;
    }
  }

  bool pressed() const { return stablePressed_; }
  bool rawPressed() const { return digitalRead(Pins::MAIN_BUTTON) == ButtonLogic::PRESSED; }

 private:
  bool lastRawPressed_ = false;
  bool stablePressed_ = false;
  uint32_t lastChangeMs_ = 0;
};

void wakeTaskOnWifiRestore(void* context) {
  TaskHandle_t task = static_cast<TaskHandle_t>(context);
  if (task != nullptr) {
    xTaskNotifyGive(task);
  }
}

bool isSsePauseRequested() {
  portENTER_CRITICAL(&sseControlMux);
  const bool requested = ssePauseRequested;
  portEXIT_CRITICAL(&sseControlMux);
  return requested;
}

void setSsePauseAcknowledged(bool acknowledged) {
  portENTER_CRITICAL(&sseControlMux);
  ssePauseAcknowledged = acknowledged;
  portEXIT_CRITICAL(&sseControlMux);
}

bool isSsePauseAcknowledged() {
  portENTER_CRITICAL(&sseControlMux);
  const bool acknowledged = ssePauseAcknowledged;
  portEXIT_CRITICAL(&sseControlMux);
  return acknowledged;
}

bool pauseSseForRtdbRequests(uint32_t timeoutMs) {
  portENTER_CRITICAL(&sseControlMux);
  ssePauseRequested = true;
  ssePauseAcknowledged = false;
  portEXIT_CRITICAL(&sseControlMux);

  if (sseTaskHandle != nullptr) {
    xTaskNotifyGive(sseTaskHandle);
  }

  const uint32_t startedAtMs = millis();

  while (!isSsePauseAcknowledged()) {
    if ((millis() - startedAtMs) >= timeoutMs) {
      Serial.printf(
          "[RTDB][STREAM] pause_timeout timeoutMs=%lu listening=%s\n",
          static_cast<unsigned long>(timeoutMs),
          RtdbAudioStream::isListening() ? "true" : "false");
      return false;
    }

    vTaskDelay(pdMS_TO_TICKS(5));
  }

  // Give the TLS allocator a brief opportunity to release its buffers.
  vTaskDelay(pdMS_TO_TICKS(20));

  Serial.printf(
      "[RTDB][STREAM] paused_for_requests freeHeap=%lu largestBlock=%lu\n",
      static_cast<unsigned long>(ESP.getFreeHeap()),
      static_cast<unsigned long>(
          heap_caps_get_largest_free_block(MALLOC_CAP_8BIT)));

  return true;
}

void resumeSseAfterRtdbRequests() {
  portENTER_CRITICAL(&sseControlMux);
  ssePauseRequested = false;
  ssePauseAcknowledged = false;
  portEXIT_CRITICAL(&sseControlMux);

  Serial.println("[RTDB][STREAM] resume_requested");

  if (sseTaskHandle != nullptr) {
    xTaskNotifyGive(sseTaskHandle);
  }
}

uint32_t flushPlaybackQueue(const char* reason);

uint8_t currentListeningChannel() {
  portENTER_CRITICAL(&channelMux);
  const uint8_t channel = listeningChannel;
  portEXIT_CRITICAL(&channelMux);
  return channel;
}

uint8_t userNumberFromDeviceId(const char* deviceId) {
  if (deviceId == nullptr) {
    return 0;
  }
  const size_t length = strlen(deviceId);
  if (length < 2) {
    return 0;
  }
  const char tens = deviceId[length - 2];
  const char ones = deviceId[length - 1];
  if (tens < '0' || tens > '9' || ones < '0' || ones > '9') {
    return 0;
  }
  const uint8_t userNumber =
      static_cast<uint8_t>((tens - '0') * 10 + (ones - '0'));
  return userNumber >= 1 && userNumber <= AvailabilityConfig::USER_COUNT
             ? userNumber
             : 0;
}

void syncRuntimeStatisticsNetworkCounters() {
  RuntimeStatistics::setVoipNetworkCounters(
      RtdbRequestService::totalAudioUploadFailures(),
      RtdbRequestService::totalDiscardedAudioSamples());
}

uint32_t failedUploadsInJoinedChannel() {
  const uint32_t total = RtdbRequestService::totalAudioUploadFailures();
  portENTER_CRITICAL(&transportSessionMux);
  const uint32_t baseline = channelFailureBaseline;
  portEXIT_CRITICAL(&transportSessionMux);
  return total - baseline;
}

bool voipFailureLimitExceeded() {
  if (!Communication::automaticP2pDowngradeEnabled()) {
    return false;
  }

  const uint32_t limit =
      P2pAudioConfig::VOIP_FAILED_UPLOAD_ATTEMPT_LIMIT;
  return limit == 0 || failedUploadsInJoinedChannel() > limit;
}

void resetChannelTransport(uint8_t channel) {
  const uint32_t baseline =
      RtdbRequestService::totalAudioUploadFailures();
  portENTER_CRITICAL(&transportSessionMux);
  channelFailureBaseline = baseline;
  portEXIT_CRITICAL(&transportSessionMux);

  P2pAudio::clearReceiveQueue();
  const bool wifiEnabled = WifiConnection::isEnabled();
  const bool automaticP2pDowngrade =
      Communication::automaticP2pDowngradeEnabled();
  const bool immediateP2p =
      !wifiEnabled ||
      (automaticP2pDowngrade &&
       P2pAudioConfig::VOIP_FAILED_UPLOAD_ATTEMPT_LIMIT == 0);
  const Communication::Transport initialTransport =
      immediateP2p ? Communication::Transport::P2p
                   : Communication::Transport::Voip;
  Communication::setTransport(
      initialTransport,
      channel == NO_CHANNEL ? "channel_left" : "channel_joined");

  if (channel != NO_CHANNEL &&
      initialTransport == Communication::Transport::P2p) {
    // Explicit Wi-Fi disable and zero-threshold automatic fallback both start
    // the joined channel in P2P. Announce so a peer still on VoIP follows us.
    P2pAudio::announceSwitch(channel, 0);
  }

  Serial.printf(
      "[TRANSPORT] channel=%u failureBaseline=%lu limit=%lu wifi=%s autoP2p=%s initial=%s\n",
      static_cast<unsigned int>(channel),
      static_cast<unsigned long>(baseline),
      static_cast<unsigned long>(
          P2pAudioConfig::VOIP_FAILED_UPLOAD_ATTEMPT_LIMIT),
      wifiEnabled ? "enabled" : "disabled",
      automaticP2pDowngrade ? "enabled" : "disabled",
      Communication::transportName(initialTransport));

  if (sseTaskHandle != nullptr) {
    xTaskNotifyGive(sseTaskHandle);
  }
}

bool activateP2pTransport(const char* reason, bool announce) {
  const uint8_t channel = currentListeningChannel();
  if (channel == NO_CHANNEL) {
    return false;
  }

  const uint32_t failures = failedUploadsInJoinedChannel();
  const bool changed =
      Communication::transport() != Communication::Transport::P2p;
  Communication::setTransport(Communication::Transport::P2p, reason);

  // Stop admitting new RTDB audio immediately. Any request already inside the
  // bounded Firebase call is allowed to return, while queued stale PCM is freed.
  RtdbRequestService::setRecordingActive(false);
  RtdbRequestService::requestAudioUploadAbort("transport_switched_to_p2p");
  RtdbRequestService::discardPendingAudio("transport_switched_to_p2p");

  if (announce) {
    P2pAudio::announceSwitch(channel, failures);
  }

  if (sseTaskHandle != nullptr) {
    xTaskNotifyGive(sseTaskHandle);
  }
  if (communicationTaskHandle != nullptr) {
    xTaskNotifyGive(communicationTaskHandle);
  }

  if (changed) {
    RuntimeStatistics::recordFallback();
    Serial.printf(
        "[TRANSPORT] fallback channel=%u failures=%lu limit=%lu announce=%s\n",
        static_cast<unsigned int>(channel),
        static_cast<unsigned long>(failures),
        static_cast<unsigned long>(
            P2pAudioConfig::VOIP_FAILED_UPLOAD_ATTEMPT_LIMIT),
        announce ? "true" : "false");
  }
  return true;
}

void processRemoteP2pSwitchRequest() {
  P2pAudio::SwitchRequest request;
  while (P2pAudio::takeSwitchRequest(request)) {
    const uint8_t channel = currentListeningChannel();
    if (channel == NO_CHANNEL || request.logicalChannel != channel) {
      Serial.printf(
          "[P2P] switch_ignored sourceUser=%u packetChannel=%u joinedChannel=%u\n",
          static_cast<unsigned int>(request.senderUser),
          static_cast<unsigned int>(request.logicalChannel),
          static_cast<unsigned int>(channel));
      continue;
    }

    if (Communication::transport() == Communication::Transport::P2p) {
      continue;
    }

    Serial.printf(
        "[P2P] remote_switch sourceUser=%u channel=%u reportedFailures=%lu\n",
        static_cast<unsigned int>(request.senderUser),
        static_cast<unsigned int>(request.logicalChannel),
        static_cast<unsigned long>(request.failedUploads));
    activateP2pTransport("peer_requested_p2p", false);
  }
}

void syncCommunicationMethodToGui() {
  static Communication::Transport lastShown =
      static_cast<Communication::Transport>(0xff);
  const Communication::Transport current = Communication::transport();
  if (current == lastShown) {
    return;
  }
  lastShown = current;
  Gui::appGui.setCommunicationMethod(
      current == Communication::Transport::P2p
          ? Gui::CommunicationMethod::P2p
          : Gui::CommunicationMethod::Voip);
}

void requestLogicalChannel(uint8_t channel) {
  // Channel 0 is the explicit "not joined" state.
  if (channel > Gui::CHANNEL_COUNT) {
    return;
  }
  portENTER_CRITICAL(&channelMux);
  pendingChannel = channel;
  channelChangePending = true;
  portEXIT_CRITICAL(&channelMux);
  if (channel == NO_CHANNEL) {
    Serial.printf(
        "[CHANNEL] requested=none state=%s\n",
        Communication::stateName(Communication::state()));
  } else {
    Serial.printf(
        "[CHANNEL] requested=%u state=%s\n",
        static_cast<unsigned int>(channel),
        Communication::stateName(Communication::state()));
  }
  if (communicationTaskHandle != nullptr) {
    xTaskNotifyGive(communicationTaskHandle);
  }
}

void applyPendingLogicalChannel() {
  bool changed = false;
  uint8_t channel = NO_CHANNEL;
  uint8_t previousChannel = NO_CHANNEL;
  portENTER_CRITICAL(&channelMux);
  if (channelChangePending) {
    previousChannel = listeningChannel;
    listeningChannel = pendingChannel;
    channel = listeningChannel;
    channelChangePending = false;
    changed = true;
  }
  portEXIT_CRITICAL(&channelMux);
  if (!changed) {
    return;
  }

  resetChannelTransport(channel);

  if (channel == NO_CHANNEL) {
    const uint32_t discarded = flushPlaybackQueue("left_channel");
    Serial.printf(
        "[CHANNEL] left previous=%u discardedPlayback=%lu freeHeap=%lu largestBlock=%lu\n",
        static_cast<unsigned int>(previousChannel),
        static_cast<unsigned long>(discarded),
        static_cast<unsigned long>(ESP.getFreeHeap()),
        static_cast<unsigned long>(
            heap_caps_get_largest_free_block(MALLOC_CAP_8BIT)));
  } else {
    Serial.printf("[CHANNEL] active=%u\n", static_cast<unsigned int>(channel));
  }

  // Leaving a joined channel (including a direct switch to another channel)
  // requests a save. The actual NVS write is performed by the Arduino loop,
  // not by this communication-task path.
  if (previousChannel != NO_CHANNEL && previousChannel != channel) {
    RuntimeStatistics::requestPersistenceSave();
  }

  if (sseTaskHandle != nullptr) {
    xTaskNotifyGive(sseTaskHandle);
  }
}

Gui::AvailabilityIndicator toGuiAvailabilityIndicator(
    AvailabilityService::EvidenceState state) {
  switch (state) {
    case AvailabilityService::EvidenceState::Available:
      return Gui::AvailabilityIndicator::Available;
    case AvailabilityService::EvidenceState::Stale:
      return Gui::AvailabilityIndicator::Stale;
    case AvailabilityService::EvidenceState::Unknown:
    default:
      return Gui::AvailabilityIndicator::Unknown;
  }
}

Gui::AvailabilityRefreshIndicator toGuiRefreshIndicator(
    AvailabilityService::RefreshState state) {
  switch (state) {
    case AvailabilityService::RefreshState::Refreshing:
      return Gui::AvailabilityRefreshIndicator::Refreshing;
    case AvailabilityService::RefreshState::Complete:
      return Gui::AvailabilityRefreshIndicator::Complete;
    case AvailabilityService::RefreshState::Failed:
      return Gui::AvailabilityRefreshIndicator::Failed;
    case AvailabilityService::RefreshState::Idle:
    default:
      return Gui::AvailabilityRefreshIndicator::Idle;
  }
}

void syncAvailabilityToGui() {
  static uint32_t lastRefreshMs = 0;
  const uint32_t now = millis();
  if ((now - lastRefreshMs) < AvailabilityConfig::GUI_REFRESH_MS) {
    return;
  }
  lastRefreshMs = now;

  const size_t count = AvailabilityService::copyUserStatuses(
      availabilitySnapshot,
      AvailabilityConfig::USER_COUNT);
  for (size_t index = 0; index < count; ++index) {
    Gui::appGui.setUserStatus(
        availabilitySnapshot[index].userNumber,
        toGuiAvailabilityIndicator(availabilitySnapshot[index].voip),
        toGuiAvailabilityIndicator(availabilitySnapshot[index].p2p),
        availabilitySnapshot[index].voipAgeSeconds,
        availabilitySnapshot[index].p2pAgeSeconds);
  }
  Gui::appGui.setAvailabilityRefreshIndicator(
      toGuiRefreshIndicator(AvailabilityService::refreshState()));
}

void maybeRunAvailabilityMaintenance() {
  const uint8_t channel = currentListeningChannel();
  const bool communicationBusy = Communication::isTransmitting();
  if (!AvailabilityService::maintenanceLeaseDue(channel, communicationBusy) ||
      Communication::transport() != Communication::Transport::Voip ||
      Communication::state() != Communication::State::Listening) {
    return;
  }

  Serial.println("[Availability][MAINTENANCE] pausing SSE for lease");
  if (!pauseSseForRtdbRequests(CommunicationConfig::SSE_PAUSE_TIMEOUT_MS)) {
    Serial.println("[Availability][MAINTENANCE] SSE pause failed");
    resumeSseAfterRtdbRequests();
    return;
  }

  const bool success = AvailabilityService::performMaintenanceLeaseSync(
      AvailabilityConfig::SSE_MAINTENANCE_OPERATION_TIMEOUT_MS);
  Serial.printf(
      "[Availability][MAINTENANCE] leaseSuccess=%s\n",
      success ? "true" : "false");
  resumeSseAfterRtdbRequests();
}

void handleGuiResult(const Gui::UpdateResult& result) {
  if (result.consumedPttNavigation) {
    suppressPttUntilRelease();
  }
  if (result.hasSelection) {
    Serial.printf(
        "[GUI] selectedScreen=%u\n",
        static_cast<unsigned int>(result.selectedScreen));
  }
  if (result.availabilityRefreshRequested && AvailabilityConfig::ENABLED) {
    AvailabilityService::requestRefresh();
  }
  if (result.availabilityScreenClosed && AvailabilityConfig::ENABLED) {
    AvailabilityService::cancelRefresh();
  }
  if (result.hasChannelLeave) {
    Serial.println("[GUI] leave_channel");
    requestLogicalChannel(NO_CHANNEL);
  } else if (result.hasChannelSelection) {
    requestLogicalChannel(result.selectedChannel);
  }
}

bool isPlaybackInFlight() {
  portENTER_CRITICAL(&playbackMux);
  const bool active = playbackInFlight;
  portEXIT_CRITICAL(&playbackMux);
  return active;
}

void setPlaybackInFlight(bool active) {
  portENTER_CRITICAL(&playbackMux);
  playbackInFlight = active;
  portEXIT_CRITICAL(&playbackMux);
}

uint32_t flushPlaybackQueue(const char* reason) {
  uint32_t discarded = 0;
  uint8_t index = 0;
  while (playbackReadyQueue != nullptr &&
         xQueueReceive(playbackReadyQueue, &index, 0) == pdTRUE) {
    xQueueSend(playbackFreeQueue, &index, portMAX_DELAY);
    ++discarded;
  }
  if (discarded > 0) {
    Serial.printf(
        "[AUDIO_RX] discard reason=%s count=%lu\n",
        reason == nullptr ? "half_duplex" : reason,
        static_cast<unsigned long>(discarded));
  }
  return discarded;
}

bool waitForPlaybackToStop() {
  flushPlaybackQueue("transmitting");
  const uint32_t startedAt = millis();
  while (isPlaybackInFlight() &&
         (millis() - startedAt) < CommunicationConfig::PLAYBACK_STOP_TIMEOUT_MS) {
    vTaskDelay(pdMS_TO_TICKS(5));
  }
  if (isPlaybackInFlight()) {
    Serial.println("[AUDIO_RX] playback_stop_timeout");
    return false;
  }

  // P2P blocks deliberately stream continuously. Drain any final speaker DMA
  // frames once the queue and active write are stopped, before enabling the mic.
  if (!AudioIO::drainSpeaker()) {
    Serial.println("[AUDIO_RX] speaker_drain_failed");
    return false;
  }
  return true;
}

uint8_t acquireSseScratchBlock() {
  uint8_t index = 0;
  if (xQueueReceive(playbackFreeQueue, &index, 0) == pdTRUE) {
    return index;
  }

  // Keep the stream serviced even if speaker playback temporarily owns one
  // block and the ready queue owns the rest. Drop the oldest queued audio.
  if (xQueueReceive(playbackReadyQueue, &index, 0) == pdTRUE) {
    Serial.printf(
        "[AUDIO_RX] discard reason=queue_full policy=drop_oldest readyDepth=%lu\n",
        static_cast<unsigned long>(uxQueueMessagesWaiting(playbackReadyQueue)));
    return index;
  }

  xQueueReceive(playbackFreeQueue, &index, portMAX_DELAY);
  return index;
}

void enqueuePlaybackBlock(uint8_t index) {
  if (xQueueSend(playbackReadyQueue, &index, 0) == pdTRUE) {
    return;
  }

  uint8_t oldest = 0;
  if (xQueueReceive(playbackReadyQueue, &oldest, 0) == pdTRUE) {
    Serial.printf(
        "[AUDIO_RX] discard reason=queue_full policy=drop_oldest session=%s seq=%lu\n",
        playbackBlocks[oldest].sessionId,
        static_cast<unsigned long>(playbackBlocks[oldest].sequence));
    xQueueSend(playbackFreeQueue, &oldest, portMAX_DELAY);
  }
  if (xQueueSend(playbackReadyQueue, &index, 0) != pdTRUE) {
    Serial.println("[AUDIO_RX] discard reason=queue_full policy=drop_newest");
    xQueueSend(playbackFreeQueue, &index, portMAX_DELAY);
  }
}

void playbackTask(void*) {
  uint8_t blockIndex = 0;
  for (;;) {
    if (xQueueReceive(playbackReadyQueue, &blockIndex, portMAX_DELAY) != pdTRUE) {
      continue;
    }

    PlaybackBlock& block = playbackBlocks[blockIndex];
    if (isGuiCommunicationBlocked() ||
        !Communication::permitsPlayback() ||
        currentListeningChannel() == NO_CHANNEL) {
      Serial.printf(
          "[AUDIO_RX] discard reason=%s session=%s seq=%lu\n",
          isGuiCommunicationBlocked()
              ? "statistics_screen"
              : (currentListeningChannel() == NO_CHANNEL ? "not_in_channel"
                                                         : "transmitting"),
          block.sessionId,
          static_cast<unsigned long>(block.sequence));
      xQueueSend(playbackFreeQueue, &blockIndex, portMAX_DELAY);
      continue;
    }

    setPlaybackInFlight(true);
    if (isGuiCommunicationBlocked() ||
        !Communication::permitsPlayback() ||
        currentListeningChannel() == NO_CHANNEL) {
      setPlaybackInFlight(false);
      xQueueSend(playbackFreeQueue, &blockIndex, portMAX_DELAY);
      continue;
    }

    const bool played = AudioIO::playPcm16(
        block.samples,
        block.sampleCount,
        !block.continuousPlayback);
    setPlaybackInFlight(false);
    Serial.printf(
        "[AUDIO_RX] playback success=%s source=%s session=%s seq=%lu samples=%u\n",
        played ? "true" : "false",
        block.deviceId,
        block.sessionId,
        static_cast<unsigned long>(block.sequence),
        static_cast<unsigned int>(block.sampleCount));
    if (played) {
      RuntimeStatistics::recordPlayedSamples(
          block.sampleCount,
          userNumberFromDeviceId(block.deviceId));
    }
    xQueueSend(playbackFreeQueue, &blockIndex, portMAX_DELAY);
  }
}

void sseListenerTask(void*) {
  WifiConnection::registerConnectedObserver(
      wakeTaskOnWifiRestore,
      xTaskGetCurrentTaskHandle());

  uint8_t scratchIndex = acquireSseScratchBlock();
  uint8_t connectedChannel = 0;
  uint32_t lastReconnectAttemptMs = 0;
  uint32_t reconnectAllowedAtMs = 0;
  bool wasPaused = false;

  uint32_t p2pAggregateStreamId = 0;
  uint32_t p2pExpectedSequence = 0;
  uint8_t p2pAggregateSenderUser = 0;
  uint8_t p2pPacketsInAggregate = 0;

  for (;;) {
    if (isSsePauseRequested()) {
      if (RtdbAudioStream::isListening()) {
        RtdbAudioStream::stopListening();
      }

      connectedChannel = 0;
      lastReconnectAttemptMs = millis();
      wasPaused = true;
      setSsePauseAcknowledged(true);

      ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(50));
      continue;
    }

    if (isGuiCommunicationBlocked()) {
      if (RtdbAudioStream::isListening()) {
        RtdbAudioStream::stopListening();
      }
      connectedChannel = NO_CHANNEL;
      flushPlaybackQueue("statistics_screen");
      P2pAudio::clearReceiveQueue();
      playbackBlocks[scratchIndex].sampleCount = 0;
      p2pAggregateStreamId = 0;
      p2pExpectedSequence = 0;
      p2pAggregateSenderUser = 0;
      p2pPacketsInAggregate = 0;
      ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(50));
      continue;
    }

    if (wasPaused) {
      wasPaused = false;
      reconnectAllowedAtMs =
          millis() + CommunicationConfig::SSE_RESUME_RECONNECT_GRACE_MS;
    }
    setSsePauseAcknowledged(false);

    const uint8_t requestedChannel = currentListeningChannel();
    if (requestedChannel == NO_CHANNEL) {
      if (RtdbAudioStream::isListening()) {
        RtdbAudioStream::stopListening();
      }
      connectedChannel = NO_CHANNEL;
      flushPlaybackQueue("not_in_channel");
      P2pAudio::clearReceiveQueue();
      playbackBlocks[scratchIndex].sampleCount = 0;
      p2pAggregateStreamId = 0;
      p2pExpectedSequence = 0;
      p2pAggregateSenderUser = 0;
      p2pPacketsInAggregate = 0;
      ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(250));
      continue;
    }

    if (Communication::transport() == Communication::Transport::P2p) {
      if (RtdbAudioStream::isListening()) {
        RtdbAudioStream::stopListening();
      }
      connectedChannel = 0;

      P2pAudio::ReceivedPacket packet;
      if (!P2pAudio::takeReceivedPacket(packet, 20)) {
        ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(5));
        continue;
      }
      if (packet.logicalChannel != requestedChannel) {
        Serial.printf(
            "[P2P_RX] discard reason=wrong_channel sourceUser=%u packetChannel=%u joinedChannel=%u\n",
            static_cast<unsigned int>(packet.senderUser),
            static_cast<unsigned int>(packet.logicalChannel),
            static_cast<unsigned int>(requestedChannel));
        continue;
      }

      if (packet.type == P2pAudio::PacketType::Start) {
        flushPlaybackQueue("p2p_stream_start");
        PlaybackBlock& aggregate = playbackBlocks[scratchIndex];
        aggregate.sampleCount = 0;
        aggregate.continuousPlayback = true;
        p2pAggregateStreamId = packet.streamId;
        p2pExpectedSequence = packet.sequence + 1;
        p2pAggregateSenderUser = packet.senderUser;
        p2pPacketsInAggregate = 0;
        Serial.printf(
            "[P2P_RX] stream_start sourceUser=%u channel=%u stream=%lu aggregatePackets=%u aggregateSamples=%u\n",
            static_cast<unsigned int>(packet.senderUser),
            static_cast<unsigned int>(packet.logicalChannel),
            static_cast<unsigned long>(packet.streamId),
            static_cast<unsigned int>(
                P2pAudioConfig::PLAYBACK_PACKETS_PER_BLOCK),
            static_cast<unsigned int>(
                P2pAudioConfig::PLAYBACK_SAMPLES_PER_BLOCK));
        continue;
      }

      if (packet.type == P2pAudio::PacketType::End) {
        PlaybackBlock& aggregate = playbackBlocks[scratchIndex];
        const bool matchingAggregate =
            p2pAggregateStreamId == packet.streamId &&
            p2pAggregateSenderUser == packet.senderUser;
        const bool flushedPartial = matchingAggregate && aggregate.sampleCount > 0;
        if (flushedPartial) {
          aggregate.sequence = packet.sequence;
          enqueuePlaybackBlock(scratchIndex);
          scratchIndex = acquireSseScratchBlock();
        }
        playbackBlocks[scratchIndex].sampleCount = 0;
        playbackBlocks[scratchIndex].continuousPlayback = true;
        p2pAggregateStreamId = 0;
        p2pExpectedSequence = 0;
        p2pAggregateSenderUser = 0;
        p2pPacketsInAggregate = 0;
        Serial.printf(
            "[P2P_RX] stream_end sourceUser=%u channel=%u stream=%lu seq=%lu partialFlushed=%s\n",
            static_cast<unsigned int>(packet.senderUser),
            static_cast<unsigned int>(packet.logicalChannel),
            static_cast<unsigned long>(packet.streamId),
            static_cast<unsigned long>(packet.sequence),
            flushedPartial ? "true" : "false");
        continue;
      }

      if (!Communication::permitsPlayback()) {
        // Do not preserve pre-transmission samples and later combine them with
        // packets received after the local half-duplex transmission finishes.
        playbackBlocks[scratchIndex].sampleCount = 0;
        p2pPacketsInAggregate = 0;
        p2pExpectedSequence = packet.sequence + 1;
        Serial.printf(
            "[P2P_RX] discard reason=transmitting sourceUser=%u stream=%lu seq=%lu\n",
            static_cast<unsigned int>(packet.senderUser),
            static_cast<unsigned long>(packet.streamId),
            static_cast<unsigned long>(packet.sequence));
        continue;
      }

      // Count only P2P AUDIO packets observed while this device is actually in
      // receive mode. Sequence-gap accounting below uses the same observation
      // window, making the displayed loss percentage internally consistent.
      RuntimeStatistics::recordP2pPacketReceived();

      // START is unacknowledged in this protocol revision. AUDIO therefore acts
      // as an implicit start when the explicit START packet was missed.
      if (p2pAggregateStreamId != packet.streamId ||
          p2pAggregateSenderUser != packet.senderUser) {
        // AUDIO can be the first observed packet when START or earlier AUDIO
        // packets were lost. Sequence 1 is the first valid audio packet because
        // sequence 0 belongs to START.
        if (packet.sequence > 1) {
          RuntimeStatistics::recordP2pPacketsMissed(packet.sequence - 1);
        }
        PlaybackBlock& aggregate = playbackBlocks[scratchIndex];
        if (aggregate.sampleCount > 0) {
          Serial.printf(
              "[P2P_RX] discard reason=stream_changed partialSamples=%u oldStream=%lu newStream=%lu\n",
              static_cast<unsigned int>(aggregate.sampleCount),
              static_cast<unsigned long>(p2pAggregateStreamId),
              static_cast<unsigned long>(packet.streamId));
        }
        aggregate.sampleCount = 0;
        aggregate.continuousPlayback = true;
        p2pAggregateStreamId = packet.streamId;
        p2pExpectedSequence = packet.sequence;
        p2pAggregateSenderUser = packet.senderUser;
        p2pPacketsInAggregate = 0;
      }

      if (packet.sequence != p2pExpectedSequence) {
        Serial.printf(
            "[P2P_RX] sequence_gap sourceUser=%u stream=%lu expected=%lu received=%lu\n",
            static_cast<unsigned int>(packet.senderUser),
            static_cast<unsigned long>(packet.streamId),
            static_cast<unsigned long>(p2pExpectedSequence),
            static_cast<unsigned long>(packet.sequence));
        if (packet.sequence > p2pExpectedSequence) {
          RuntimeStatistics::recordP2pPacketsMissed(
              packet.sequence - p2pExpectedSequence);
        }
      }
      p2pExpectedSequence = packet.sequence + 1;

      PlaybackBlock& aggregate = playbackBlocks[scratchIndex];
      constexpr size_t aggregateCapacity =
          P2pAudioConfig::PLAYBACK_SAMPLES_PER_BLOCK;
      if (aggregate.sampleCount > 0 &&
          aggregate.sampleCount + packet.sampleCount > aggregateCapacity) {
        enqueuePlaybackBlock(scratchIndex);
        scratchIndex = acquireSseScratchBlock();
        playbackBlocks[scratchIndex].sampleCount = 0;
        playbackBlocks[scratchIndex].continuousPlayback = true;
        p2pPacketsInAggregate = 0;
      }

      PlaybackBlock& target = playbackBlocks[scratchIndex];
      if (target.sampleCount == 0) {
        target.continuousPlayback = true;
        snprintf(
            target.sessionId,
            sizeof(target.sessionId),
            "p2p-%08lx",
            static_cast<unsigned long>(packet.streamId));
        snprintf(
            target.deviceId,
            sizeof(target.deviceId),
            "p2p-user-%02u",
            static_cast<unsigned int>(packet.senderUser));
      }

      memcpy(
          target.samples + target.sampleCount,
          packet.samples,
          packet.sampleCount * sizeof(int16_t));
      target.sampleCount += packet.sampleCount;
      target.sequence = packet.sequence;
      ++p2pPacketsInAggregate;

      if (p2pPacketsInAggregate >=
              P2pAudioConfig::PLAYBACK_PACKETS_PER_BLOCK ||
          target.sampleCount >= aggregateCapacity) {
        enqueuePlaybackBlock(scratchIndex);
        scratchIndex = acquireSseScratchBlock();
        playbackBlocks[scratchIndex].sampleCount = 0;
        playbackBlocks[scratchIndex].continuousPlayback = true;
        p2pPacketsInAggregate = 0;
      }
      continue;
    }

    // Do not retain a partial ESP-NOW aggregate after returning to VoIP.
    playbackBlocks[scratchIndex].sampleCount = 0;
    p2pAggregateStreamId = 0;
    p2pExpectedSequence = 0;
    p2pAggregateSenderUser = 0;
    p2pPacketsInAggregate = 0;

    if (!WifiConnection::isConnected()) {
      if (RtdbAudioStream::isListening()) {
        RtdbAudioStream::stopListening();
      }
      ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(250));
      continue;
    }

    const bool channelChanged = connectedChannel != requestedChannel;
    if (channelChanged && RtdbAudioStream::isListening()) {
      RtdbAudioStream::stopListening();
    }

    if (!RtdbAudioStream::isListening()) {
      const uint32_t now = millis();
      if (static_cast<int32_t>(now - reconnectAllowedAtMs) < 0) {
        ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(25));
        continue;
      }
      if (isSsePauseRequested() ||
          Communication::transport() != Communication::Transport::Voip) {
        continue;
      }
      if (channelChanged ||
          (now - lastReconnectAttemptMs) >=
              RtdbHttpConfig::STREAM_RECONNECT_INTERVAL_MS) {
        lastReconnectAttemptMs = now;
        Serial.printf(
            "[RTDB][STREAM] reconnect channel=%u\n",
            static_cast<unsigned int>(requestedChannel));
        if (RtdbAudioStream::startListening(requestedChannel)) {
          connectedChannel = requestedChannel;
        }
      }
      ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(50));
      continue;
    }

    PlaybackBlock& scratch = playbackBlocks[scratchIndex];
    RtdbPcmChunk chunk;
    if (!RtdbAudioStream::pollListening(
            scratch.samples,
            AudioConfig::CHUNK_SAMPLES,
            chunk)) {
      ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(SSE_IDLE_DELAY_MS));
      continue;
    }

    scratch.sampleCount = chunk.sampleCount;
    scratch.sequence = chunk.seq;
    scratch.continuousPlayback = false;
    snprintf(
        scratch.sessionId,
        sizeof(scratch.sessionId),
        "%s",
        chunk.sessionId.c_str());
    snprintf(
        scratch.deviceId,
        sizeof(scratch.deviceId),
        "%s",
        chunk.deviceId.c_str());

    if (chunk.deviceId == AppConfig::DEVICE_ID) {
      Serial.printf(
          "[AUDIO_RX] discard reason=self session=%s seq=%lu\n",
          scratch.sessionId,
          static_cast<unsigned long>(scratch.sequence));
      continue;
    }
    if (isGuiCommunicationBlocked() ||
        !Communication::permitsPlayback() ||
        currentListeningChannel() == NO_CHANNEL ||
        Communication::transport() != Communication::Transport::Voip) {
      Serial.printf(
          "[AUDIO_RX] discard reason=%s source=%s session=%s seq=%lu\n",
          isGuiCommunicationBlocked()
              ? "statistics_screen"
              : (currentListeningChannel() == NO_CHANNEL
                     ? "not_in_channel"
                     : (Communication::isTransmitting()
                            ? "transmitting"
                            : "transport_changed")),
          scratch.deviceId,
          scratch.sessionId,
          static_cast<unsigned long>(scratch.sequence));
      continue;
    }

    enqueuePlaybackBlock(scratchIndex);
    scratchIndex = acquireSseScratchBlock();
  }
}

void resetSessionState(
    bool& sessionStarted,
    bool& sessionUsesRtdb,
    bool& p2pStreamStarted,
    char* sessionId,
    size_t sessionIdSize) {
  if (p2pStreamStarted) {
    P2pAudio::endStream();
    p2pStreamStarted = false;
  }
  sessionStarted = false;
  sessionUsesRtdb = false;
  if (sessionId != nullptr && sessionIdSize > 0) {
    sessionId[0] = '\0';
  }
  RtdbRequestService::setRecordingActive(false);
  RtdbRequestService::setAudioPriorityActive(false);
  RtdbRequestService::clearAudioUploadFailure();
  setActivityLedMode(ActivityLedMode::Off);
}

void communicationTask(void*) {
  WifiConnection::registerConnectedObserver(
      wakeTaskOnWifiRestore,
      xTaskGetCurrentTaskHandle());

  DebouncedPtt ptt;
  ptt.begin();
  bool pttAttemptArmed = !ptt.rawPressed();
  bool sessionStarted = false;
  bool sessionUsesRtdb = false;
  bool p2pStreamStarted = false;
  uint8_t sessionChannel = NO_CHANNEL;
  char sessionId[64] = {0};
  uint32_t sequence = 0;
  uint32_t droppedChunks = 0;
  uint64_t sessionCapturedSamples = 0;
  uint32_t sessionStartedAtMs = 0;
  uint32_t drainStartedAtMs = 0;
  bool drainAbortRequested = false;
  bool recordedAtLeastOneChunk = false;
  bool resumeDrainAfterReconnect = false;
  bool rtdbTakeoverHandled = false;

  Communication::begin();

  for (;;) {
    ptt.update();

    bool guiCapturesPtt = false;
    bool pttSuppressedUntilRelease = false;
    readGuiPttState(guiCapturesPtt, pttSuppressedUntilRelease);

    if (guiCapturesPtt || pttSuppressedUntilRelease) {
      pttAttemptArmed = false;
      if (pttSuppressedUntilRelease && !ptt.rawPressed()) {
        clearPttReleaseSuppression();
        pttSuppressedUntilRelease = false;
      }
    } else if (!ptt.rawPressed()) {
      pttAttemptArmed = true;
    }

    // Turning infrastructure Wi-Fi off is an explicit request to continue via
    // ESP-NOW, independent of the automatic failure-downgrade preference.
    if (!WifiConnection::isEnabled() &&
        Communication::transport() == Communication::Transport::Voip) {
      const uint8_t channel = currentListeningChannel();
      if (channel == NO_CHANNEL) {
        Communication::setTransport(
            Communication::Transport::P2p,
            "wifi_disabled_outside_channel");
      } else {
        activateP2pTransport("wifi_disabled", true);
      }
    }

    switch (Communication::state()) {
      case Communication::State::Listening: {
        applyPendingLogicalChannel();

        if (Communication::transport() == Communication::Transport::Voip &&
            voipFailureLimitExceeded()) {
          activateP2pTransport("voip_failure_limit_exceeded_idle", true);
        }

        if (Communication::transport() == Communication::Transport::Voip &&
            !WifiConnection::isConnected()) {
          Communication::transitionTo(
              Communication::State::Reconnecting,
              "wifi_disconnected_idle");
          break;
        }
        if (guiCapturesPtt || pttSuppressedUntilRelease) {
          break;
        }
        if (!ptt.pressed() || !pttAttemptArmed) {
          break;
        }

        const uint8_t joinedChannel = currentListeningChannel();
        if (joinedChannel == NO_CHANNEL) {
          pttAttemptArmed = false;
          Serial.println("[AUDIO_TX] ignored reason=not_in_channel");
          break;
        }

        pttAttemptArmed = false;
        sessionChannel = joinedChannel;
        Communication::transitionTo(
            Communication::State::StartingSession,
            "push_to_talk_pressed");

        snprintf(
            sessionId,
            sizeof(sessionId),
            "%s-%08lx-%lu",
            AppConfig::DEVICE_ID,
            static_cast<unsigned long>(esp_random()),
            static_cast<unsigned long>(millis()));

        if (!ptt.rawPressed()) {
          resetSessionState(
              sessionStarted,
              sessionUsesRtdb,
              p2pStreamStarted,
              sessionId,
              sizeof(sessionId));
          Communication::transitionTo(
              Communication::State::Listening,
              "button_released_before_session_start");
          resumeSseAfterRtdbRequests();
          break;
        }

        if (!waitForPlaybackToStop()) {
          resetSessionState(
              sessionStarted,
              sessionUsesRtdb,
              p2pStreamStarted,
              sessionId,
              sizeof(sessionId));
          Communication::transitionTo(
              Communication::State::Listening,
              "playback_did_not_stop");
          resumeSseAfterRtdbRequests();
          break;
        }

        sessionStarted = true;
        sessionUsesRtdb = false;
        p2pStreamStarted = false;
        sessionStartedAtMs = millis();
        sequence = 0;
        droppedChunks = 0;
        sessionCapturedSamples = 0;
        drainAbortRequested = false;
        recordedAtLeastOneChunk = false;
        resumeDrainAfterReconnect = false;
        rtdbTakeoverHandled = false;
        RtdbRequestService::clearAudioUploadFailure();

        if (Communication::transport() == Communication::Transport::P2p) {
          RtdbRequestService::setAudioPriorityActive(false);
          RtdbRequestService::setRecordingActive(false);
          P2pAudio::announceSwitch(
              sessionChannel,
              failedUploadsInJoinedChannel());
          P2pAudio::startStream(sessionChannel);
          p2pStreamStarted = true;
          setActivityLedMode(ActivityLedMode::Recording);
          Communication::transitionTo(
              Communication::State::Transmitting,
              "p2p_stream_started");
          break;
        }

        RtdbRequestService::setAudioPriorityActive(true);

        // VoIP writes need the persistent SSE TLS allocation released first.
        if (!pauseSseForRtdbRequests(CommunicationConfig::SSE_PAUSE_TIMEOUT_MS)) {
          Serial.println("[AUDIO_TX] session_start_abort reason=sse_pause_failed");
          resetSessionState(
              sessionStarted,
              sessionUsesRtdb,
              p2pStreamStarted,
              sessionId,
              sizeof(sessionId));
          Communication::transitionTo(
              Communication::State::Listening,
              "sse_pause_failed");
          resumeSseAfterRtdbRequests();
          break;
        }

        // A peer may have requested P2P while SSE was being stopped.
        if (Communication::transport() == Communication::Transport::P2p) {
          RtdbRequestService::setAudioPriorityActive(false);
          P2pAudio::startStream(sessionChannel);
          p2pStreamStarted = true;
          setActivityLedMode(ActivityLedMode::Recording);
          Communication::transitionTo(
              Communication::State::Transmitting,
              "peer_switched_during_session_start");
          resumeSseAfterRtdbRequests();
          break;
        }

        if (!ptt.rawPressed()) {
          resetSessionState(
              sessionStarted,
              sessionUsesRtdb,
              p2pStreamStarted,
              sessionId,
              sizeof(sessionId));
          Communication::transitionTo(
              Communication::State::Listening,
              "button_released_before_voip_start");
          resumeSseAfterRtdbRequests();
          break;
        }

        if (!RtdbRequestService::scheduleSessionStart(sessionChannel, sessionId)) {
          Serial.printf(
              "[AUDIO_TX] session_start_schedule_failed channel=%u session=%s\n",
              static_cast<unsigned int>(sessionChannel),
              sessionId);
          resetSessionState(
              sessionStarted,
              sessionUsesRtdb,
              p2pStreamStarted,
              sessionId,
              sizeof(sessionId));
          Communication::transitionTo(
              Communication::State::Listening,
              "session_start_schedule_failed");
          resumeSseAfterRtdbRequests();
          break;
        }

        sessionUsesRtdb = true;
        RtdbRequestService::setRecordingActive(true);
        setActivityLedMode(ActivityLedMode::Recording);
        Communication::transitionTo(
            Communication::State::Transmitting,
            "session_start_queued_recording_started");
        break;
      }

      case Communication::State::StartingSession:
        // The Listening branch completes setup before yielding.
        break;

      case Communication::State::Transmitting: {
        if (Communication::transport() == Communication::Transport::P2p) {
          if (sessionUsesRtdb && !rtdbTakeoverHandled) {
            RtdbRequestService::setRecordingActive(false);
            RtdbRequestService::requestAudioUploadAbort(
                "p2p_takeover_during_transmission");
            RtdbRequestService::discardPendingAudio(
                "p2p_takeover_during_transmission");
            rtdbTakeoverHandled = true;
          }
          RtdbRequestService::setAudioPriorityActive(false);

          if (!p2pStreamStarted) {
            P2pAudio::startStream(sessionChannel);
            p2pStreamStarted = true;
          }

          if ((guiCapturesPtt || !ptt.pressed()) &&
              (recordedAtLeastOneChunk ||
               (millis() - sessionStartedAtMs) >=
                   CommunicationConfig::INITIAL_CAPTURE_GRACE_MS)) {
            P2pAudio::endStream();
            p2pStreamStarted = false;
            setActivityLedMode(ActivityLedMode::Busy);
            Communication::transitionTo(
                Communication::State::EndingSession,
                recordedAtLeastOneChunk
                    ? "p2p_push_to_talk_released"
                    : "p2p_push_to_talk_released_no_chunk_timeout");
            break;
          }

          size_t samplesRead = 0;
          if (!AudioIO::readMicChunk(
                  p2pMicBuffer,
                  P2pAudioConfig::SAMPLES_PER_PACKET,
                  samplesRead)) {
            Serial.printf(
                "[P2P_TX] mic_read_fail streamSession=%s seq=%lu\n",
                sessionId,
                static_cast<unsigned long>(sequence));
            break;
          }

          recordedAtLeastOneChunk = true;
          sessionCapturedSamples += samplesRead;
          RuntimeStatistics::recordCapturedSamples(
              Communication::Transport::P2p,
              sessionChannel,
              samplesRead);
          const bool p2pPacketAccepted =
              P2pAudio::sendAudio(p2pMicBuffer, samplesRead);
          RuntimeStatistics::recordP2pPacketSendResult(p2pPacketAccepted);
          if (!p2pPacketAccepted) {
            ++droppedChunks;
            RuntimeStatistics::recordLocalDiscardedSamples(samplesRead);
            Serial.printf(
                "[P2P_TX] send_fail channel=%u seq=%lu dropped=%lu\n",
                static_cast<unsigned int>(sessionChannel),
                static_cast<unsigned long>(sequence),
                static_cast<unsigned long>(droppedChunks));
          }
          ++sequence;
          break;
        }

        if (voipFailureLimitExceeded()) {
          activateP2pTransport(
              "voip_failure_limit_exceeded_during_transmission",
              true);
          break;
        }

        if (!WifiConnection::isConnected()) {
          RtdbRequestService::setRecordingActive(false);
          // Preserve queued PCM while waiting for a new IP. If the failure
          // threshold is crossed meanwhile, Reconnecting changes to P2P.
          RtdbRequestService::requestAudioUploadAbort(
              "wifi_disconnected_during_transmission");
          resumeDrainAfterReconnect = true;
          setActivityLedMode(ActivityLedMode::Busy);
          Communication::transitionTo(
              Communication::State::Reconnecting,
              "wifi_disconnected_during_transmission");
          break;
        }
        if (RtdbRequestService::audioUploadFailed()) {
          RtdbRequestService::setRecordingActive(false);
          RtdbRequestService::discardPendingAudio("audio_upload_failed");
          setActivityLedMode(ActivityLedMode::Busy);
          drainStartedAtMs = millis();
          drainAbortRequested = false;
          Communication::transitionTo(
              Communication::State::DrainingUploads,
              "audio_upload_failed");
          break;
        }
        if ((guiCapturesPtt || !ptt.pressed()) &&
            (recordedAtLeastOneChunk ||
             (millis() - sessionStartedAtMs) >=
                 CommunicationConfig::INITIAL_CAPTURE_GRACE_MS)) {
          RtdbRequestService::setRecordingActive(false);
          setActivityLedMode(ActivityLedMode::Busy);
          drainStartedAtMs = millis();
          drainAbortRequested = false;
          Communication::transitionTo(
              Communication::State::DrainingUploads,
              recordedAtLeastOneChunk ? "push_to_talk_released"
                                      : "push_to_talk_released_no_chunk_timeout");
          break;
        }

        if (sequence >= RtdbBufferConfig::MAX_CHUNKS_PER_SESSION) {
          RtdbRequestService::setRecordingActive(false);
          setActivityLedMode(ActivityLedMode::Busy);
          drainStartedAtMs = millis();
          drainAbortRequested = false;
          Communication::transitionTo(
              Communication::State::DrainingUploads,
              "session_chunk_limit_reached");
          break;
        }

        uint8_t blockIndex = 0;
        int16_t* samples = nullptr;
        bool droppedOldest = false;
        if (!RtdbRequestService::acquireAudioBlock(
                blockIndex,
                samples,
                droppedOldest)) {
          size_t discardedSamples = 0;
          AudioIO::readMicChunk(
              droppedMicBuffer,
              AudioConfig::CHUNK_SAMPLES,
              discardedSamples);
          if (discardedSamples > 0) {
            sessionCapturedSamples += discardedSamples;
            RuntimeStatistics::recordCapturedSamples(
                Communication::Transport::Voip,
                sessionChannel,
                discardedSamples);
            RuntimeStatistics::recordLocalDiscardedSamples(discardedSamples);
            recordedAtLeastOneChunk = true;
          }
          ++droppedChunks;
          Serial.printf(
              "[AUDIO_TX] discard reason=all_blocks_unavailable dropped=%lu queueDepth=%lu\n",
              static_cast<unsigned long>(droppedChunks),
              static_cast<unsigned long>(RtdbRequestService::audioQueueDepth()));
          break;
        }
        if (droppedOldest) {
          ++droppedChunks;
        }

        size_t samplesRead = 0;
        if (!AudioIO::readMicChunk(
                samples,
                AudioConfig::CHUNK_SAMPLES,
                samplesRead)) {
          RtdbRequestService::releaseAudioBlock(blockIndex);
          Serial.printf(
              "[AUDIO_TX] mic_read_fail session=%s seq=%lu\n",
              sessionId,
              static_cast<unsigned long>(sequence));
          break;
        }
        sessionCapturedSamples += samplesRead;
        RuntimeStatistics::recordCapturedSamples(
            Communication::Transport::Voip,
            sessionChannel,
            samplesRead);
        if (!RtdbRequestService::submitAudioBlock(
                blockIndex,
                sessionChannel,
                sessionId,
                sequence,
                samplesRead,
                millis())) {
          ++droppedChunks;
        } else {
          recordedAtLeastOneChunk = true;
        }
        ++sequence;
        break;
      }

      case Communication::State::DrainingUploads:
        setActivityLedMode(ActivityLedMode::Busy);

        if (Communication::transport() == Communication::Transport::Voip &&
            voipFailureLimitExceeded()) {
          activateP2pTransport(
              "voip_failure_limit_exceeded_during_drain",
              true);
        }
        if (Communication::transport() == Communication::Transport::P2p) {
          RtdbRequestService::setRecordingActive(false);
          if (!rtdbTakeoverHandled) {
            RtdbRequestService::requestAudioUploadAbort(
                "p2p_takeover_during_drain");
            RtdbRequestService::discardPendingAudio(
                "p2p_takeover_during_drain");
            rtdbTakeoverHandled = true;
          }
          Communication::transitionTo(
              Communication::State::EndingSession,
              "transport_changed_to_p2p_during_drain");
          break;
        }

        if (!WifiConnection::isConnected()) {
          RtdbRequestService::requestAudioUploadAbort(
              "wifi_disconnected_during_drain");
          resumeDrainAfterReconnect = true;
          Serial.printf(
              "[AUDIO_TX] drain_paused reason=wifi_disconnected preservedDepth=%lu\n",
              static_cast<unsigned long>(RtdbRequestService::audioQueueDepth()));
          Communication::transitionTo(
              Communication::State::Reconnecting,
              "wifi_disconnected_during_drain");
          break;
        }
        if (RtdbRequestService::audioUploadFailed()) {
          RtdbRequestService::discardPendingAudio(
              "audio_upload_failed_during_drain");
        }
        if (!drainAbortRequested &&
            (millis() - drainStartedAtMs) >=
                CommunicationConfig::UPLOAD_DRAIN_TIMEOUT_MS) {
          RtdbRequestService::discardPendingAudio("drain_timeout");
          RtdbRequestService::requestAudioUploadAbort("drain_timeout");
          drainAbortRequested = true;
          Serial.printf(
              "[AUDIO_TX] drain_timeout session=%s queueDepth=%lu abortRequested=true\n",
              sessionId,
              static_cast<unsigned long>(RtdbRequestService::audioQueueDepth()));
        }
        if (RtdbRequestService::audioUploadsIdle()) {
          Communication::transitionTo(
              Communication::State::EndingSession,
              "upload_queue_empty");
        }
        break;

      case Communication::State::EndingSession: {
        RtdbRequestService::setRecordingActive(false);
        setActivityLedMode(ActivityLedMode::Busy);
        if (p2pStreamStarted) {
          P2pAudio::endStream();
          p2pStreamStarted = false;
        }

        const uint32_t lastSequence = sequence == 0 ? 0 : sequence - 1;
        if (sessionUsesRtdb && WifiConnection::isConnected()) {
          const RtdbRequestService::Result endResult =
              RtdbRequestService::endSession(
                  sessionChannel,
                  sessionId,
                  lastSequence,
                  RtdbRequestConfig::SYNC_OPERATION_TIMEOUT_MS);
          Serial.printf(
              "[AUDIO_TX] session_end channel=%u session=%s outcome=%s http=%d queued=%lu dropped=%lu durationMs=%lu transport=%s\n",
              static_cast<unsigned int>(sessionChannel),
              sessionId,
              RtdbRequestService::outcomeName(endResult.outcome),
              endResult.httpCode,
              static_cast<unsigned long>(sequence),
              static_cast<unsigned long>(droppedChunks),
              static_cast<unsigned long>(millis() - sessionStartedAtMs),
              Communication::transportName(Communication::transport()));
        } else if (sessionUsesRtdb) {
          Serial.printf(
              "[AUDIO_TX] session_end_skipped reason=wifi_disconnected channel=%u session=%s transport=%s\n",
              static_cast<unsigned int>(sessionChannel),
              sessionId,
              Communication::transportName(Communication::transport()));
        }

        // VoIP cleanup still waits for Wi-Fi. P2P must remain usable even when
        // the infrastructure network is unavailable.
        if (sessionUsesRtdb &&
            Communication::transport() == Communication::Transport::Voip &&
            !WifiConnection::isConnected()) {
          Communication::transitionTo(
              Communication::State::Reconnecting,
              "cleanup_interrupted_by_wifi");
          break;
        }

        RuntimeStatistics::recordCompletedSession(sessionCapturedSamples);
        resetSessionState(
            sessionStarted,
            sessionUsesRtdb,
            p2pStreamStarted,
            sessionId,
            sizeof(sessionId));
        sessionCapturedSamples = 0;
        resumeDrainAfterReconnect = false;
        rtdbTakeoverHandled = false;
        Communication::transitionTo(
            Communication::State::Listening,
            "session_cleanup_complete");
        resumeSseAfterRtdbRequests();
        break;
      }

      case Communication::State::Reconnecting:
        RtdbRequestService::setRecordingActive(false);

        if (Communication::transport() == Communication::Transport::Voip &&
            voipFailureLimitExceeded()) {
          activateP2pTransport(
              "voip_failure_limit_exceeded_while_reconnecting",
              true);
        }
        if (Communication::transport() == Communication::Transport::P2p) {
          if (!rtdbTakeoverHandled) {
            RtdbRequestService::requestAudioUploadAbort(
                "p2p_takeover_while_reconnecting");
            RtdbRequestService::discardPendingAudio(
                "p2p_takeover_while_reconnecting");
            rtdbTakeoverHandled = true;
          }
          RtdbRequestService::setAudioPriorityActive(false);
          resumeDrainAfterReconnect = false;

          if (sessionStarted && ptt.rawPressed()) {
            if (!p2pStreamStarted) {
              P2pAudio::startStream(sessionChannel);
              p2pStreamStarted = true;
            }
            setActivityLedMode(ActivityLedMode::Recording);
            Communication::transitionTo(
                Communication::State::Transmitting,
                "p2p_available_without_wifi");
          } else if (sessionStarted) {
            setActivityLedMode(ActivityLedMode::Busy);
            Communication::transitionTo(
                Communication::State::EndingSession,
                "p2p_takeover_after_ptt_release");
          } else {
            resetSessionState(
                sessionStarted,
                sessionUsesRtdb,
                p2pStreamStarted,
                sessionId,
                sizeof(sessionId));
            Communication::transitionTo(
                Communication::State::Listening,
                "p2p_available_without_wifi_idle");
            resumeSseAfterRtdbRequests();
          }
          break;
        }

        setActivityLedMode(
            sessionStarted ? ActivityLedMode::Busy : ActivityLedMode::Off);
        if (WifiConnection::isConnected()) {
          if (sessionStarted) {
            RtdbRequestService::resumeAudioUploadsAfterReconnect();
            const bool haveAudioToDrain =
                !RtdbRequestService::audioUploadsIdle();
            if (resumeDrainAfterReconnect && haveAudioToDrain) {
              drainStartedAtMs = millis();
              drainAbortRequested = false;
              resumeDrainAfterReconnect = false;
              Communication::transitionTo(
                  Communication::State::DrainingUploads,
                  "wifi_restored_resume_drain");
            } else {
              resumeDrainAfterReconnect = false;
              Communication::transitionTo(
                  Communication::State::EndingSession,
                  "wifi_restored_cleanup_required");
            }
          } else {
            resumeDrainAfterReconnect = false;
            resetSessionState(
                sessionStarted,
                sessionUsesRtdb,
                p2pStreamStarted,
                sessionId,
                sizeof(sessionId));
            Communication::transitionTo(
                Communication::State::Listening,
                "wifi_restored");
            resumeSseAfterRtdbRequests();
          }
        }
        break;
    }

    ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(CommunicationConfig::LOOP_DELAY_MS));
  }
}

bool initializePlaybackQueues() {
  playbackFreeQueue = xQueueCreate(PlaybackConfig::BLOCK_COUNT, sizeof(uint8_t));
  playbackReadyQueue = xQueueCreate(PlaybackConfig::QUEUE_LENGTH, sizeof(uint8_t));
  if (playbackFreeQueue == nullptr || playbackReadyQueue == nullptr) {
    return false;
  }
  for (uint8_t index = 0; index < PlaybackConfig::BLOCK_COUNT; ++index) {
    xQueueSend(playbackFreeQueue, &index, portMAX_DELAY);
  }
  return true;
}

bool startApplicationTasks() {
  if (xTaskCreatePinnedToCore(
          activityLedTask,
          "activity_led",
          ActivityLedConfig::TASK_STACK_BYTES,
          nullptr,
          ActivityLedConfig::TASK_PRIORITY,
          &activityLedTaskHandle,
          ActivityLedConfig::TASK_CORE) != pdPASS) {
    return false;
  }
  if (xTaskCreatePinnedToCore(
          playbackTask,
          "audio_playback",
          PlaybackConfig::TASK_STACK_BYTES,
          nullptr,
          PlaybackConfig::TASK_PRIORITY,
          &playbackTaskHandle,
          PlaybackConfig::TASK_CORE) != pdPASS) {
    return false;
  }
  if (xTaskCreatePinnedToCore(
          sseListenerTask,
          "rtdb_sse",
          SSE_TASK_STACK_BYTES,
          nullptr,
          SSE_TASK_PRIORITY,
          &sseTaskHandle,
          SSE_TASK_CORE) != pdPASS) {
    return false;
  }
  if (xTaskCreatePinnedToCore(
          communicationTask,
          "communication",
          CommunicationConfig::TASK_STACK_BYTES,
          nullptr,
          CommunicationConfig::TASK_PRIORITY,
          &communicationTaskHandle,
          CommunicationConfig::TASK_CORE) != pdPASS) {
    return false;
  }
  return true;
}

[[noreturn]] void haltWithError(const char* message) {
  Serial.println(message);
  for (;;) {
    delay(1000);
  }
}

bool timeSyncConfigured = false;
bool timeSyncReadyLogged = false;
uint32_t lastTimeSyncAttemptMs = 0;

bool systemClockIsSynchronized() {
  struct timeval tv {};
  return gettimeofday(&tv, nullptr) == 0 &&
         tv.tv_sec >= static_cast<time_t>(
             AvailabilityConfig::CLOCK_VALID_EPOCH_SECONDS);
}

void maintainTimeSynchronization() {
  if (systemClockIsSynchronized()) {
    if (!timeSyncReadyLogged) {
      struct timeval tv {};
      gettimeofday(&tv, nullptr);

      Serial.printf(
          "[TIME_SYNC] ready epochSeconds=%lld\n",
          static_cast<long long>(tv.tv_sec));

      timeSyncReadyLogged = true;
    }
    return;
  }

  timeSyncReadyLogged = false;

  if (!WifiConnection::isConnected()) {
    return;
  }

  const uint32_t nowMs = millis();
  if (timeSyncConfigured &&
      (nowMs - lastTimeSyncAttemptMs) <
          AvailabilityConfig::TIME_SYNC_CHECK_MS) {
    return;
  }

  lastTimeSyncAttemptMs = nowMs;
  timeSyncConfigured = true;

  configTzTime(
      AvailabilityConfig::TIME_ZONE_POSIX,
      AvailabilityConfig::NTP_SERVER_1,
      AvailabilityConfig::NTP_SERVER_2);

  Serial.printf(
      "[TIME_SYNC] configured timezone=%s servers=%s,%s\n",
      AvailabilityConfig::TIME_ZONE_NAME,
      AvailabilityConfig::NTP_SERVER_1,
      AvailabilityConfig::NTP_SERVER_2);
}

}  // namespace

void setup() {
  Serial.begin(115200);
  delay(500);

  // Configure the GUI button virtual grounds before their INPUT_PULLUP pins.
  pinMode(Pins::GUI_BUTTON_LEFT_VIRTUAL_GND, OUTPUT);
  digitalWrite(Pins::GUI_BUTTON_LEFT_VIRTUAL_GND, LOW);
  pinMode(Pins::GUI_BUTTON_RIGHT_VIRTUAL_GND, OUTPUT);
  digitalWrite(Pins::GUI_BUTTON_RIGHT_VIRTUAL_GND, LOW);

  pinMode(Pins::MAIN_BUTTON, INPUT_PULLUP);
  pinMode(Pins::RECORDING_LED, OUTPUT);
  digitalWrite(Pins::RECORDING_LED, LOW);

  RuntimeStatistics::begin();

  if (!Gui::appGui.begin()) {
    haltWithError("[ERROR] OLED display initialization failed");
  }
  Serial.println("[READY] GUI initialized");

  if (!WifiConnection::begin()) {
    haltWithError("[ERROR] Wi-Fi manager initialization failed");
  }
  if (!P2pAudio::begin()) {
    haltWithError("[ERROR] ESP-NOW P2P transport initialization failed");
  }
  if (!RtdbRequestService::begin()) {
    haltWithError("[ERROR] RTDB request service initialization failed");
  }
  RtdbAudioStream::begin();

  if (!AudioIO::beginMicrophone()) {
    haltWithError("[ERROR] Microphone initialization failed");
  }
  if (!AudioIO::beginSpeaker()) {
    haltWithError("[ERROR] Speaker initialization failed");
  }
  if (!initializePlaybackQueues()) {
    haltWithError("[ERROR] Playback queue initialization failed");
  }
  if (!startApplicationTasks()) {
    haltWithError("[ERROR] Application task initialization failed");
  }
  if (AvailabilityConfig::ENABLED && !AvailabilityService::begin()) {
    Serial.println("[Availability] Event-driven availability did not start");
  }

  Serial.printf(
      "[READY] Bidirectional half-duplex device=%s user=%s channel=%u method=%s wifi=%s autoP2p=%s p2pFailureLimit=%lu p2pAggregatePackets=%u p2pAggregateSamples=%u txBlocks=%u playbackBlocks=%u freeHeap=%lu\n",
      AppConfig::DEVICE_ID,
      AppConfig::USER_ID,
      static_cast<unsigned int>(currentListeningChannel()),
      Communication::transportName(Communication::transport()),
      WifiConnection::isEnabled() ? "enabled" : "disabled",
      Communication::automaticP2pDowngradeEnabled() ? "enabled" : "disabled",
      static_cast<unsigned long>(
          P2pAudioConfig::VOIP_FAILED_UPLOAD_ATTEMPT_LIMIT),
      static_cast<unsigned int>(
          P2pAudioConfig::PLAYBACK_PACKETS_PER_BLOCK),
      static_cast<unsigned int>(
          P2pAudioConfig::PLAYBACK_SAMPLES_PER_BLOCK),
      static_cast<unsigned int>(RtdbUploadConfig::TX_QUEUE_LENGTH),
      static_cast<unsigned int>(PlaybackConfig::BLOCK_COUNT),
      static_cast<unsigned long>(ESP.getFreeHeap()));
}

void loop() {
  processRemoteP2pSwitchRequest();
  syncCommunicationMethodToGui();
  syncRuntimeStatisticsNetworkCounters();

  // NVS writes are deferred until the local audio path is idle. This avoids a
  // periodic flash operation competing with microphone capture, upload drain,
  // or speaker playback. A due/forced save remains pending until this is safe.
  const bool playbackQueued =
      playbackReadyQueue != nullptr &&
      uxQueueMessagesWaiting(playbackReadyQueue) > 0;
  if (!Communication::isTransmitting() &&
      !isPlaybackInFlight() &&
      !playbackQueued) {
    RuntimeStatistics::maintainPersistence();
  }
  setGuiCommunicationBlocked(Gui::appGui.blocksCommunication());
  setGuiPttCaptureActive(Gui::appGui.consumesPttForNavigation());
  const Gui::UpdateResult guiResult = Gui::appGui.update();
  handleGuiResult(guiResult);
  setGuiCommunicationBlocked(Gui::appGui.blocksCommunication());
  setGuiPttCaptureActive(Gui::appGui.consumesPttForNavigation());
  RtdbAudioStream::loopMaintenance();
  if (AvailabilityConfig::ENABLED) {
    AvailabilityService::poll(
        currentListeningChannel(),
        Communication::isTransmitting(),
        Communication::transport() == Communication::Transport::P2p ? 1 : 0);
    syncAvailabilityToGui();
    maintainTimeSynchronization();
    maybeRunAvailabilityMaintenance();
  }
  delay(5);
}
