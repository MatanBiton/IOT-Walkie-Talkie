#define ENABLE_DATABASE
// FirebaseClient stores the complete prepared header and payload in every
// asynchronous queue slot.  This project permits only the single SSE task; all
// writes are synchronous in the dedicated RTDB worker task.
#define FIREBASE_ASYNC_QUEUE_LIMIT 1

#include "rtdb_request_service.h"

#include <FirebaseClient.h>
#include <WiFiClientSecure.h>
#include <esp_heap_caps.h>
#include <freertos/FreeRTOS.h>
#include <freertos/queue.h>
#include <freertos/semphr.h>
#include <freertos/task.h>
#include <mbedtls/base64.h>
#include <string.h>

#include "app_config.h"
#include "rtdb_audio_stream.h"
#include "wifi_connection.h"

namespace {

enum class RequestType : uint8_t {
  SessionStart,
  AudioBatch,
  SessionEnd,
  StreamStart,
  StreamStop,
  PresenceLease,
  PresenceUsersRead,
};

enum class SlotState : uint8_t {
  Free,
  Queued,
  Running,
  Complete,
  Abandoned,
};

enum class AudioBatchOutcome : uint8_t {
  Success,
  TransientFailure,
  FatalFailure,
};

struct ControlSlot {
  SlotState state = SlotState::Free;
  SemaphoreHandle_t done = nullptr;
  RequestType type = RequestType::SessionStart;
  uint8_t channel = 0;
  char sessionId[64] = {0};
  uint32_t lastSeq = 0;
  bool autoRelease = false;
  RtdbRequestService::Result result;
};

struct AudioBlock {
  int16_t samples[AudioConfig::CHUNK_SAMPLES];
  size_t sampleCount = 0;
  uint8_t channel = 0;
  uint32_t sequence = 0;
  uint32_t recordedAtMs = 0;
  uint8_t uploadAttempts = 0;
  char sessionId[64] = {0};
};

TaskHandle_t requestTaskHandle = nullptr;
QueueHandle_t highQueue = nullptr;
QueueHandle_t freeAudioQueue = nullptr;
QueueHandle_t readyAudioQueue = nullptr;
SemaphoreHandle_t lowDataMutex = nullptr;

ControlSlot controlSlots[RtdbRequestConfig::CONTROL_SLOT_COUNT];
AudioBlock audioBlocks[RtdbUploadConfig::TX_QUEUE_LENGTH];
portMUX_TYPE slotMux = portMUX_INITIALIZER_UNLOCKED;
portMUX_TYPE serviceMux = portMUX_INITIALIZER_UNLOCKED;

bool requestRunning = false;
bool audioPriority = false;
bool recordingActive = false;
bool uploadInFlight = false;
uint8_t uploadInFlightBlockCount = 0;
bool uploadFailure = false;
uint32_t consecutiveTransientAudioFailures = 0;
uint32_t totalTransientAudioFailures = 0;
uint32_t lifetimeAudioUploadFailures = 0;
uint64_t lifetimeDiscardedAudioSamples = 0;
bool audioAbortRequested = false;
uint32_t audioRetryNotBeforeMs = 0;
bool uploadSessionAuthorized = false;
uint8_t authorizedChannel = 0;
char authorizedSessionId[64] = {0};

bool presenceLeasePending = false;
bool presenceLeaseResultReady = false;
bool presenceLeaseLastResult = false;
bool presenceLeaseDeferredLogged = false;
uint32_t presenceLeaseExpiresAtMs = 0;
char pendingPresenceLeasePayload[AvailabilityConfig::LEASE_PAYLOAD_BUFFER_BYTES] = {0};

bool presenceUsersReadPending = false;
bool presenceUsersReadResultReady = false;
bool presenceUsersReadLastResult = false;
uint32_t presenceUsersReadExpiresAtMs = 0;
uint32_t presenceUsersReadGeneration = 0;
RtdbRequestService::PresenceSnapshot presenceUsersReadSnapshot;

// Exactly one secure client and one FirebaseClient transport exist.  The SSE
// task is removed before any synchronous write starts, so a second TLS session
// is never resident in heap.
WiFiClientSecure firebaseTls;
AsyncClientClass firebaseAsync(firebaseTls);
FirebaseApp firebaseApp;
RealtimeDatabase firebaseDatabase;
NoAuth firebaseNoAuth;
bool firebaseInitialized = false;
bool firebaseStreamActive = false;
uint8_t firebaseStreamChannel = 0;
constexpr const char* FIREBASE_STREAM_UID = "rtdbAudioStream";

constexpr size_t AUDIO_UPLOAD_JSON_BYTES = AudioConfig::BASE64_BUFFER_BYTES + 640;
alignas(4) char audioUploadJson[AUDIO_UPLOAD_JSON_BYTES] = {0};
char controlJson[320] = {0};
char pathBuffer[256] = {0};

const char* requestTypeName(RequestType type) {
  switch (type) {
    case RequestType::SessionStart: return "SESSION_START";
    case RequestType::AudioBatch: return "AUDIO_BATCH";
    case RequestType::SessionEnd: return "SESSION_END";
    case RequestType::StreamStart: return "STREAM_START";
    case RequestType::StreamStop: return "STREAM_STOP";
    case RequestType::PresenceLease: return "PRESENCE_LEASE";
    case RequestType::PresenceUsersRead: return "PRESENCE_USERS_READ";
    default: return "UNKNOWN";
  }
}

const char* priorityName(RequestType type) {
  return (type == RequestType::PresenceLease ||
          type == RequestType::PresenceUsersRead)
             ? "low"
             : "high";
}

uint32_t highDepth() {
  return highQueue == nullptr ? 0 : uxQueueMessagesWaiting(highQueue);
}

uint32_t lowDepth() {
  if (!AvailabilityConfig::ENABLED || lowDataMutex == nullptr) {
    return 0;
  }
  uint32_t depth = 0;
  xSemaphoreTake(lowDataMutex, portMAX_DELAY);
  depth = (presenceLeasePending ? 1U : 0U) +
          (presenceUsersReadPending ? 1U : 0U);
  xSemaphoreGive(lowDataMutex);
  return depth;
}

uint32_t readyAudioDepth() {
  return readyAudioQueue == nullptr ? 0 : uxQueueMessagesWaiting(readyAudioQueue);
}

void logQueueEnqueue(RequestType type) {
  Serial.printf(
      "[RTDB_QUEUE] enqueue type=%s priority=%s highDepth=%lu lowDepth=%lu audioDepth=%lu\n",
      requestTypeName(type),
      priorityName(type),
      static_cast<unsigned long>(highDepth()),
      static_cast<unsigned long>(lowDepth()),
      static_cast<unsigned long>(readyAudioDepth()));
}

void logHeap(RequestType type, const char* phase) {
  Serial.printf(
      "[RTDB_HEAP] type=%s phase=%s free=%lu largest=%lu minimum=%lu stackRemaining=%lu\n",
      requestTypeName(type),
      phase,
      static_cast<unsigned long>(ESP.getFreeHeap()),
      static_cast<unsigned long>(heap_caps_get_largest_free_block(MALLOC_CAP_8BIT)),
      static_cast<unsigned long>(heap_caps_get_minimum_free_size(MALLOC_CAP_8BIT)),
      static_cast<unsigned long>(uxTaskGetStackHighWaterMark(nullptr)));
}

void resetAudioBlock(uint8_t index) {
  if (index >= RtdbUploadConfig::TX_QUEUE_LENGTH) {
    return;
  }
  AudioBlock& block = audioBlocks[index];
  block.sampleCount = 0;
  block.channel = 0;
  block.sequence = 0;
  block.recordedAtMs = 0;
  block.uploadAttempts = 0;
  block.sessionId[0] = '\0';
}

void returnAudioBlockToFreeQueue(uint8_t index) {
  if (freeAudioQueue == nullptr || index >= RtdbUploadConfig::TX_QUEUE_LENGTH) {
    return;
  }
  resetAudioBlock(index);
  xQueueSend(freeAudioQueue, &index, portMAX_DELAY);
}

void addDiscardedAudioSamples(size_t sampleCount) {
  if (sampleCount == 0) {
    return;
  }
  portENTER_CRITICAL(&serviceMux);
  lifetimeDiscardedAudioSamples += sampleCount;
  portEXIT_CRITICAL(&serviceMux);
}

bool isAudioAbortRequested() {
  portENTER_CRITICAL(&serviceMux);
  const bool requested = audioAbortRequested;
  portEXIT_CRITICAL(&serviceMux);
  return requested;
}

bool isRecordingActiveSnapshot() {
  portENTER_CRITICAL(&serviceMux);
  const bool active = recordingActive;
  portEXIT_CRITICAL(&serviceMux);
  return active;
}

bool canDequeueAudioNow() {
  if (!WifiConnection::isConnected()) {
    return false;
  }
  bool blocked = false;
  uint32_t retryAt = 0;
  portENTER_CRITICAL(&serviceMux);
  blocked = audioAbortRequested || !uploadSessionAuthorized;
  retryAt = audioRetryNotBeforeMs;
  portEXIT_CRITICAL(&serviceMux);
  if (blocked || retryAt == 0) {
    return !blocked;
  }
  return static_cast<int32_t>(millis() - retryAt) >= 0;
}

uint32_t transientRetryBackoffMs(uint32_t consecutiveFailures) {
  uint32_t multiplier = consecutiveFailures == 0 ? 1 : consecutiveFailures;
  uint32_t delayMs = RtdbUploadConfig::TRANSIENT_RETRY_BASE_MS * multiplier;
  return delayMs > RtdbUploadConfig::TRANSIENT_RETRY_MAX_MS
             ? RtdbUploadConfig::TRANSIENT_RETRY_MAX_MS
             : delayMs;
}

const char* audioBatchOutcomeName(AudioBatchOutcome outcome) {
  switch (outcome) {
    case AudioBatchOutcome::Success: return "success";
    case AudioBatchOutcome::TransientFailure: return "transient_failure";
    case AudioBatchOutcome::FatalFailure: return "fatal_failure";
    default: return "unknown";
  }
}

bool isTransientFirebaseError(int code) {
  // FirebaseClient reports positive HTTP status codes and negative client/TCP
  // errors.  Permission and malformed-request statuses are fatal; transport,
  // throttling, timeout and server failures are retryable.
  return code <= 0 || code == 408 || code == 425 || code == 429 || code >= 500;
}

bool buildChannelKey(uint8_t channel, char* out, size_t outSize) {
  return out != nullptr && outSize >= 5 &&
         snprintf(out, outSize, "ch%02u", static_cast<unsigned int>(channel)) > 0;
}

bool buildTalkersPath(uint8_t channel, char* out, size_t outSize) {
  char channelKey[8] = {0};
  if (!buildChannelKey(channel, channelKey, sizeof(channelKey))) {
    return false;
  }
  const int written = snprintf(
      out,
      outSize,
      "/rooms/%s/channels/%s/talkers",
      AppConfig::ROOM_ID,
      channelKey);
  return written > 0 && static_cast<size_t>(written) < outSize;
}

bool buildLivePath(uint8_t channel, char* out, size_t outSize) {
  char talkers[192] = {0};
  if (!buildTalkersPath(channel, talkers, sizeof(talkers))) {
    return false;
  }
  const int written = snprintf(
      out, outSize, "%s/%s/live", talkers, AppConfig::DEVICE_ID);
  return written > 0 && static_cast<size_t>(written) < outSize;
}

bool buildChunkPath(
    uint8_t channel,
    uint32_t sequence,
    char* out,
    size_t outSize) {
  char live[224] = {0};
  if (!buildLivePath(channel, live, sizeof(live))) {
    return false;
  }
  const int written = snprintf(
      out,
      outSize,
      "%s/chunks/%08lu",
      live,
      static_cast<unsigned long>(sequence));
  return written > 0 && static_cast<size_t>(written) < outSize;
}

bool buildPresenceUserPath(char* out, size_t outSize) {
  const int written = snprintf(
      out,
      outSize,
      "%s/user_%s",
      AvailabilityConfig::RTDB_USERS_PATH,
      AppConfig::USER_ID);
  return written > 0 && static_cast<size_t>(written) < outSize;
}

void configureFirebaseTimeouts() {
  firebaseAsync.setSyncSendTimeout(FirebaseClientConfig::SYNC_SEND_TIMEOUT_SECONDS);
  firebaseAsync.setSyncReadTimeout(FirebaseClientConfig::SYNC_READ_TIMEOUT_SECONDS);
  // FirebaseClient requires a value above 150 seconds for this keep-alive
  // session timeout.  It does not keep a second client or second TLS session.
  firebaseAsync.setSessionTimeout(FirebaseClientConfig::SESSION_TIMEOUT_SECONDS);
}

void pumpFirebase() {
  if (!firebaseInitialized) {
    return;
  }
  firebaseApp.loop();
  firebaseDatabase.loop();
}

int firebaseErrorCode() {
  return firebaseAsync.lastError().code();
}

void logFirebaseFailure(RequestType type, uint8_t attempt, uint32_t durationMs) {
  FirebaseError error = firebaseAsync.lastError();
  Serial.printf(
      "[RTDB_REQUEST] fail type=%s attempt=%u firebaseCode=%d message=%s durationMs=%lu\n",
      requestTypeName(type),
      static_cast<unsigned int>(attempt),
      error.code(),
      error.message().c_str(),
      static_cast<unsigned long>(durationMs));
}

void firebaseStreamCallback(AsyncResult& result) {
  if (!result.isResult()) {
    return;
  }
  if (result.isError()) {
    Serial.printf(
        "[RTDB][STREAM][ERROR] code=%d message=%s uid=%s\n",
        result.error().code(),
        result.error().message().c_str(),
        result.uid().c_str());
  }
  if (!result.available()) {
    return;
  }

  RealtimeDatabaseResult& stream = result.to<RealtimeDatabaseResult>();
  if (!stream.isStream()) {
    return;
  }

  RtdbAudioStream::ingestFirebaseEvent(
      stream.event().c_str(),
      stream.dataPath().c_str(),
      stream.data().c_str());
}

bool initializeFirebaseClient() {
  if (firebaseInitialized) {
    return true;
  }
  if (!WifiConnection::isConnected()) {
    Serial.println("[FIREBASE_CLIENT] initialize skipped: WiFi disconnected");
    return false;
  }

  firebaseTls.setInsecure();
  firebaseTls.setHandshakeTimeout(
      FirebaseClientConfig::TLS_HANDSHAKE_TIMEOUT_SECONDS);
  configureFirebaseTimeouts();

  logHeap(RequestType::StreamStart, "before_firebase_initialize");
  initializeApp(
      firebaseAsync,
      firebaseApp,
      getAuth(firebaseNoAuth),
      FirebaseClientConfig::INITIALIZE_TIMEOUT_MS);
  firebaseApp.getApp<RealtimeDatabase>(firebaseDatabase);
  firebaseDatabase.url(FirebaseConfig::DATABASE_URL);
  firebaseInitialized = firebaseApp.ready();
  logHeap(RequestType::StreamStart, "after_firebase_initialize");

  if (!firebaseInitialized) {
    FirebaseError error = firebaseAsync.lastError();
    Serial.printf(
        "[FIREBASE_CLIENT] initialize failed code=%d message=%s\n",
        error.code(),
        error.message().c_str());
  } else {
    Serial.printf(
        "[FIREBASE_CLIENT] initialized version=%s asyncQueueLimit=1 freeHeap=%lu largestBlock=%lu\n",
        FIREBASE_CLIENT_VERSION,
        static_cast<unsigned long>(ESP.getFreeHeap()),
        static_cast<unsigned long>(
            heap_caps_get_largest_free_block(MALLOC_CAP_8BIT)));
  }
  return firebaseInitialized;
}

bool stopFirebaseStreamInternal(uint32_t timeoutMs) {
  if (!firebaseInitialized) {
    firebaseStreamActive = false;
    firebaseStreamChannel = 0;
    return true;
  }

  if (firebaseAsync.taskCount() > 0) {
    firebaseAsync.stopAsync(FIREBASE_STREAM_UID);
  }
  const uint32_t startedAt = millis();
  while (firebaseAsync.taskCount() > 0 && millis() - startedAt < timeoutMs) {
    pumpFirebase();
    vTaskDelay(pdMS_TO_TICKS(2));
  }

  const bool stopped = firebaseAsync.taskCount() == 0;
  // Explicitly release mbedTLS buffers before microphone/upload activity.
  firebaseTls.stop();
  firebaseStreamActive = false;
  firebaseStreamChannel = 0;
  Serial.printf(
      "[RTDB][STREAM] firebaseclient_stop stopped=%s tasks=%u durationMs=%lu freeHeap=%lu largestBlock=%lu\n",
      stopped ? "true" : "false",
      static_cast<unsigned int>(firebaseAsync.taskCount()),
      static_cast<unsigned long>(millis() - startedAt),
      static_cast<unsigned long>(ESP.getFreeHeap()),
      static_cast<unsigned long>(
          heap_caps_get_largest_free_block(MALLOC_CAP_8BIT)));
  return stopped;
}

RtdbRequestService::Result startFirebaseStreamInternal(
    uint8_t channel,
    uint32_t timeoutMs) {
  RtdbRequestService::Result result;
  if (!WifiConnection::isConnected()) {
    result.outcome = RtdbRequestService::Outcome::WifiDisconnected;
    return result;
  }
  if (!initializeFirebaseClient()) {
    result.outcome = RtdbRequestService::Outcome::Failed;
    result.httpCode = firebaseErrorCode();
    return result;
  }

  stopFirebaseStreamInternal(FirebaseClientConfig::STREAM_STOP_TIMEOUT_MS);
  if (!buildTalkersPath(channel, pathBuffer, sizeof(pathBuffer))) {
    result.outcome = RtdbRequestService::Outcome::Failed;
    return result;
  }

  firebaseAsync.setSSEFilters("get,put,patch,keep-alive,cancel,auth_revoked");
  firebaseDatabase.get(
      firebaseAsync,
      pathBuffer,
      firebaseStreamCallback,
      true,
      FIREBASE_STREAM_UID);

  const uint32_t startedAt = millis();
  while (firebaseAsync.taskCount() == 0 && millis() - startedAt < timeoutMs) {
    pumpFirebase();
    vTaskDelay(pdMS_TO_TICKS(2));
  }
  firebaseStreamActive = firebaseAsync.taskCount() > 0;
  firebaseStreamChannel = firebaseStreamActive ? channel : 0;
  result.outcome = firebaseStreamActive
                       ? RtdbRequestService::Outcome::Success
                       : RtdbRequestService::Outcome::Failed;
  result.httpCode = firebaseStreamActive ? 200 : firebaseErrorCode();
  Serial.printf(
      "[RTDB][STREAM] firebaseclient_start channel=%u active=%s tasks=%u path=%s durationMs=%lu\n",
      static_cast<unsigned int>(channel),
      firebaseStreamActive ? "true" : "false",
      static_cast<unsigned int>(firebaseAsync.taskCount()),
      pathBuffer,
      static_cast<unsigned long>(millis() - startedAt));
  return result;
}

bool prepareForSynchronousWrite(RequestType type) {
  if (!WifiConnection::isConnected()) {
    return false;
  }
  if (!initializeFirebaseClient()) {
    return false;
  }
  if (firebaseStreamActive || firebaseAsync.taskCount() > 0) {
    Serial.printf(
        "[RTDB_REQUEST] stopping_stream_before_write type=%s tasks=%u\n",
        requestTypeName(type),
        static_cast<unsigned int>(firebaseAsync.taskCount()));
    if (!stopFirebaseStreamInternal(
            FirebaseClientConfig::STREAM_STOP_TIMEOUT_MS)) {
      return false;
    }
  }
  configureFirebaseTimeouts();
  return true;
}

RtdbRequestService::Result syncUpdateJson(
    RequestType type,
    const char* path,
    const char* json,
    uint8_t maxAttempts) {
  RtdbRequestService::Result result;
  if (path == nullptr || json == nullptr) {
    result.outcome = RtdbRequestService::Outcome::Failed;
    return result;
  }

  for (uint8_t attempt = 1; attempt <= maxAttempts; ++attempt) {
    if (!prepareForSynchronousWrite(type)) {
      result.outcome = WifiConnection::isConnected()
                           ? RtdbRequestService::Outcome::Failed
                           : RtdbRequestService::Outcome::WifiDisconnected;
      result.httpCode = firebaseErrorCode();
      return result;
    }
    const uint32_t startedAt = millis();
    Serial.printf(
        "[RTDB_REQUEST] start type=%s attempt=%u firebaseClient=true path=%s payloadBytes=%u\n",
        requestTypeName(type),
        static_cast<unsigned int>(attempt),
        path,
        static_cast<unsigned int>(strlen(json)));
    logHeap(type, "before_firebase_update");
    const bool success = firebaseDatabase.update<object_t>(
        firebaseAsync, path, object_t(json));
    const uint32_t durationMs = millis() - startedAt;
    logHeap(type, "after_firebase_update");
    result.httpCode = success ? 204 : firebaseErrorCode();
    if (success) {
      result.outcome = RtdbRequestService::Outcome::Success;
      Serial.printf(
          "[RTDB_REQUEST] success type=%s attempt=%u firebaseClient=true durationMs=%lu\n",
          requestTypeName(type),
          static_cast<unsigned int>(attempt),
          static_cast<unsigned long>(durationMs));
      return result;
    }
    logFirebaseFailure(type, attempt, durationMs);
    firebaseTls.stop();
    if (!WifiConnection::isConnected()) {
      result.outcome = RtdbRequestService::Outcome::WifiDisconnected;
      return result;
    }
    if (attempt < maxAttempts) {
      vTaskDelay(pdMS_TO_TICKS(RtdbHttpConfig::CONTROL_RETRY_DELAY_MS));
    }
  }
  result.outcome = RtdbRequestService::Outcome::Failed;
  return result;
}

RtdbRequestService::Result syncSetJson(
    RequestType type,
    const char* path,
    const char* json) {
  RtdbRequestService::Result result;
  if (path == nullptr || json == nullptr) {
    result.outcome = RtdbRequestService::Outcome::Failed;
    return result;
  }
  if (!prepareForSynchronousWrite(type)) {
    result.outcome = WifiConnection::isConnected()
                         ? RtdbRequestService::Outcome::Failed
                         : RtdbRequestService::Outcome::WifiDisconnected;
    result.httpCode = firebaseErrorCode();
    return result;
  }

  const uint32_t startedAt = millis();
  Serial.printf(
      "[RTDB_REQUEST] start type=%s attempt=1 firebaseClient=true path=%s payloadBytes=%u\n",
      requestTypeName(type),
      path,
      static_cast<unsigned int>(strlen(json)));
  logHeap(type, "before_firebase_set");
  const bool success = firebaseDatabase.set<object_t>(
      firebaseAsync, path, object_t(json));
  const uint32_t durationMs = millis() - startedAt;
  logHeap(type, "after_firebase_set");
  result.httpCode = success ? 204 : firebaseErrorCode();
  if (success) {
    result.outcome = RtdbRequestService::Outcome::Success;
    Serial.printf(
        "[RTDB_REQUEST] success type=%s firebaseClient=true durationMs=%lu\n",
        requestTypeName(type),
        static_cast<unsigned long>(durationMs));
  } else {
    result.outcome = WifiConnection::isConnected()
                         ? RtdbRequestService::Outcome::Failed
                         : RtdbRequestService::Outcome::WifiDisconnected;
    logFirebaseFailure(type, 1, durationMs);
    firebaseTls.stop();
  }
  return result;
}

RtdbRequestService::Result performSessionStart(
    uint8_t channel,
    const char* sessionId) {
  RtdbRequestService::Result result;
  if (!buildLivePath(channel, pathBuffer, sizeof(pathBuffer))) {
    return result;
  }
  const int written = snprintf(
      controlJson,
      sizeof(controlJson),
      "{\"meta\":{\"active\":true,\"sessionId\":\"%s\",\"deviceId\":\"%s\","
      "\"sampleRate\":%lu,\"chunkMs\":%u},\"chunks\":null}",
      sessionId,
      AppConfig::DEVICE_ID,
      static_cast<unsigned long>(AudioConfig::SAMPLE_RATE),
      static_cast<unsigned int>(AudioConfig::CHUNK_MS));
  if (written <= 0 || static_cast<size_t>(written) >= sizeof(controlJson)) {
    return result;
  }
  return syncUpdateJson(
      RequestType::SessionStart,
      pathBuffer,
      controlJson,
      RtdbHttpConfig::CONTROL_REQUEST_MAX_ATTEMPTS);
}

RtdbRequestService::Result performSessionEnd(
    uint8_t channel,
    const char* sessionId,
    uint32_t lastSeq) {
  RtdbRequestService::Result result;
  if (!buildLivePath(channel, pathBuffer, sizeof(pathBuffer))) {
    return result;
  }
  const int written = snprintf(
      controlJson,
      sizeof(controlJson),
      "{\"meta\":{\"active\":false,\"sessionId\":\"%s\",\"deviceId\":\"%s\","
      "\"lastSeq\":%lu,\"sampleRate\":%lu,\"chunkMs\":%u},\"chunks\":null}",
      sessionId,
      AppConfig::DEVICE_ID,
      static_cast<unsigned long>(lastSeq),
      static_cast<unsigned long>(AudioConfig::SAMPLE_RATE),
      static_cast<unsigned int>(AudioConfig::CHUNK_MS));
  if (written <= 0 || static_cast<size_t>(written) >= sizeof(controlJson)) {
    return result;
  }
  return syncUpdateJson(
      RequestType::SessionEnd,
      pathBuffer,
      controlJson,
      RtdbHttpConfig::CONTROL_REQUEST_MAX_ATTEMPTS);
}

bool buildSingleAudioJson(
    const AudioBlock& block,
    char* out,
    size_t outSize,
    size_t& outLength,
    size_t& encodedBytes) {
  outLength = 0;
  encodedBytes = 0;
  if (out == nullptr || outSize == 0) {
    return false;
  }
  const int prefixLength = snprintf(
      out,
      outSize,
      "{\"seq\":%lu,\"sessionId\":\"%s\",\"deviceId\":\"%s\","
      "\"format\":\"pcm_s16le_base64\",\"sampleRate\":%lu,\"chunkMs\":%u,"
      "\"recordedAtMs\":%lu,\"data\":\"",
      static_cast<unsigned long>(block.sequence),
      block.sessionId,
      AppConfig::DEVICE_ID,
      static_cast<unsigned long>(AudioConfig::SAMPLE_RATE),
      static_cast<unsigned int>(AudioConfig::CHUNK_MS),
      static_cast<unsigned long>(block.recordedAtMs));
  constexpr size_t suffixBytes = 2;  // closing quote + object brace
  if (prefixLength <= 0 ||
      static_cast<size_t>(prefixLength) + suffixBytes + 1 >= outSize) {
    return false;
  }

  size_t outputLength = 0;
  const size_t pcmBytes = block.sampleCount * sizeof(int16_t);
  const size_t available =
      outSize - static_cast<size_t>(prefixLength) - suffixBytes - 1;
  const int encodeResult = mbedtls_base64_encode(
      reinterpret_cast<unsigned char*>(out + prefixLength),
      available,
      &outputLength,
      reinterpret_cast<const unsigned char*>(block.samples),
      pcmBytes);
  if (encodeResult != 0 || outputLength > available) {
    Serial.printf(
        "[AUDIO_TX] encode_fail seq=%lu rc=%d pcmBytes=%u available=%u\n",
        static_cast<unsigned long>(block.sequence),
        encodeResult,
        static_cast<unsigned int>(pcmBytes),
        static_cast<unsigned int>(available));
    return false;
  }

  size_t cursor = static_cast<size_t>(prefixLength) + outputLength;
  out[cursor++] = '"';
  out[cursor++] = '}';
  out[cursor] = '\0';
  outLength = cursor;
  encodedBytes = outputLength;
  return true;
}

bool isAudioSessionAuthorized(const AudioBlock& block) {
  portENTER_CRITICAL(&serviceMux);
  const bool authorized = uploadSessionAuthorized &&
                          authorizedChannel == block.channel &&
                          strcmp(authorizedSessionId, block.sessionId) == 0;
  portEXIT_CRITICAL(&serviceMux);
  return authorized;
}

AudioBatchOutcome uploadAudioBatch(const uint8_t* indices, size_t count) {
  if (indices == nullptr || count == 0) {
    return AudioBatchOutcome::Success;
  }
  if (count != 1) {
    Serial.printf(
        "[AUDIO_TX] batch_abort reason=unsupported_batch_count count=%u\n",
        static_cast<unsigned int>(count));
    return AudioBatchOutcome::FatalFailure;
  }

  AudioBlock& block = audioBlocks[indices[0]];
  if (!isAudioSessionAuthorized(block)) {
    Serial.printf(
        "[AUDIO_TX] batch_abort reason=session_not_authorized channel=%u session=%s seq=%lu\n",
        static_cast<unsigned int>(block.channel),
        block.sessionId,
        static_cast<unsigned long>(block.sequence));
    return AudioBatchOutcome::FatalFailure;
  }
  if (!buildChunkPath(
          block.channel, block.sequence, pathBuffer, sizeof(pathBuffer))) {
    return AudioBatchOutcome::FatalFailure;
  }

  size_t payloadLength = 0;
  size_t encodedBytes = 0;
  if (!buildSingleAudioJson(
          block,
          audioUploadJson,
          sizeof(audioUploadJson),
          payloadLength,
          encodedBytes)) {
    Serial.printf(
        "[AUDIO_TX] batch_abort reason=fixed_payload_build_failed seq=%lu bufferBytes=%u largestBlock=%lu\n",
        static_cast<unsigned long>(block.sequence),
        static_cast<unsigned int>(sizeof(audioUploadJson)),
        static_cast<unsigned long>(
            heap_caps_get_largest_free_block(MALLOC_CAP_8BIT)));
    return AudioBatchOutcome::FatalFailure;
  }

  const bool recordingNow = isRecordingActiveSnapshot();
  Serial.printf(
      "[AUDIO_TX] batch channel=%u count=1 seq=%lu recording=%s totalAttempt=%u/%u\n",
      static_cast<unsigned int>(block.channel),
      static_cast<unsigned long>(block.sequence),
      recordingNow ? "true" : "false",
      static_cast<unsigned int>(block.uploadAttempts + 1),
      static_cast<unsigned int>(
          RtdbUploadConfig::DRAIN_TOTAL_MAX_ATTEMPTS_PER_CHUNK));

  const RtdbRequestService::Result writeResult = syncSetJson(
      RequestType::AudioBatch, pathBuffer, audioUploadJson);
  const bool success = writeResult.succeeded();
  const AudioBatchOutcome outcome =
      success
          ? AudioBatchOutcome::Success
          : (isTransientFirebaseError(writeResult.httpCode)
                 ? AudioBatchOutcome::TransientFailure
                 : AudioBatchOutcome::FatalFailure);
  Serial.printf(
      "[AUDIO_TX] batch_result outcome=%s success=%s seq=%lu firebaseCode=%d encodedBytes=%u payloadBytes=%u queueDepth=%lu largestBlock=%lu\n",
      audioBatchOutcomeName(outcome),
      success ? "true" : "false",
      static_cast<unsigned long>(block.sequence),
      writeResult.httpCode,
      static_cast<unsigned int>(encodedBytes),
      static_cast<unsigned int>(payloadLength),
      static_cast<unsigned long>(readyAudioDepth()),
      static_cast<unsigned long>(
          heap_caps_get_largest_free_block(MALLOC_CAP_8BIT)));
  return outcome;
}

void processAudioBatch(uint8_t firstIndex) {
  uint8_t indices[RtdbUploadConfig::UPLOAD_BATCH_MAX_CHUNKS];
  indices[0] = firstIndex;
  const size_t count = 1;

  portENTER_CRITICAL(&serviceMux);
  uploadInFlight = true;
  uploadInFlightBlockCount = 1;
  portEXIT_CRITICAL(&serviceMux);

  logQueueEnqueue(RequestType::AudioBatch);
  const AudioBatchOutcome outcome = uploadAudioBatch(indices, count);
  uint32_t consecutiveSnapshot = 0;
  uint32_t totalSnapshot = 0;
  uint32_t lifetimeFailureSnapshot = 0;
  uint32_t retryDelayMs = 0;

  portENTER_CRITICAL(&serviceMux);
  uploadInFlight = false;
  uploadInFlightBlockCount = 0;
  if (outcome != AudioBatchOutcome::Success) {
    ++lifetimeAudioUploadFailures;
    lifetimeFailureSnapshot = lifetimeAudioUploadFailures;
  }
  if (outcome == AudioBatchOutcome::Success) {
    consecutiveTransientAudioFailures = 0;
    audioRetryNotBeforeMs = 0;
  } else if (outcome == AudioBatchOutcome::TransientFailure) {
    ++consecutiveTransientAudioFailures;
    ++totalTransientAudioFailures;
    consecutiveSnapshot = consecutiveTransientAudioFailures;
    totalSnapshot = totalTransientAudioFailures;
    retryDelayMs = transientRetryBackoffMs(consecutiveTransientAudioFailures);
    audioRetryNotBeforeMs = millis() + retryDelayMs;
  } else {
    uploadFailure = true;
  }
  portEXIT_CRITICAL(&serviceMux);

  if (outcome == AudioBatchOutcome::FatalFailure) {
    Serial.printf(
        "[AUDIO_TX] fatal_failure lifetimeTotal=%lu\n",
        static_cast<unsigned long>(lifetimeFailureSnapshot));
  }

  AudioBlock& block = audioBlocks[firstIndex];
  if (outcome == AudioBatchOutcome::TransientFailure) {
    ++block.uploadAttempts;
  }

  const bool recordingNow = isRecordingActiveSnapshot();
  const bool underRetryLimit =
      block.uploadAttempts <
      RtdbUploadConfig::DRAIN_TOTAL_MAX_ATTEMPTS_PER_CHUNK;
  const bool preserveForDrain =
      outcome == AudioBatchOutcome::TransientFailure &&
      !recordingNow &&
      !isAudioAbortRequested() &&
      underRetryLimit;

  bool preserved = false;
  if (preserveForDrain &&
      xQueueSendToFront(readyAudioQueue, &firstIndex, 0) == pdTRUE) {
    preserved = true;
  } else {
    if (outcome != AudioBatchOutcome::Success) {
      addDiscardedAudioSamples(block.sampleCount);
    }
    if (outcome == AudioBatchOutcome::TransientFailure &&
        !recordingNow && !underRetryLimit) {
      Serial.printf(
          "[AUDIO_TX] retry_exhausted seq=%lu attempts=%u action=discard_continue\n",
          static_cast<unsigned long>(block.sequence),
          static_cast<unsigned int>(block.uploadAttempts));
    }
    returnAudioBlockToFreeQueue(firstIndex);
  }

  if (outcome == AudioBatchOutcome::TransientFailure) {
    Serial.printf(
        "[AUDIO_TX] transient_failure_continue consecutive=%lu sessionTotal=%lu lifetimeTotal=%lu recording=%s readyDepth=%lu preserved=%s retryDelayMs=%lu\n",
        static_cast<unsigned long>(consecutiveSnapshot),
        static_cast<unsigned long>(totalSnapshot),
        static_cast<unsigned long>(lifetimeFailureSnapshot),
        recordingNow ? "true" : "false",
        static_cast<unsigned long>(readyAudioDepth()),
        preserved ? "true" : "false",
        static_cast<unsigned long>(retryDelayMs));
  }
}

RtdbRequestService::Result processControl(const ControlSlot& slot) {
  switch (slot.type) {
    case RequestType::SessionStart:
      return performSessionStart(slot.channel, slot.sessionId);
    case RequestType::SessionEnd:
      return performSessionEnd(slot.channel, slot.sessionId, slot.lastSeq);
    case RequestType::StreamStart:
      return startFirebaseStreamInternal(
          slot.channel, FirebaseClientConfig::STREAM_START_TIMEOUT_MS);
    case RequestType::StreamStop: {
      RtdbRequestService::Result result;
      result.outcome = stopFirebaseStreamInternal(
                           FirebaseClientConfig::STREAM_STOP_TIMEOUT_MS)
                           ? RtdbRequestService::Outcome::Success
                           : RtdbRequestService::Outcome::TimedOut;
      result.httpCode = result.succeeded() ? 200 : firebaseErrorCode();
      return result;
    }
    default: {
      RtdbRequestService::Result result;
      result.outcome = RtdbRequestService::Outcome::Failed;
      return result;
    }
  }
}

void processControlSlot(uint8_t slotIndex) {
  if (slotIndex >= RtdbRequestConfig::CONTROL_SLOT_COUNT) {
    return;
  }
  bool skip = false;
  bool autoRelease = false;
  portENTER_CRITICAL(&slotMux);
  if (controlSlots[slotIndex].state == SlotState::Abandoned) {
    controlSlots[slotIndex].state = SlotState::Free;
    skip = true;
  } else {
    controlSlots[slotIndex].state = SlotState::Running;
    autoRelease = controlSlots[slotIndex].autoRelease;
  }
  portEXIT_CRITICAL(&slotMux);
  if (skip) {
    return;
  }

  ControlSlot& slot = controlSlots[slotIndex];
  const RtdbRequestService::Result result = processControl(slot);

  if (slot.type == RequestType::SessionStart) {
    const bool uploadPrepared = result.succeeded();
    portENTER_CRITICAL(&serviceMux);
    uploadSessionAuthorized = uploadPrepared;
    if (uploadPrepared) {
      authorizedChannel = slot.channel;
      snprintf(
          authorizedSessionId,
          sizeof(authorizedSessionId),
          "%s",
          slot.sessionId);
    } else {
      authorizedChannel = 0;
      authorizedSessionId[0] = '\0';
      uploadFailure = true;
    }
    portEXIT_CRITICAL(&serviceMux);
    Serial.printf(
        "[AUDIO_TX] session_start_async channel=%u session=%s outcome=%s firebaseCode=%d uploadPrepared=%s\n",
        static_cast<unsigned int>(slot.channel),
        slot.sessionId,
        RtdbRequestService::outcomeName(result.outcome),
        result.httpCode,
        uploadPrepared ? "true" : "false");
  } else if (slot.type == RequestType::SessionEnd) {
    portENTER_CRITICAL(&serviceMux);
    if (uploadSessionAuthorized &&
        authorizedChannel == slot.channel &&
        strcmp(authorizedSessionId, slot.sessionId) == 0) {
      uploadSessionAuthorized = false;
      authorizedChannel = 0;
      authorizedSessionId[0] = '\0';
    }
    portEXIT_CRITICAL(&serviceMux);
    // Release TLS before the listener starts its SSE connection again.
    firebaseTls.stop();
  }

  bool abandoned = false;
  portENTER_CRITICAL(&slotMux);
  abandoned = slot.state == SlotState::Abandoned;
  if (!abandoned) {
    if (autoRelease) {
      slot.state = SlotState::Free;
    } else {
      slot.result = result;
      slot.state = SlotState::Complete;
    }
  }
  portEXIT_CRITICAL(&slotMux);

  if (!abandoned) {
    if (!autoRelease) {
      xSemaphoreGive(slot.done);
    }
    return;
  }

  if (slot.type == RequestType::SessionStart && result.succeeded()) {
    performSessionEnd(slot.channel, slot.sessionId, 0);
    firebaseTls.stop();
  }
  portENTER_CRITICAL(&slotMux);
  slot.state = SlotState::Free;
  portEXIT_CRITICAL(&slotMux);
}

bool canRunLow(RequestType) {
  if (highDepth() > 0 || readyAudioDepth() > 0) {
    return false;
  }
  portENTER_CRITICAL(&serviceMux);
  const bool blocked = uploadInFlight || audioPriority;
  portEXIT_CRITICAL(&serviceMux);
  return !blocked && !firebaseStreamActive;
}

bool deadlineReached(uint32_t deadlineMs) {
  return deadlineMs != 0 &&
         static_cast<int32_t>(millis() - deadlineMs) >= 0;
}

bool lowRequestStillPending(RequestType type) {
  bool pending = false;
  xSemaphoreTake(lowDataMutex, portMAX_DELAY);
  pending = type == RequestType::PresenceLease
                ? presenceLeasePending
                : presenceUsersReadPending;
  xSemaphoreGive(lowDataMutex);
  return pending;
}

void storePresenceLeaseResult(bool success) {
  xSemaphoreTake(lowDataMutex, portMAX_DELAY);
  presenceLeasePending = false;
  presenceLeaseExpiresAtMs = 0;
  presenceLeaseResultReady = true;
  presenceLeaseLastResult = success;
  presenceLeaseDeferredLogged = false;
  xSemaphoreGive(lowDataMutex);
}

void logPresenceLeaseDeferredOnce() {
  bool shouldLog = false;
  xSemaphoreTake(lowDataMutex, portMAX_DELAY);
  if (!presenceLeaseDeferredLogged) {
    presenceLeaseDeferredLogged = true;
    shouldLog = true;
  }
  xSemaphoreGive(lowDataMutex);
  if (shouldLog) {
    Serial.println(
        "[AVAILABILITY] deferred reason=audio_or_stream_busy type=presence_lease");
  }
}

void initializePresenceSnapshot(
    RtdbRequestService::PresenceSnapshot& snapshot) {
  snapshot = RtdbRequestService::PresenceSnapshot{};
  for (uint8_t index = 0;
       index < AvailabilityConfig::USER_COUNT;
       ++index) {
    snapshot.users[index].userNumber = index + 1;
  }
}

void storePresenceUsersResult(
    bool success,
    const RtdbRequestService::PresenceSnapshot& snapshot,
    uint32_t generation) {
  xSemaphoreTake(lowDataMutex, portMAX_DELAY);
  if (!presenceUsersReadPending ||
      generation != presenceUsersReadGeneration) {
    xSemaphoreGive(lowDataMutex);
    return;
  }
  presenceUsersReadPending = false;
  presenceUsersReadExpiresAtMs = 0;
  presenceUsersReadResultReady = true;
  presenceUsersReadLastResult = success;
  presenceUsersReadSnapshot = snapshot;
  xSemaphoreGive(lowDataMutex);
}

bool findJsonObjectBounds(
    const String& json,
    const char* key,
    int& outStart,
    int& outEnd) {
  outStart = -1;
  outEnd = -1;
  const int keyPos = json.indexOf(key);
  if (keyPos < 0) {
    return false;
  }

  int start = json.indexOf('{', keyPos + static_cast<int>(strlen(key)));
  if (start < 0) {
    return false;
  }

  bool inString = false;
  bool escaping = false;
  int depth = 0;
  for (int pos = start; pos < static_cast<int>(json.length()); ++pos) {
    const char c = json[pos];
    if (inString) {
      if (escaping) {
        escaping = false;
      } else if (c == '\\') {
        escaping = true;
      } else if (c == '"') {
        inString = false;
      }
      continue;
    }
    if (c == '"') {
      inString = true;
    } else if (c == '{') {
      ++depth;
    } else if (c == '}') {
      --depth;
      if (depth == 0) {
        outStart = start;
        outEnd = pos;
        return true;
      }
    }
  }
  return false;
}

bool extractJsonUInt64InRange(
    const String& json,
    int objectStart,
    int objectEnd,
    const char* field,
    uint64_t& outValue) {
  outValue = 0;
  const int fieldPos = json.indexOf(field, objectStart);
  if (fieldPos < 0 || fieldPos > objectEnd) {
    return false;
  }
  int pos = fieldPos + static_cast<int>(strlen(field));
  while (pos <= objectEnd &&
         (json[pos] == ' ' || json[pos] == '\t' ||
          json[pos] == '\r' || json[pos] == '\n')) {
    ++pos;
  }

  bool sawDigit = false;
  uint64_t value = 0;
  while (pos <= objectEnd && json[pos] >= '0' && json[pos] <= '9') {
    sawDigit = true;
    value = value * 10ULL + static_cast<uint64_t>(json[pos] - '0');
    ++pos;
  }
  if (!sawDigit) {
    return false;
  }
  outValue = value;
  return true;
}

void parsePresenceSnapshot(
    const String& response,
    RtdbRequestService::PresenceSnapshot& snapshot) {
  initializePresenceSnapshot(snapshot);
  for (uint8_t userNumber = 1;
       userNumber <= AvailabilityConfig::USER_COUNT;
       ++userNumber) {
    char userKey[20] = {0};
    snprintf(
        userKey,
        sizeof(userKey),
        "\"user_%02u\":",
        static_cast<unsigned int>(userNumber));

    int objectStart = -1;
    int objectEnd = -1;
    if (!findJsonObjectBounds(response, userKey, objectStart, objectEnd)) {
      continue;
    }

    RtdbRequestService::PresenceRecord& record =
        snapshot.users[userNumber - 1];
    record.present = true;

    uint64_t value = 0;
    if (extractJsonUInt64InRange(
            response,
            objectStart,
            objectEnd,
            "\"lastSeenServerMs\":",
            value)) {
      record.lastSeenServerMs = value;
    }
    if (extractJsonUInt64InRange(
            response,
            objectStart,
            objectEnd,
            "\"bootId\":",
            value)) {
      record.bootId = static_cast<uint32_t>(value);
    }
    if (extractJsonUInt64InRange(
            response,
            objectStart,
            objectEnd,
            "\"state\":",
            value)) {
      record.state = static_cast<uint8_t>(value);
    }
    if (extractJsonUInt64InRange(
            response,
            objectStart,
            objectEnd,
            "\"channel\":",
            value)) {
      record.logicalChannel = static_cast<uint8_t>(value);
    }
    if (extractJsonUInt64InRange(
            response,
            objectStart,
            objectEnd,
            "\"transport\":",
            value)) {
      record.transport = static_cast<uint8_t>(value);
    }
  }
}

void expireLowRequestsIfNeeded() {
  bool expireLease = false;
  bool expireUsersRead = false;
  xSemaphoreTake(lowDataMutex, portMAX_DELAY);
  expireLease = presenceLeasePending &&
                deadlineReached(presenceLeaseExpiresAtMs);
  expireUsersRead = presenceUsersReadPending &&
                    deadlineReached(presenceUsersReadExpiresAtMs);
  xSemaphoreGive(lowDataMutex);

  if (expireLease) {
    Serial.println("[AVAILABILITY] lease_expired_before_execution");
    storePresenceLeaseResult(false);
  }
  if (expireUsersRead) {
    Serial.println("[AVAILABILITY] users_read_expired_before_execution");
    RtdbRequestService::PresenceSnapshot empty;
    initializePresenceSnapshot(empty);
    uint32_t generation = 0;
    xSemaphoreTake(lowDataMutex, portMAX_DELAY);
    generation = presenceUsersReadGeneration;
    xSemaphoreGive(lowDataMutex);
    storePresenceUsersResult(false, empty, generation);
  }
}

bool nextPendingLow(RequestType& outType) {
  bool found = false;
  xSemaphoreTake(lowDataMutex, portMAX_DELAY);
  if (presenceLeasePending) {
    outType = RequestType::PresenceLease;
    found = true;
  } else if (presenceUsersReadPending) {
    outType = RequestType::PresenceUsersRead;
    found = true;
  }
  xSemaphoreGive(lowDataMutex);
  return found;
}

void processLow(RequestType type) {
  if (!lowRequestStillPending(type)) {
    return;
  }
  if (type == RequestType::PresenceLease) {
    char payloadBuffer[sizeof(pendingPresenceLeasePayload)] = {0};
    xSemaphoreTake(lowDataMutex, portMAX_DELAY);
    snprintf(
        payloadBuffer,
        sizeof(payloadBuffer),
        "%s",
        pendingPresenceLeasePayload);
    xSemaphoreGive(lowDataMutex);
    if (!buildPresenceUserPath(pathBuffer, sizeof(pathBuffer))) {
      storePresenceLeaseResult(false);
      return;
    }
    const RtdbRequestService::Result result = syncSetJson(
        RequestType::PresenceLease,
        pathBuffer,
        payloadBuffer);
    storePresenceLeaseResult(result.succeeded());
    return;
  }

  bool success = false;
  uint32_t generation = 0;
  xSemaphoreTake(lowDataMutex, portMAX_DELAY);
  generation = presenceUsersReadGeneration;
  xSemaphoreGive(lowDataMutex);
  RtdbRequestService::PresenceSnapshot snapshot;
  initializePresenceSnapshot(snapshot);
  if (prepareForSynchronousWrite(RequestType::PresenceUsersRead)) {
    const uint32_t startedAt = millis();
    // This is the only dynamic JSON response owned by availability. It is
    // parsed in this worker and destroyed before any result crosses tasks.
    String response = firebaseDatabase.get<String>(
        firebaseAsync, AvailabilityConfig::RTDB_USERS_PATH);
    success = firebaseErrorCode() == 0;
    if (success) {
      parsePresenceSnapshot(response, snapshot);
    } else {
      logFirebaseFailure(
          RequestType::PresenceUsersRead, 1, millis() - startedAt);
      firebaseTls.stop();
    }
  }
  storePresenceUsersResult(success, snapshot, generation);
}

void onWifiConnectivityRestored(void* context) {
  TaskHandle_t task = static_cast<TaskHandle_t>(context);
  if (task != nullptr) {
    xTaskNotifyGive(task);
  }
}

void requestTask(void*) {
  WifiConnection::registerConnectedObserver(
      onWifiConnectivityRestored,
      xTaskGetCurrentTaskHandle());

  for (;;) {
    pumpFirebase();
    expireLowRequestsIfNeeded();

    uint8_t controlIndex = 0;
    if (xQueueReceive(highQueue, &controlIndex, 0) == pdTRUE) {
      processControlSlot(controlIndex);
      continue;
    }

    uint8_t audioIndex = 0;
    if (canDequeueAudioNow() &&
        xQueueReceive(readyAudioQueue, &audioIndex, 0) == pdTRUE) {
      processAudioBatch(audioIndex);
      continue;
    }

    if (AvailabilityConfig::ENABLED) {
      RequestType lowType = RequestType::PresenceLease;
      if (nextPendingLow(lowType)) {
        if (canRunLow(lowType)) {
          processLow(lowType);
          continue;
        }
        if (lowType == RequestType::PresenceLease) {
          logPresenceLeaseDeferredOnce();
        }
      }
    }

    ulTaskNotifyTake(
        pdTRUE,
        pdMS_TO_TICKS(RtdbRequestConfig::TASK_IDLE_DELAY_MS));
  }
}

int allocateControlSlot(
    RequestType type,
    uint8_t channel,
    const char* sessionId,
    uint32_t lastSeq) {
  int selected = -1;
  portENTER_CRITICAL(&slotMux);
  for (uint8_t index = 0;
       index < RtdbRequestConfig::CONTROL_SLOT_COUNT;
       ++index) {
    if (controlSlots[index].state == SlotState::Free) {
      controlSlots[index].state = SlotState::Queued;
      selected = index;
      break;
    }
  }
  portEXIT_CRITICAL(&slotMux);
  if (selected < 0) {
    return selected;
  }

  ControlSlot& slot = controlSlots[selected];
  xSemaphoreTake(slot.done, 0);
  slot.type = type;
  slot.channel = channel;
  snprintf(
      slot.sessionId,
      sizeof(slot.sessionId),
      "%s",
      sessionId == nullptr ? "" : sessionId);
  slot.lastSeq = lastSeq;
  slot.autoRelease = false;
  slot.result = RtdbRequestService::Result();
  return selected;
}

RtdbRequestService::Result executeControl(
    RequestType type,
    uint8_t channel,
    const char* sessionId,
    uint32_t lastSeq,
    uint32_t timeoutMs) {
  RtdbRequestService::Result result;
  const int slotIndex = allocateControlSlot(
      type, channel, sessionId, lastSeq);
  if (slotIndex < 0) {
    result.outcome = RtdbRequestService::Outcome::Busy;
    Serial.printf(
        "[RTDB_REQUEST] fail type=%s error=no_control_slot\n",
        requestTypeName(type));
    return result;
  }

  const uint8_t queuedIndex = static_cast<uint8_t>(slotIndex);
  if (xQueueSend(highQueue, &queuedIndex, 0) != pdTRUE) {
    portENTER_CRITICAL(&slotMux);
    controlSlots[slotIndex].state = SlotState::Free;
    portEXIT_CRITICAL(&slotMux);
    result.outcome = RtdbRequestService::Outcome::Busy;
    return result;
  }
  logQueueEnqueue(type);
  if (requestTaskHandle != nullptr) {
    xTaskNotifyGive(requestTaskHandle);
  }

  if (xSemaphoreTake(
          controlSlots[slotIndex].done,
          pdMS_TO_TICKS(timeoutMs)) == pdTRUE) {
    portENTER_CRITICAL(&slotMux);
    result = controlSlots[slotIndex].result;
    controlSlots[slotIndex].state = SlotState::Free;
    portEXIT_CRITICAL(&slotMux);
    return result;
  }

  bool completionSignalPending = false;
  portENTER_CRITICAL(&slotMux);
  if (controlSlots[slotIndex].state == SlotState::Complete) {
    completionSignalPending = true;
  } else {
    controlSlots[slotIndex].state = SlotState::Abandoned;
  }
  portEXIT_CRITICAL(&slotMux);
  if (completionSignalPending) {
    xSemaphoreTake(controlSlots[slotIndex].done, portMAX_DELAY);
    portENTER_CRITICAL(&slotMux);
    result = controlSlots[slotIndex].result;
    controlSlots[slotIndex].state = SlotState::Free;
    portEXIT_CRITICAL(&slotMux);
    return result;
  }

  result.outcome = RtdbRequestService::Outcome::TimedOut;
  Serial.printf(
      "[RTDB_REQUEST] fail type=%s error=worker_timeout durationMs=%lu\n",
      requestTypeName(type),
      static_cast<unsigned long>(timeoutMs));
  return result;
}

}  // namespace

namespace RtdbRequestService {

const char* outcomeName(Outcome outcome) {
  switch (outcome) {
    case Outcome::Success: return "success";
    case Outcome::Failed: return "failed";
    case Outcome::Busy: return "busy";
    case Outcome::TimedOut: return "timed_out";
    case Outcome::WifiDisconnected: return "wifi_disconnected";
    default: return "unknown";
  }
}

bool begin() {
  if (requestTaskHandle != nullptr) {
    return true;
  }

  highQueue = xQueueCreate(
      RtdbRequestConfig::HIGH_QUEUE_LENGTH, sizeof(uint8_t));
  freeAudioQueue = xQueueCreate(
      RtdbUploadConfig::TX_QUEUE_LENGTH, sizeof(uint8_t));
  readyAudioQueue = xQueueCreate(
      RtdbUploadConfig::TX_QUEUE_LENGTH, sizeof(uint8_t));
  if (AvailabilityConfig::ENABLED) {
    lowDataMutex = xSemaphoreCreateMutex();
    initializePresenceSnapshot(presenceUsersReadSnapshot);
  }
  if (highQueue == nullptr || freeAudioQueue == nullptr ||
      readyAudioQueue == nullptr ||
      (AvailabilityConfig::ENABLED && lowDataMutex == nullptr)) {
    Serial.println("[ERROR] RTDB request service queue creation failed");
    return false;
  }

  for (uint8_t index = 0;
       index < RtdbRequestConfig::CONTROL_SLOT_COUNT;
       ++index) {
    controlSlots[index].done = xSemaphoreCreateBinary();
    if (controlSlots[index].done == nullptr) {
      Serial.println("[ERROR] RTDB request service semaphore creation failed");
      return false;
    }
  }
  for (uint8_t index = 0;
       index < RtdbUploadConfig::TX_QUEUE_LENGTH;
       ++index) {
    xQueueSend(freeAudioQueue, &index, portMAX_DELAY);
  }

  // Wi-Fi association is asynchronous.  Do not make application startup
  // depend on Firebase being reachable at this exact moment: otherwise setup()
  // halts before the GUI loop and button/communication tasks are started.
  // The RTDB worker initializes FirebaseClient lazily on the first stream or
  // write request after STA_GOT_IP; the Wi-Fi connected observer wakes it.
  if (!WifiConnection::isConnected()) {
    Serial.println(
        "[FIREBASE_CLIENT] initialization deferred until WiFi obtains IP");
  }

  const BaseType_t created = xTaskCreatePinnedToCore(
      requestTask,
      "rtdb_requests",
      RtdbRequestConfig::TASK_STACK_BYTES,
      nullptr,
      RtdbRequestConfig::TASK_PRIORITY,
      &requestTaskHandle,
      RtdbRequestConfig::TASK_CORE);
  if (created != pdPASS) {
    requestTaskHandle = nullptr;
    Serial.println("[ERROR] Failed creating RTDB request task");
    return false;
  }

  portENTER_CRITICAL(&serviceMux);
  requestRunning = true;
  portEXIT_CRITICAL(&serviceMux);
  Serial.printf(
      "[READY] FirebaseClient RTDB service oneTls=true asyncQueueLimit=1 firebaseInit=%s txBlocks=%u blockBytes=%u staticPcmBytes=%u freeHeap=%lu largestBlock=%lu\n",
      firebaseInitialized ? "ready" : "deferred",
      static_cast<unsigned int>(RtdbUploadConfig::TX_QUEUE_LENGTH),
      static_cast<unsigned int>(sizeof(AudioBlock)),
      static_cast<unsigned int>(sizeof(audioBlocks)),
      static_cast<unsigned long>(ESP.getFreeHeap()),
      static_cast<unsigned long>(
          heap_caps_get_largest_free_block(MALLOC_CAP_8BIT)));
  return true;
}

bool isRunning() {
  portENTER_CRITICAL(&serviceMux);
  const bool running = requestRunning;
  portEXIT_CRITICAL(&serviceMux);
  return running;
}

bool startAudioStream(uint8_t channel, uint32_t timeoutMs) {
  if (channel == 0) {
    Serial.println("[RTDB][STREAM] start rejected: no channel joined");
    return false;
  }
  const Result result = executeControl(
      RequestType::StreamStart, channel, "", 0, timeoutMs);
  return result.succeeded();
}

void stopAudioStream(uint32_t timeoutMs) {
  (void)executeControl(
      RequestType::StreamStop, 0, "", 0, timeoutMs);
}

bool scheduleSessionStart(uint8_t channel, const char* sessionId) {
  if (channel == 0) {
    Serial.println("[RTDB_REQUEST] fail type=SESSION_START error=no_channel_joined");
    return false;
  }
  const int slotIndex = allocateControlSlot(
      RequestType::SessionStart, channel, sessionId, 0);
  if (slotIndex < 0) {
    Serial.println("[RTDB_REQUEST] fail type=SESSION_START error=no_control_slot");
    return false;
  }

  portENTER_CRITICAL(&serviceMux);
  audioAbortRequested = false;
  audioRetryNotBeforeMs = 0;
  uploadFailure = false;
  consecutiveTransientAudioFailures = 0;
  totalTransientAudioFailures = 0;
  uploadSessionAuthorized = false;
  authorizedChannel = 0;
  authorizedSessionId[0] = '\0';
  portEXIT_CRITICAL(&serviceMux);

  const uint8_t queuedIndex = static_cast<uint8_t>(slotIndex);
  portENTER_CRITICAL(&slotMux);
  controlSlots[slotIndex].autoRelease = true;
  portEXIT_CRITICAL(&slotMux);
  if (xQueueSend(highQueue, &queuedIndex, 0) != pdTRUE) {
    portENTER_CRITICAL(&slotMux);
    controlSlots[slotIndex].state = SlotState::Free;
    portEXIT_CRITICAL(&slotMux);
    return false;
  }
  logQueueEnqueue(RequestType::SessionStart);
  if (requestTaskHandle != nullptr) {
    xTaskNotifyGive(requestTaskHandle);
  }
  return true;
}

Result endSession(
    uint8_t channel,
    const char* sessionId,
    uint32_t lastSeq,
    uint32_t timeoutMs) {
  return executeControl(
      RequestType::SessionEnd,
      channel,
      sessionId,
      lastSeq,
      timeoutMs);
}

void setAudioPriorityActive(bool active) {
  portENTER_CRITICAL(&serviceMux);
  audioPriority = active;
  portEXIT_CRITICAL(&serviceMux);
  if (requestTaskHandle != nullptr) {
    xTaskNotifyGive(requestTaskHandle);
  }
}

void setRecordingActive(bool active) {
  portENTER_CRITICAL(&serviceMux);
  recordingActive = active;
  portEXIT_CRITICAL(&serviceMux);
  if (requestTaskHandle != nullptr) {
    xTaskNotifyGive(requestTaskHandle);
  }
}

bool audioPriorityActive() {
  portENTER_CRITICAL(&serviceMux);
  const bool active = audioPriority;
  portEXIT_CRITICAL(&serviceMux);
  return active;
}

bool acquireAudioBlock(
    uint8_t& outIndex,
    int16_t*& outSamples,
    bool& outDroppedOldest) {
  outSamples = nullptr;
  outDroppedOldest = false;
  if (freeAudioQueue != nullptr &&
      xQueueReceive(freeAudioQueue, &outIndex, 0) == pdTRUE) {
    outSamples = audioBlocks[outIndex].samples;
    return true;
  }

  uint8_t reclaimedIndex = 0;
  if (readyAudioQueue != nullptr &&
      xQueueReceive(readyAudioQueue, &reclaimedIndex, 0) == pdTRUE) {
    const uint32_t droppedSequence = audioBlocks[reclaimedIndex].sequence;
    const size_t droppedSampleCount = audioBlocks[reclaimedIndex].sampleCount;
    char droppedSessionId[64] = {0};
    snprintf(
        droppedSessionId,
        sizeof(droppedSessionId),
        "%s",
        audioBlocks[reclaimedIndex].sessionId);
    addDiscardedAudioSamples(droppedSampleCount);
    resetAudioBlock(reclaimedIndex);
    outIndex = reclaimedIndex;
    outSamples = audioBlocks[outIndex].samples;
    outDroppedOldest = true;
    Serial.printf(
        "[AUDIO_TX] queue_full policy=drop_oldest droppedSession=%s droppedSeq=%lu readyDepth=%lu\n",
        droppedSessionId,
        static_cast<unsigned long>(droppedSequence),
        static_cast<unsigned long>(readyAudioDepth()));
    return true;
  }

  uint8_t inFlightCount = 0;
  portENTER_CRITICAL(&serviceMux);
  inFlightCount = uploadInFlightBlockCount;
  portEXIT_CRITICAL(&serviceMux);
  Serial.printf(
      "[AUDIO_TX] queue_exhausted readyDepth=%lu inFlight=%u freeDepth=0\n",
      static_cast<unsigned long>(readyAudioDepth()),
      static_cast<unsigned int>(inFlightCount));
  return false;
}

void releaseAudioBlock(uint8_t index) {
  returnAudioBlockToFreeQueue(index);
}

bool submitAudioBlock(
    uint8_t index,
    uint8_t channel,
    const char* sessionId,
    uint32_t sequence,
    size_t sampleCount,
    uint32_t recordedAtMs) {
  if (readyAudioQueue == nullptr ||
      index >= RtdbUploadConfig::TX_QUEUE_LENGTH ||
      channel == 0 || sessionId == nullptr || sessionId[0] == '\0' ||
      sampleCount == 0 || sampleCount > AudioConfig::CHUNK_SAMPLES) {
    addDiscardedAudioSamples(sampleCount);
    releaseAudioBlock(index);
    return false;
  }

  AudioBlock& block = audioBlocks[index];
  block.sampleCount = sampleCount;
  block.channel = channel;
  block.sequence = sequence;
  block.recordedAtMs = recordedAtMs;
  block.uploadAttempts = 0;
  snprintf(block.sessionId, sizeof(block.sessionId), "%s", sessionId);
  if (xQueueSend(readyAudioQueue, &index, 0) != pdTRUE) {
    addDiscardedAudioSamples(sampleCount);
    releaseAudioBlock(index);
    return false;
  }
  if (RtdbHttpConfig::LOG_EVERY_AUDIO_PACKET) {
    Serial.printf(
        "[AUDIO_TX] queued session=%s seq=%lu queueDepth=%lu\n",
        sessionId,
        static_cast<unsigned long>(sequence),
        static_cast<unsigned long>(readyAudioDepth()));
  }
  if (requestTaskHandle != nullptr) {
    xTaskNotifyGive(requestTaskHandle);
  }
  return true;
}

uint32_t audioQueueDepth() {
  uint32_t depth = readyAudioDepth();
  portENTER_CRITICAL(&serviceMux);
  depth += uploadInFlightBlockCount;
  portEXIT_CRITICAL(&serviceMux);
  return depth;
}

bool audioUploadsIdle() {
  portENTER_CRITICAL(&serviceMux);
  const bool inFlight = uploadInFlight;
  portEXIT_CRITICAL(&serviceMux);
  return readyAudioDepth() == 0 && !inFlight;
}

bool audioUploadFailed() {
  portENTER_CRITICAL(&serviceMux);
  const bool failed = uploadFailure;
  portEXIT_CRITICAL(&serviceMux);
  return failed;
}

uint32_t totalAudioUploadFailures() {
  portENTER_CRITICAL(&serviceMux);
  const uint32_t total = lifetimeAudioUploadFailures;
  portEXIT_CRITICAL(&serviceMux);
  return total;
}

uint64_t totalDiscardedAudioSamples() {
  portENTER_CRITICAL(&serviceMux);
  const uint64_t total = lifetimeDiscardedAudioSamples;
  portEXIT_CRITICAL(&serviceMux);
  return total;
}

void clearAudioUploadFailure() {
  portENTER_CRITICAL(&serviceMux);
  uploadFailure = false;
  audioAbortRequested = false;
  audioRetryNotBeforeMs = 0;
  consecutiveTransientAudioFailures = 0;
  totalTransientAudioFailures = 0;
  portEXIT_CRITICAL(&serviceMux);
}

void requestAudioUploadAbort(const char* reason) {
  portENTER_CRITICAL(&serviceMux);
  audioAbortRequested = true;
  portEXIT_CRITICAL(&serviceMux);
  Serial.printf(
      "[AUDIO_TX] abort_requested reason=%s queueDepth=%lu\n",
      reason == nullptr ? "unspecified" : reason,
      static_cast<unsigned long>(audioQueueDepth()));
  if (requestTaskHandle != nullptr) {
    xTaskNotifyGive(requestTaskHandle);
  }
}

void resumeAudioUploadsAfterReconnect() {
  portENTER_CRITICAL(&serviceMux);
  audioAbortRequested = false;
  audioRetryNotBeforeMs = 0;
  consecutiveTransientAudioFailures = 0;
  portEXIT_CRITICAL(&serviceMux);
  Serial.printf(
      "[AUDIO_TX] transport_resumed reason=wifi_restored queueDepth=%lu\n",
      static_cast<unsigned long>(audioQueueDepth()));
  if (requestTaskHandle != nullptr) {
    xTaskNotifyGive(requestTaskHandle);
  }
}

uint32_t discardPendingAudio(const char* reason) {
  uint32_t discarded = 0;
  uint8_t index = 0;
  while (readyAudioQueue != nullptr &&
         xQueueReceive(readyAudioQueue, &index, 0) == pdTRUE) {
    addDiscardedAudioSamples(audioBlocks[index].sampleCount);
    returnAudioBlockToFreeQueue(index);
    ++discarded;
  }
  if (discarded > 0) {
    Serial.printf(
        "[AUDIO_TX] discard_pending count=%lu reason=%s\n",
        static_cast<unsigned long>(discarded),
        reason == nullptr ? "unspecified" : reason);
  }
  return discarded;
}

bool schedulePresenceLease(
    const char* jsonPayload,
    uint32_t validForMs) {
  if (!AvailabilityConfig::ENABLED || jsonPayload == nullptr ||
      jsonPayload[0] == '\0' || lowDataMutex == nullptr ||
      validForMs == 0) {
    return false;
  }

  xSemaphoreTake(lowDataMutex, portMAX_DELAY);
  // Coalesce multiple context changes into one latest lease payload. A caller
  // never allocates another queue entry and the worker sees only the newest
  // state once the shared Firebase connection becomes safe.
  presenceLeasePending = true;
  presenceLeaseDeferredLogged = false;
  presenceLeaseExpiresAtMs = millis() + validForMs;
  snprintf(
      pendingPresenceLeasePayload,
      sizeof(pendingPresenceLeasePayload),
      "%s",
      jsonPayload);
  xSemaphoreGive(lowDataMutex);

  logQueueEnqueue(RequestType::PresenceLease);
  if (requestTaskHandle != nullptr) {
    xTaskNotifyGive(requestTaskHandle);
  }
  return true;
}

bool schedulePresenceUsersRead(uint32_t validForMs) {
  if (!AvailabilityConfig::ENABLED || lowDataMutex == nullptr ||
      validForMs == 0) {
    return false;
  }

  xSemaphoreTake(lowDataMutex, portMAX_DELAY);
  if (presenceUsersReadPending) {
    // Refresh the expiration without creating duplicate work.
    presenceUsersReadExpiresAtMs = millis() + validForMs;
    xSemaphoreGive(lowDataMutex);
    return true;
  }
  presenceUsersReadPending = true;
  ++presenceUsersReadGeneration;
  if (presenceUsersReadGeneration == 0) {
    presenceUsersReadGeneration = 1;
  }
  presenceUsersReadExpiresAtMs = millis() + validForMs;
  xSemaphoreGive(lowDataMutex);

  logQueueEnqueue(RequestType::PresenceUsersRead);
  if (requestTaskHandle != nullptr) {
    xTaskNotifyGive(requestTaskHandle);
  }
  return true;
}

void cancelPresenceUsersRead() {
  if (!AvailabilityConfig::ENABLED || lowDataMutex == nullptr) {
    return;
  }
  xSemaphoreTake(lowDataMutex, portMAX_DELAY);
  ++presenceUsersReadGeneration;
  if (presenceUsersReadGeneration == 0) {
    presenceUsersReadGeneration = 1;
  }
  presenceUsersReadPending = false;
  presenceUsersReadExpiresAtMs = 0;
  presenceUsersReadResultReady = false;
  xSemaphoreGive(lowDataMutex);
}

bool takePresenceLeaseResult(bool& outSuccess) {
  if (!AvailabilityConfig::ENABLED || lowDataMutex == nullptr) {
    return false;
  }
  xSemaphoreTake(lowDataMutex, portMAX_DELAY);
  const bool ready = presenceLeaseResultReady;
  if (ready) {
    outSuccess = presenceLeaseLastResult;
    presenceLeaseResultReady = false;
  }
  xSemaphoreGive(lowDataMutex);
  return ready;
}

bool takePresenceUsersResult(
    bool& outSuccess,
    PresenceSnapshot& outSnapshot) {
  if (!AvailabilityConfig::ENABLED || lowDataMutex == nullptr) {
    return false;
  }
  xSemaphoreTake(lowDataMutex, portMAX_DELAY);
  const bool ready = presenceUsersReadResultReady;
  if (ready) {
    outSuccess = presenceUsersReadLastResult;
    outSnapshot = presenceUsersReadSnapshot;
    presenceUsersReadResultReady = false;
  }
  xSemaphoreGive(lowDataMutex);
  return ready;
}

}  // namespace RtdbRequestService
