#include "app_config.h"
#include "audio_io.h"
#include "consts.h"
#include "gui.h"
#include "rtdb_audio_stream.h"
#include "wifi_connection.h"

#include <freertos/FreeRTOS.h>
#include <freertos/queue.h>
#include <freertos/task.h>
#include <string.h>

namespace {

int16_t pcmBuffer[AudioConfig::CHUNK_SAMPLES];
unsigned long lastStreamReconnectAttemptMs = 0;

// -----------------------------------------------------------------------------
// Talker state
// -----------------------------------------------------------------------------

struct TxQueueItem {
  enum class Type : uint8_t {
    AudioChunk,
    EndSession,
  };

  Type type = Type::AudioChunk;
  char sessionId[64] = {0};
  uint32_t seq = 0;
  uint32_t lastSeq = 0;
  size_t sampleCount = 0;
  int16_t samples[AudioConfig::CHUNK_SAMPLES];
  unsigned long recordedAtMs = 0;
  unsigned long sessionStartMs = 0;
  unsigned long recordingStopMs = 0;
  uint32_t queuedCount = 0;
  uint32_t droppedCount = 0;
};

QueueHandle_t txUploadQueue = nullptr;
TaskHandle_t txUploadTaskHandle = nullptr;
TxQueueItem uploadBatchItems[RtdbUploadConfig::UPLOAD_BATCH_MAX_CHUNKS];
RtdbUploadChunk uploadBatchRefs[RtdbUploadConfig::UPLOAD_BATCH_MAX_CHUNKS];

bool talkSessionActive = false;
char currentSessionId[64] = {0};
uint32_t txSeq = 0;
uint32_t txQueuedCount = 0;
uint32_t txDroppedCount = 0;
unsigned long talkSessionStartMs = 0;

volatile bool uploadSessionBusy = false;

bool isMainButtonPressed() {
  return digitalRead(Pins::MAIN_BUTTON) == ButtonLogic::PRESSED;
}

void copySessionId(char* dst, size_t dstSize, const char* src) {
  if (dst == nullptr || dstSize == 0) {
    return;
  }
  snprintf(dst, dstSize, "%s", (src == nullptr) ? "" : src);
}

void createSessionId() {
  snprintf(
      currentSessionId,
      sizeof(currentSessionId),
      "%s-%lu",
      AppConfig::DEVICE_ID,
      static_cast<unsigned long>(millis()));
}

uint32_t uploadQueueDepth() {
  if (txUploadQueue == nullptr) {
    return 0;
  }
  return static_cast<uint32_t>(uxQueueMessagesWaiting(txUploadQueue));
}

void uploadTask(void*) {
  TxQueueItem item;
  char taskSessionId[64] = {0};
  uint32_t uploadOkCount = 0;
  uint32_t uploadFailCount = 0;

  for (;;) {
    if (txUploadQueue == nullptr) {
      vTaskDelay(pdMS_TO_TICKS(100));
      continue;
    }

    if (xQueueReceive(txUploadQueue, &item, portMAX_DELAY) != pdTRUE) {
      continue;
    }

    if (item.type == TxQueueItem::Type::AudioChunk) {
      uploadBatchItems[0] = item;
      size_t batchCount = 1;

      TxQueueItem peeked;
      while (batchCount < RtdbUploadConfig::UPLOAD_BATCH_MAX_CHUNKS &&
             xQueuePeek(txUploadQueue, &peeked, 0) == pdTRUE &&
             peeked.type == TxQueueItem::Type::AudioChunk) {
        if (xQueueReceive(txUploadQueue, &uploadBatchItems[batchCount], 0) != pdTRUE) {
          break;
        }
        ++batchCount;
      }

      if (strncmp(taskSessionId, uploadBatchItems[0].sessionId, sizeof(taskSessionId)) != 0) {
        copySessionId(taskSessionId, sizeof(taskSessionId), uploadBatchItems[0].sessionId);
        uploadOkCount = 0;
        uploadFailCount = 0;
      }

      for (size_t i = 0; i < batchCount; ++i) {
        uploadBatchRefs[i].sessionId = uploadBatchItems[i].sessionId;
        uploadBatchRefs[i].seq = uploadBatchItems[i].seq;
        uploadBatchRefs[i].samples = uploadBatchItems[i].samples;
        uploadBatchRefs[i].sampleCount = uploadBatchItems[i].sampleCount;
      }

      Serial.printf(
          "[Talker][UPLOAD_TASK_DEQUEUE_BATCH] session=%s count=%u firstSeq=%lu lastSeq=%lu queueDepth=%lu firstAgeMs=%lu lastAgeMs=%lu freeHeap=%lu\n",
          uploadBatchItems[0].sessionId,
          static_cast<unsigned int>(batchCount),
          static_cast<unsigned long>(uploadBatchItems[0].seq),
          static_cast<unsigned long>(uploadBatchItems[batchCount - 1].seq),
          static_cast<unsigned long>(uploadQueueDepth()),
          static_cast<unsigned long>(millis() - uploadBatchItems[0].recordedAtMs),
          static_cast<unsigned long>(millis() - uploadBatchItems[batchCount - 1].recordedAtMs),
          static_cast<unsigned long>(ESP.getFreeHeap()));

      bool ok = false;
      for (uint8_t attempt = 1; attempt <= RtdbUploadConfig::UPLOAD_MAX_ATTEMPTS; ++attempt) {
        Serial.printf(
            "[Talker][UPLOAD_TASK_BATCH_ATTEMPT] session=%s attempt=%u/%u count=%u firstSeq=%lu lastSeq=%lu\n",
            uploadBatchItems[0].sessionId,
            static_cast<unsigned int>(attempt),
            static_cast<unsigned int>(RtdbUploadConfig::UPLOAD_MAX_ATTEMPTS),
            static_cast<unsigned int>(batchCount),
            static_cast<unsigned long>(uploadBatchItems[0].seq),
            static_cast<unsigned long>(uploadBatchItems[batchCount - 1].seq));

        ok = RtdbAudioStream::uploadPcmChunkBatch(
            AppConfig::DEFAULT_CHANNEL,
            uploadBatchRefs,
            batchCount);
        if (ok) {
          break;
        }
        if (attempt < RtdbUploadConfig::UPLOAD_MAX_ATTEMPTS) {
          delay(RtdbUploadConfig::UPLOAD_RETRY_DELAY_MS);
        }
      }

      if (ok) {
        uploadOkCount += batchCount;
      } else {
        uploadFailCount += batchCount;
      }

      Serial.printf(
          "[Talker][UPLOAD_TASK_BATCH_%s] session=%s count=%u firstSeq=%lu lastSeq=%lu uploadOk=%lu uploadFail=%lu queueDepth=%lu freeHeap=%lu\n",
          ok ? "OK" : "FAIL",
          uploadBatchItems[0].sessionId,
          static_cast<unsigned int>(batchCount),
          static_cast<unsigned long>(uploadBatchItems[0].seq),
          static_cast<unsigned long>(uploadBatchItems[batchCount - 1].seq),
          static_cast<unsigned long>(uploadOkCount),
          static_cast<unsigned long>(uploadFailCount),
          static_cast<unsigned long>(uploadQueueDepth()),
          static_cast<unsigned long>(ESP.getFreeHeap()));
      continue;
    }

    if (item.type == TxQueueItem::Type::EndSession) {
      Serial.printf(
          "[Talker][UPLOAD_TASK_END_REQUEST] session=%s queued=%lu dropped=%lu lastSeq=%lu uploadOk=%lu uploadFail=%lu queueDepth=%lu recordingDurationMs=%lu uploadLagAfterReleaseMs=%lu\n",
          item.sessionId,
          static_cast<unsigned long>(item.queuedCount),
          static_cast<unsigned long>(item.droppedCount),
          static_cast<unsigned long>(item.lastSeq),
          static_cast<unsigned long>(uploadOkCount),
          static_cast<unsigned long>(uploadFailCount),
          static_cast<unsigned long>(uploadQueueDepth()),
          static_cast<unsigned long>(item.recordingStopMs - item.sessionStartMs),
          static_cast<unsigned long>(millis() - item.recordingStopMs));

      const bool ended = RtdbAudioStream::endTransmission(
          AppConfig::DEFAULT_CHANNEL,
          item.sessionId,
          item.lastSeq);

      Serial.printf(
          "[Talker][SESSION_UPLOAD_DONE] session=%s endMeta=%s queued=%lu dropped=%lu uploadOk=%lu uploadFail=%lu totalDurationMs=%lu\n",
          item.sessionId,
          ended ? "ok" : "failed",
          static_cast<unsigned long>(item.queuedCount),
          static_cast<unsigned long>(item.droppedCount),
          static_cast<unsigned long>(uploadOkCount),
          static_cast<unsigned long>(uploadFailCount),
          static_cast<unsigned long>(millis() - item.sessionStartMs));

      uploadSessionBusy = false;
      taskSessionId[0] = '\0';
      uploadOkCount = 0;
      uploadFailCount = 0;
    }
  }
}

void handleGuiResult(const Gui::UpdateResult& guiResult) {
  if (guiResult.hasSelection) {
    switch (guiResult.selectedScreen) {
      case Gui::ScreenId::SelectChannels:
        Serial.println("[GUI] Opened: Select channels");
        break;
      case Gui::ScreenId::ViewUsers:
        Serial.println("[GUI] Selected: View users");
        break;
      case Gui::ScreenId::ViewStatistics:
        Serial.println("[GUI] Selected: View statistics");
        break;
      case Gui::ScreenId::ViewSettings:
        Serial.println("[GUI] Selected: View settings");
        break;
      case Gui::ScreenId::MainMenu:
      case Gui::ScreenId::ChannelJoinPreview:
      default:
        break;
    }
  }

  if (guiResult.hasChannelSelection) {
    Serial.print("[GUI] Selected channel ");
    Serial.print(guiResult.selectedChannel);
    Serial.println(" - RTDB audio still uses AppConfig::DEFAULT_CHANNEL for now");
  }
}

void setupTalker() {
  pinMode(Pins::MAIN_BUTTON, INPUT_PULLUP);
  pinMode(Pins::RECORDING_LED, OUTPUT);
  digitalWrite(Pins::RECORDING_LED, LOW);

  txUploadQueue = xQueueCreate(
      RtdbUploadConfig::TX_QUEUE_LENGTH,
      sizeof(TxQueueItem));

  if (txUploadQueue == nullptr) {
    Serial.println("[ERROR] Failed creating RTDB upload queue");
    while (true) {
      delay(1000);
    }
  }

  const BaseType_t taskCreated = xTaskCreatePinnedToCore(
      uploadTask,
      "rtdb_upload",
      RtdbUploadConfig::UPLOAD_TASK_STACK_BYTES,
      nullptr,
      RtdbUploadConfig::UPLOAD_TASK_PRIORITY,
      &txUploadTaskHandle,
      RtdbUploadConfig::UPLOAD_TASK_CORE);

  if (taskCreated != pdPASS) {
    Serial.println("[ERROR] Failed creating RTDB upload task");
    while (true) {
      delay(1000);
    }
  }

  if (!AudioIO::beginMicrophone()) {
    Serial.println("[ERROR] Microphone initialization failed");
    while (true) {
      delay(1000);
    }
  }

  Serial.printf(
      "[READY] RTDB buffered async talker initialized queueLength=%u batchMax=%u itemBytes=%u queueBytesApprox=%lu freeHeap=%lu\n",
      static_cast<unsigned int>(RtdbUploadConfig::TX_QUEUE_LENGTH),
      static_cast<unsigned int>(RtdbUploadConfig::UPLOAD_BATCH_MAX_CHUNKS),
      static_cast<unsigned int>(sizeof(TxQueueItem)),
      static_cast<unsigned long>(sizeof(TxQueueItem) * RtdbUploadConfig::TX_QUEUE_LENGTH),
      static_cast<unsigned long>(ESP.getFreeHeap()));
}

void setupListener() {
  if (!AudioIO::beginSpeaker()) {
    Serial.println("[ERROR] Speaker initialization failed");
    while (true) {
      delay(1000);
    }
  }

  if (!RtdbAudioStream::startListening(AppConfig::DEFAULT_CHANNEL)) {
    Serial.println("[RTDB] Initial stream connection failed; loop will retry");
  }

  Serial.println("[READY] RTDB buffered listener initialized");
}

void beginTalkSession() {
  if (txUploadQueue == nullptr) {
    Serial.println("[Talker][SESSION_START_BLOCKED] reason=upload_queue_missing");
    return;
  }

  if (uploadSessionBusy) {
    Serial.printf(
        "[Talker][SESSION_START_BLOCKED] reason=previous_upload_in_progress queueDepth=%lu\n",
        static_cast<unsigned long>(uploadQueueDepth()));
    return;
  }

  createSessionId();
  txSeq = 0;
  txQueuedCount = 0;
  txDroppedCount = 0;
  talkSessionStartMs = millis();

  xQueueReset(txUploadQueue);

  Serial.printf(
      "[Talker][SESSION_START_REQUEST] session=%s channel=%u chunkMs=%u samplesPerChunk=%u\n",
      currentSessionId,
      static_cast<unsigned int>(AppConfig::DEFAULT_CHANNEL),
      static_cast<unsigned int>(AudioConfig::CHUNK_MS),
      static_cast<unsigned int>(AudioConfig::CHUNK_SAMPLES));

  if (!RtdbAudioStream::beginTransmission(AppConfig::DEFAULT_CHANNEL, currentSessionId)) {
    Serial.println("[Talker] Failed starting RTDB session");
    currentSessionId[0] = '\0';
    talkSessionActive = false;
    uploadSessionBusy = false;
    return;
  }

  uploadSessionBusy = true;
  talkSessionActive = true;
  Serial.printf("[Talker][SESSION_STARTED] session=%s\n", currentSessionId);
}

void enqueueRecordedChunk(const int16_t* samples, size_t sampleCount) {
  if (!talkSessionActive || txUploadQueue == nullptr) {
    return;
  }

  TxQueueItem item;
  item.type = TxQueueItem::Type::AudioChunk;
  copySessionId(item.sessionId, sizeof(item.sessionId), currentSessionId);
  item.seq = txSeq++;
  item.sampleCount = sampleCount;
  item.recordedAtMs = millis();
  memcpy(item.samples, samples, sampleCount * sizeof(int16_t));

  const BaseType_t queued = xQueueSend(txUploadQueue, &item, 0);
  if (queued != pdTRUE) {
    ++txDroppedCount;
    Serial.printf(
        "[Talker][QUEUE_FULL_DROP] session=%s seq=%lu samples=%u queueDepth=%lu dropped=%lu\n",
        currentSessionId,
        static_cast<unsigned long>(item.seq),
        static_cast<unsigned int>(sampleCount),
        static_cast<unsigned long>(uploadQueueDepth()),
        static_cast<unsigned long>(txDroppedCount));
    return;
  }

  ++txQueuedCount;
  Serial.printf(
      "[Talker][QUEUE_ENQUEUE_OK] session=%s seq=%lu samples=%u queued=%lu dropped=%lu queueDepth=%lu\n",
      currentSessionId,
      static_cast<unsigned long>(item.seq),
      static_cast<unsigned int>(sampleCount),
      static_cast<unsigned long>(txQueuedCount),
      static_cast<unsigned long>(txDroppedCount),
      static_cast<unsigned long>(uploadQueueDepth()));
}

void endTalkSession() {
  if (!talkSessionActive) {
    return;
  }

  const unsigned long recordingStopMs = millis();
  const uint32_t lastSeq = (txSeq == 0) ? 0 : (txSeq - 1);

  Serial.printf(
      "[Talker][SESSION_RECORDING_STOPPED] session=%s recordingDurationMs=%lu queued=%lu dropped=%lu lastSeq=%lu queueDepth=%lu\n",
      currentSessionId,
      static_cast<unsigned long>(recordingStopMs - talkSessionStartMs),
      static_cast<unsigned long>(txQueuedCount),
      static_cast<unsigned long>(txDroppedCount),
      static_cast<unsigned long>(lastSeq),
      static_cast<unsigned long>(uploadQueueDepth()));

  TxQueueItem endItem;
  endItem.type = TxQueueItem::Type::EndSession;
  copySessionId(endItem.sessionId, sizeof(endItem.sessionId), currentSessionId);
  endItem.lastSeq = lastSeq;
  endItem.sessionStartMs = talkSessionStartMs;
  endItem.recordingStopMs = recordingStopMs;
  endItem.queuedCount = txQueuedCount;
  endItem.droppedCount = txDroppedCount;

  // This is intentionally blocking after release: the END marker must be placed
  // after all queued audio chunks so /meta.active=false is written only after the
  // upload task has flushed the buffered audio.
  xQueueSend(txUploadQueue, &endItem, portMAX_DELAY);

  Serial.printf(
      "[Talker][SESSION_END_QUEUED] session=%s queueDepth=%lu\n",
      currentSessionId,
      static_cast<unsigned long>(uploadQueueDepth()));

  talkSessionActive = false;
  currentSessionId[0] = '\0';
}

void loopTalker() {
  const bool pressed = isMainButtonPressed();
  digitalWrite(Pins::RECORDING_LED, pressed ? HIGH : LOW);

  if (!pressed) {
    endTalkSession();
    delay(2);
    return;
  }

  if (!talkSessionActive) {
    beginTalkSession();
    if (!talkSessionActive) {
      delay(50);
      return;
    }
  }

  size_t samplesRead = 0;
  if (!AudioIO::readMicChunk(pcmBuffer, AudioConfig::CHUNK_SAMPLES, samplesRead)) {
    Serial.printf(
        "[Talker][MIC_READ_FAIL] session=%s seq=%lu\n",
        currentSessionId,
        static_cast<unsigned long>(txSeq));
    return;
  }

  Serial.printf(
      "[Talker][MIC_READ_OK] session=%s seq=%lu samples=%u\n",
      currentSessionId,
      static_cast<unsigned long>(txSeq),
      static_cast<unsigned int>(samplesRead));

  enqueueRecordedChunk(pcmBuffer, samplesRead);
}

void loopListener() {
  if (!RtdbAudioStream::isListening()) {
    const unsigned long now = millis();
    if ((now - lastStreamReconnectAttemptMs) > RtdbHttpConfig::STREAM_RECONNECT_INTERVAL_MS) {
      lastStreamReconnectAttemptMs = now;
      Serial.printf("[Listener][STREAM_RECONNECT] channel=%u\n", static_cast<unsigned int>(AppConfig::DEFAULT_CHANNEL));
      RtdbAudioStream::startListening(AppConfig::DEFAULT_CHANNEL);
    }
    return;
  }

  RtdbPcmChunk chunk;
  if (RtdbAudioStream::pollListening(pcmBuffer, AudioConfig::CHUNK_SAMPLES, chunk)) {
    Serial.print("[Listener] Playing ");
    Serial.print(chunk.sessionId);
    Serial.print(" seq=");
    Serial.print(chunk.seq);
    Serial.print(", samples=");
    Serial.println(chunk.sampleCount);
    const bool played = AudioIO::playPcm16(pcmBuffer, chunk.sampleCount);
    Serial.printf("[Listener][PLAY_%s] session=%s seq=%lu samples=%u\n",
                  played ? "OK" : "FAIL",
                  chunk.sessionId.c_str(),
                  static_cast<unsigned long>(chunk.seq),
                  static_cast<unsigned int>(chunk.sampleCount));
  }
}

}  // namespace

void setup() {
  Serial.begin(115200);
  delay(500);

  if (!Gui::appGui.begin()) {
    Serial.println("[ERROR] OLED display initialization failed");
    while (true) {
      delay(1000);
    }
  }
  Serial.println("[READY] GUI initialized");

  WifiConnection::begin();
  if (!RtdbAudioStream::begin()) {
    Serial.println("[RTDB] Initial RTDB setup failed; loop will keep trying where possible");
  }

  if (AppConfig::ROLE == AppConfig::DeviceRole::Talker) {
    setupTalker();
  } else {
    setupListener();
  }
}

void loop() {
  const Gui::UpdateResult guiResult = Gui::appGui.update();
  handleGuiResult(guiResult);

  WifiConnection::ensureConnected();
  RtdbAudioStream::loopMaintenance();

  if (AppConfig::ROLE == AppConfig::DeviceRole::Talker) {
    loopTalker();
  } else {
    loopListener();
  }
}
