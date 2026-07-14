#include "app_config.h"
#include "availability_service.h"
#include "audio_io.h"
#include "communication_state.h"
#include "consts.h"
#include "gui.h"
#include "rtdb_audio_stream.h"
#include "rtdb_request_service.h"
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

constexpr uint32_t SSE_TASK_STACK_BYTES = 16384;
constexpr uint8_t SSE_TASK_PRIORITY = 2;
constexpr uint8_t SSE_TASK_CORE = 0;
constexpr uint32_t SSE_IDLE_DELAY_MS = 2;

struct PlaybackBlock {
  int16_t samples[AudioConfig::CHUNK_SAMPLES];
  size_t sampleCount = 0;
  uint32_t sequence = 0;
  char sessionId[64] = {0};
  char deviceId[48] = {0};
};

PlaybackBlock playbackBlocks[PlaybackConfig::BLOCK_COUNT];
QueueHandle_t playbackFreeQueue = nullptr;
QueueHandle_t playbackReadyQueue = nullptr;
TaskHandle_t playbackTaskHandle = nullptr;
TaskHandle_t sseTaskHandle = nullptr;
TaskHandle_t communicationTaskHandle = nullptr;

portMUX_TYPE playbackMux = portMUX_INITIALIZER_UNLOCKED;
portMUX_TYPE channelMux = portMUX_INITIALIZER_UNLOCKED;
bool playbackInFlight = false;
uint8_t listeningChannel = AppConfig::DEFAULT_CHANNEL;
uint8_t pendingChannel = AppConfig::DEFAULT_CHANNEL;
bool channelChangePending = false;
portMUX_TYPE sseControlMux = portMUX_INITIALIZER_UNLOCKED;
bool ssePauseRequested = false;
bool ssePauseAcknowledged = false;

AvailabilityService::UserStatus availabilitySnapshot[AvailabilityConfig::USER_COUNT];
int16_t droppedMicBuffer[AudioConfig::CHUNK_SAMPLES];

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

uint8_t currentListeningChannel() {
  portENTER_CRITICAL(&channelMux);
  const uint8_t channel = listeningChannel;
  portEXIT_CRITICAL(&channelMux);
  return channel;
}

void requestLogicalChannel(uint8_t channel) {
  if (channel < 1 || channel > Gui::CHANNEL_COUNT) {
    return;
  }
  portENTER_CRITICAL(&channelMux);
  pendingChannel = channel;
  channelChangePending = true;
  portEXIT_CRITICAL(&channelMux);
  Serial.printf(
      "[CHANNEL] requested=%u state=%s\n",
      static_cast<unsigned int>(channel),
      Communication::stateName(Communication::state()));
  if (communicationTaskHandle != nullptr) {
    xTaskNotifyGive(communicationTaskHandle);
  }
}

void applyPendingLogicalChannel() {
  bool changed = false;
  uint8_t channel = 0;
  portENTER_CRITICAL(&channelMux);
  if (channelChangePending) {
    listeningChannel = pendingChannel;
    channel = listeningChannel;
    channelChangePending = false;
    changed = true;
  }
  portEXIT_CRITICAL(&channelMux);
  if (!changed) {
    return;
  }
  Serial.printf("[CHANNEL] active=%u\n", static_cast<unsigned int>(channel));
  if (sseTaskHandle != nullptr) {
    xTaskNotifyGive(sseTaskHandle);
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
        availabilitySnapshot[index].voipAvailable,
        availabilitySnapshot[index].p2pAvailable,
        availabilitySnapshot[index].voipAgeSeconds,
        availabilitySnapshot[index].p2pAgeSeconds);
  }
}

void handleGuiResult(const Gui::UpdateResult& result) {
  if (result.hasSelection) {
    Serial.printf(
        "[GUI] selectedScreen=%u\n",
        static_cast<unsigned int>(result.selectedScreen));
  }
  if (result.hasChannelSelection) {
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
    if (!Communication::permitsPlayback()) {
      Serial.printf(
          "[AUDIO_RX] discard reason=transmitting session=%s seq=%lu\n",
          block.sessionId,
          static_cast<unsigned long>(block.sequence));
      xQueueSend(playbackFreeQueue, &blockIndex, portMAX_DELAY);
      continue;
    }

    setPlaybackInFlight(true);
    if (!Communication::permitsPlayback()) {
      setPlaybackInFlight(false);
      xQueueSend(playbackFreeQueue, &blockIndex, portMAX_DELAY);
      continue;
    }

    const bool played = AudioIO::playPcm16(block.samples, block.sampleCount);
    setPlaybackInFlight(false);
    Serial.printf(
        "[AUDIO_RX] playback success=%s source=%s session=%s seq=%lu samples=%u\n",
        played ? "true" : "false",
        block.deviceId,
        block.sessionId,
        static_cast<unsigned long>(block.sequence),
        static_cast<unsigned int>(block.sampleCount));
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

  for (;;) {
    if (isSsePauseRequested()) {
    if (RtdbAudioStream::isListening()) {
      RtdbAudioStream::stopListening();
    }

    // Force an immediate reconnect after suspension ends.
    connectedChannel = 0;
    lastReconnectAttemptMs = 0;

    setSsePauseAcknowledged(true);

    ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(50));
    continue;
  }

  setSsePauseAcknowledged(false);

    if (!WifiConnection::isConnected()) {
      if (RtdbAudioStream::isListening()) {
        RtdbAudioStream::stopListening();
      }
      ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(250));
      continue;
    }

    const uint8_t requestedChannel = currentListeningChannel();
    const bool channelChanged = connectedChannel != requestedChannel;
    if (channelChanged && RtdbAudioStream::isListening()) {
      RtdbAudioStream::stopListening();
    }

    if (!RtdbAudioStream::isListening()) {
      const uint32_t now = millis();
      if (channelChanged ||
          (now - lastReconnectAttemptMs) >= RtdbHttpConfig::STREAM_RECONNECT_INTERVAL_MS) {
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
    snprintf(scratch.sessionId, sizeof(scratch.sessionId), "%s", chunk.sessionId.c_str());
    snprintf(scratch.deviceId, sizeof(scratch.deviceId), "%s", chunk.deviceId.c_str());

    if (chunk.deviceId == AppConfig::DEVICE_ID) {
      Serial.printf(
          "[AUDIO_RX] discard reason=self session=%s seq=%lu\n",
          scratch.sessionId,
          static_cast<unsigned long>(scratch.sequence));
      continue;
    }
    if (!Communication::permitsPlayback()) {
      Serial.printf(
          "[AUDIO_RX] discard reason=%s source=%s session=%s seq=%lu\n",
          Communication::isTransmitting() ? "transmitting" : "half_duplex",
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
    char* sessionId,
    size_t sessionIdSize) {
  sessionStarted = false;
  if (sessionId != nullptr && sessionIdSize > 0) {
    sessionId[0] = '\0';
  }
  RtdbRequestService::setRecordingActive(false);
  RtdbRequestService::setAudioPriorityActive(false);
  RtdbRequestService::clearAudioUploadFailure();
  digitalWrite(Pins::RECORDING_LED, LOW);
}

void communicationTask(void*) {
  WifiConnection::registerConnectedObserver(
      wakeTaskOnWifiRestore,
      xTaskGetCurrentTaskHandle());

  DebouncedPtt ptt;
  ptt.begin();
  bool pttAttemptArmed = !ptt.rawPressed();
  bool sessionStarted = false;
  uint8_t sessionChannel = AppConfig::DEFAULT_CHANNEL;
  char sessionId[64] = {0};
  uint32_t sequence = 0;
  uint32_t droppedChunks = 0;
  uint32_t sessionStartedAtMs = 0;
  uint32_t drainStartedAtMs = 0;
  bool recordedAtLeastOneChunk = false;

  Communication::begin();

  for (;;) {
    ptt.update();
    if (!ptt.rawPressed()) {
      pttAttemptArmed = true;
    }

    switch (Communication::state()) {
      case Communication::State::Listening: {
        applyPendingLogicalChannel();
        if (!WifiConnection::isConnected()) {
          Communication::transitionTo(
              Communication::State::Reconnecting,
              "wifi_disconnected_idle");
          break;
        }
        if (!ptt.pressed() || !pttAttemptArmed) {
          break;
        }

        pttAttemptArmed = false;
        RtdbRequestService::setAudioPriorityActive(true);
        Communication::transitionTo(
            Communication::State::StartingSession,
            "push_to_talk_pressed");

        // The transmitter intentionally stops listening while it talks. This
        // also releases the persistent SSE TLS buffers before short REST calls.
        if (!pauseSseForRtdbRequests(1500)) {
          Serial.println("[AUDIO_TX] session_start_abort reason=sse_pause_failed");
          resetSessionState(sessionStarted, sessionId, sizeof(sessionId));
          Communication::transitionTo(
              Communication::State::Listening,
              "sse_pause_failed");
          resumeSseAfterRtdbRequests();
          break;
        }

        sessionChannel = currentListeningChannel();
        snprintf(
            sessionId,
            sizeof(sessionId),
            "%s-%08lx-%lu",
            AppConfig::DEVICE_ID,
            static_cast<unsigned long>(esp_random()),
            static_cast<unsigned long>(millis()));

        if (!ptt.rawPressed()) {
          resetSessionState(sessionStarted, sessionId, sizeof(sessionId));
          Communication::transitionTo(
              Communication::State::Listening,
              "button_released_before_session_start");
          resumeSseAfterRtdbRequests();
          break;
        }

        if (!waitForPlaybackToStop()) {
          resetSessionState(sessionStarted, sessionId, sizeof(sessionId));
          Communication::transitionTo(
              Communication::State::Listening,
              "playback_did_not_stop");
          resumeSseAfterRtdbRequests();
          break;
        }

        // Mark cleanup as required before the request. Firebase may apply the
        // PATCH even if the response is lost or the caller times out.
        sessionStarted = true;
        sessionStartedAtMs = millis();
        sequence = 0;
        droppedChunks = 0;
        recordedAtLeastOneChunk = false;

        RtdbRequestService::clearAudioUploadFailure();
        if (!RtdbRequestService::scheduleSessionStart(sessionChannel, sessionId)) {
          Serial.printf(
              "[AUDIO_TX] session_start_schedule_failed channel=%u session=%s\n",
              static_cast<unsigned int>(sessionChannel),
              sessionId);
          resetSessionState(sessionStarted, sessionId, sizeof(sessionId));
          Communication::transitionTo(
              Communication::State::Listening,
              "session_start_schedule_failed");
          resumeSseAfterRtdbRequests();
          break;
        }

        // Start capturing immediately. The RTDB request task processes the
        // queued SESSION_START first, while this task fills the fixed PCM queue.
        // This preserves speech made during the TLS request and prevents a quick
        // PTT press from becoming an active=true/active=false empty session.
        RtdbRequestService::setRecordingActive(true);
        digitalWrite(Pins::RECORDING_LED, HIGH);
        Communication::transitionTo(
            Communication::State::Transmitting,
            "session_start_queued_recording_started");
        break;
      }

      case Communication::State::StartingSession:
        // This state is transient: the REST request is queued and microphone
        // capture starts immediately in the Listening branch.
        break;

      case Communication::State::Transmitting: {
        if (!WifiConnection::isConnected()) {
          RtdbRequestService::setRecordingActive(false);
          RtdbRequestService::discardPendingAudio("wifi_disconnected");
          digitalWrite(Pins::RECORDING_LED, LOW);
          Communication::transitionTo(
              Communication::State::Reconnecting,
              "wifi_disconnected_during_transmission");
          break;
        }
        if (RtdbRequestService::audioUploadFailed()) {
          RtdbRequestService::setRecordingActive(false);
          RtdbRequestService::discardPendingAudio("audio_upload_failed");
          digitalWrite(Pins::RECORDING_LED, LOW);
          drainStartedAtMs = millis();
          Communication::transitionTo(
              Communication::State::DrainingUploads,
              "audio_upload_failed");
          break;
        }
        if (!ptt.pressed() &&
            (recordedAtLeastOneChunk ||
             (millis() - sessionStartedAtMs) >=
                 CommunicationConfig::INITIAL_CAPTURE_GRACE_MS)) {
          RtdbRequestService::setRecordingActive(false);
          digitalWrite(Pins::RECORDING_LED, LOW);
          drainStartedAtMs = millis();
          Communication::transitionTo(
              Communication::State::DrainingUploads,
              recordedAtLeastOneChunk ? "push_to_talk_released"
                                      : "push_to_talk_released_no_chunk_timeout");
          break;
        }

        if (sequence >= RtdbBufferConfig::MAX_CHUNKS_PER_SESSION) {
          RtdbRequestService::setRecordingActive(false);
          digitalWrite(Pins::RECORDING_LED, LOW);
          drainStartedAtMs = millis();
          Communication::transitionTo(
              Communication::State::DrainingUploads,
              "session_chunk_limit_reached");
          break;
        }

        uint8_t blockIndex = 0;
        int16_t* samples = nullptr;
        if (!RtdbRequestService::acquireAudioBlock(blockIndex, samples)) {
          size_t discardedSamples = 0;
          AudioIO::readMicChunk(
              droppedMicBuffer,
              AudioConfig::CHUNK_SAMPLES,
              discardedSamples);
          ++droppedChunks;
          Serial.printf(
              "[AUDIO_TX] discard reason=queue_full dropped=%lu queueDepth=%lu\n",
              static_cast<unsigned long>(droppedChunks),
              static_cast<unsigned long>(RtdbRequestService::audioQueueDepth()));
          break;
        }

        size_t samplesRead = 0;
        if (!AudioIO::readMicChunk(samples, AudioConfig::CHUNK_SAMPLES, samplesRead)) {
          RtdbRequestService::releaseAudioBlock(blockIndex);
          Serial.printf(
              "[AUDIO_TX] mic_read_fail session=%s seq=%lu\n",
              sessionId,
              static_cast<unsigned long>(sequence));
          break;
        }
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
        if (!WifiConnection::isConnected()) {
          RtdbRequestService::discardPendingAudio("wifi_disconnected_during_drain");
          Communication::transitionTo(
              Communication::State::Reconnecting,
              "wifi_disconnected_during_drain");
          break;
        }
        if (RtdbRequestService::audioUploadFailed()) {
          RtdbRequestService::discardPendingAudio("audio_upload_failed_during_drain");
        }
        if ((millis() - drainStartedAtMs) >=
            CommunicationConfig::UPLOAD_DRAIN_TIMEOUT_MS) {
          RtdbRequestService::discardPendingAudio("drain_timeout");
          Serial.printf(
              "[AUDIO_TX] drain_timeout session=%s queueDepth=%lu\n",
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
        digitalWrite(Pins::RECORDING_LED, LOW);
        const uint32_t lastSequence = sequence == 0 ? 0 : sequence - 1;

        if (sessionStarted && WifiConnection::isConnected()) {
          const RtdbRequestService::Result endResult =
              RtdbRequestService::endSession(
                  sessionChannel,
                  sessionId,
                  lastSequence,
                  RtdbRequestConfig::SYNC_OPERATION_TIMEOUT_MS);
          Serial.printf(
              "[AUDIO_TX] session_end channel=%u session=%s outcome=%s http=%d queued=%lu dropped=%lu durationMs=%lu\n",
              static_cast<unsigned int>(sessionChannel),
              sessionId,
              RtdbRequestService::outcomeName(endResult.outcome),
              endResult.httpCode,
              static_cast<unsigned long>(sequence),
              static_cast<unsigned long>(droppedChunks),
              static_cast<unsigned long>(millis() - sessionStartedAtMs));
        }

        if (!WifiConnection::isConnected()) {
          Communication::transitionTo(
              Communication::State::Reconnecting,
              "cleanup_interrupted_by_wifi");
          break;
        }

        resetSessionState(sessionStarted, sessionId, sizeof(sessionId));
        Communication::transitionTo(
            Communication::State::Listening,
            "session_cleanup_complete");
        resumeSseAfterRtdbRequests();
        break;
      }

      case Communication::State::Reconnecting:
        RtdbRequestService::setRecordingActive(false);
        digitalWrite(Pins::RECORDING_LED, LOW);
        if (WifiConnection::isConnected()) {
          Communication::transitionTo(
              sessionStarted ? Communication::State::EndingSession
                             : Communication::State::Listening,
              sessionStarted ? "wifi_restored_cleanup_required"
                             : "wifi_restored");
          if (!sessionStarted) {
            resetSessionState(sessionStarted, sessionId, sizeof(sessionId));
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

  pinMode(Pins::MAIN_BUTTON, INPUT_PULLUP);
  pinMode(Pins::RECORDING_LED, OUTPUT);
  digitalWrite(Pins::RECORDING_LED, LOW);

  if (!Gui::appGui.begin()) {
    haltWithError("[ERROR] OLED display initialization failed");
  }
  Serial.println("[READY] GUI initialized");

  if (!WifiConnection::begin()) {
    haltWithError("[ERROR] Wi-Fi manager initialization failed");
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
    Serial.println("[Availability] Background availability task did not start");
  }

  Serial.printf(
      "[READY] Bidirectional half-duplex device=%s user=%s channel=%u txBlocks=%u playbackBlocks=%u freeHeap=%lu\n",
      AppConfig::DEVICE_ID,
      AppConfig::USER_ID,
      static_cast<unsigned int>(AppConfig::DEFAULT_CHANNEL),
      static_cast<unsigned int>(RtdbUploadConfig::TX_QUEUE_LENGTH),
      static_cast<unsigned int>(PlaybackConfig::BLOCK_COUNT),
      static_cast<unsigned long>(ESP.getFreeHeap()));
}

void loop() {
  const Gui::UpdateResult guiResult = Gui::appGui.update();
  handleGuiResult(guiResult);
  RtdbAudioStream::loopMaintenance();
  if (AvailabilityConfig::ENABLED) {
    syncAvailabilityToGui();
    maintainTimeSynchronization();
  }
  delay(5);
}
