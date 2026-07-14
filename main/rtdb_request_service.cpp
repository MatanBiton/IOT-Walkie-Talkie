#include "rtdb_request_service.h"

#include <HTTPClient.h>
#include <WiFiClientSecure.h>
#include <ctype.h>
#include <esp_heap_caps.h>
#include <freertos/FreeRTOS.h>
#include <freertos/queue.h>
#include <freertos/semphr.h>
#include <freertos/task.h>
#include <mbedtls/base64.h>
#include <string.h>
#include <strings.h>
#include <stdlib.h>
#include <sys/time.h>
#include <errno.h>
#include <new>

#include "app_config.h"
#include "wifi_connection.h"

namespace {

enum class RequestType : uint8_t {
  SessionStart,
  AudioBatch,
  SessionEnd,
  AvailabilityHeartbeat,
  AvailabilityUsersRead,
};

enum class SlotState : uint8_t {
  Free,
  Queued,
  Running,
  Complete,
  Abandoned,
};

enum class HttpMethod : uint8_t {
  Get,
  Put,
  Patch,
  Delete,
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
  char sessionId[64] = {0};
};

struct HttpResponse {
  int code = 0;
  String body;
  String etag;
  uint32_t durationMs = 0;
};


TaskHandle_t requestTaskHandle = nullptr;
QueueHandle_t highQueue = nullptr;
QueueHandle_t lowQueue = nullptr;
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
bool wifiRecoveryRequestedForSession = false;
bool audioAbortRequested = false;
bool uploadSessionAuthorized = false;
uint8_t authorizedChannel = 0;
char authorizedSessionId[64] = {0};


bool heartbeatPending = false;
bool heartbeatResultReady = false;
bool heartbeatLastResult = false;
bool heartbeatDeferredLogged = false;
char pendingHeartbeatPayload[640] = {0};

bool usersReadPending = false;
bool usersReadResultReady = false;
bool usersReadLastResult = false;
String usersReadResponse;

constexpr size_t FIREBASE_HOST_BYTES = 160;
constexpr size_t AUDIO_REQUEST_PATH_BYTES = 256;
constexpr size_t AUDIO_HTTP_HEADER_BYTES = 512;
constexpr size_t AUDIO_HTTP_LINE_BYTES = 384;
constexpr size_t AUDIO_UPLOAD_JSON_BYTES = AudioConfig::BASE64_BUFFER_BYTES + 640;

WiFiClientSecure audioUploadClient;
bool audioUploadConnectionOpen = false;
char firebaseHost[FIREBASE_HOST_BYTES] = {0};
char audioUploadRequestPath[AUDIO_REQUEST_PATH_BYTES] = {0};
char audioUploadConnectionSessionId[64] = {0};
uint8_t audioUploadConnectionChannel = 0;
alignas(4) char audioUploadJson[AUDIO_UPLOAD_JSON_BYTES] = {0};

void storeHeartbeatResult(bool success);

const char* requestTypeName(RequestType type) {
  switch (type) {
    case RequestType::SessionStart: return "SESSION_START";
    case RequestType::AudioBatch: return "AUDIO_BATCH";
    case RequestType::SessionEnd: return "SESSION_END";
    case RequestType::AvailabilityHeartbeat: return "AVAILABILITY_HEARTBEAT";
    case RequestType::AvailabilityUsersRead: return "AVAILABILITY_USERS_READ";
    default: return "UNKNOWN";
  }
}

const char* priorityName(RequestType type) {
  return (type == RequestType::AvailabilityHeartbeat ||
          type == RequestType::AvailabilityUsersRead)
             ? "low"
             : "high";
}

const char* httpErrorName(int code) {
  switch (code) {
    case HTTPC_ERROR_CONNECTION_REFUSED: return "CONNECTION_REFUSED";
    case HTTPC_ERROR_SEND_HEADER_FAILED: return "SEND_HEADER_FAILED";
    case HTTPC_ERROR_SEND_PAYLOAD_FAILED: return "SEND_PAYLOAD_FAILED";
    case HTTPC_ERROR_NOT_CONNECTED: return "NOT_CONNECTED";
    case HTTPC_ERROR_CONNECTION_LOST: return "CONNECTION_LOST";
    case HTTPC_ERROR_NO_STREAM: return "NO_STREAM";
    case HTTPC_ERROR_NO_HTTP_SERVER: return "NO_HTTP_SERVER";
    case HTTPC_ERROR_TOO_LESS_RAM: return "TOO_LESS_RAM";
    case HTTPC_ERROR_ENCODING: return "ENCODING";
    case HTTPC_ERROR_STREAM_WRITE: return "STREAM_WRITE";
    case HTTPC_ERROR_READ_TIMEOUT: return "READ_TIMEOUT";
    case 412: return "PRECONDITION_FAILED";
    default: return code > 0 ? "HTTP_STATUS" : "HTTP_ERROR";
  }
}

uint32_t highDepth() {
  return highQueue == nullptr ? 0 : uxQueueMessagesWaiting(highQueue);
}

uint32_t lowDepth() {
  return lowQueue == nullptr ? 0 : uxQueueMessagesWaiting(lowQueue);
}

uint32_t readyAudioDepth() {
  return readyAudioQueue == nullptr ? 0 : uxQueueMessagesWaiting(readyAudioQueue);
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
  block.sessionId[0] = '\0';
}

void returnAudioBlockToFreeQueue(uint8_t index) {
  if (freeAudioQueue == nullptr ||
      index >= RtdbUploadConfig::TX_QUEUE_LENGTH) {
    return;
  }
  resetAudioBlock(index);
  xQueueSend(freeAudioQueue, &index, portMAX_DELAY);
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

const char* audioBatchOutcomeName(AudioBatchOutcome outcome) {
  switch (outcome) {
    case AudioBatchOutcome::Success: return "success";
    case AudioBatchOutcome::TransientFailure: return "transient_failure";
    case AudioBatchOutcome::FatalFailure: return "fatal_failure";
    default: return "unknown";
  }
}

bool isTransientAudioStatus(int statusCode) {
  return statusCode <= 0 ||
         statusCode == 408 ||
         statusCode == 425 ||
         statusCode == 429 ||
         statusCode >= 500;
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
      "[RTDB_HEAP] type=%s phase=%s free=%lu largest=%lu minimum=%lu\n",
      requestTypeName(type),
      phase,
      static_cast<unsigned long>(ESP.getFreeHeap()),
      static_cast<unsigned long>(heap_caps_get_largest_free_block(MALLOC_CAP_8BIT)),
      static_cast<unsigned long>(heap_caps_get_minimum_free_size(MALLOC_CAP_8BIT)));
}

String databaseBaseUrl() {
  String base = FirebaseConfig::DATABASE_URL;
  while (base.endsWith("/")) {
    base.remove(base.length() - 1);
  }
  return base;
}

String rtdbUrlForPath(const String& path, bool silentWrite) {
  String url = databaseBaseUrl();
  if (path != "/") {
    url += path;
  }
  url += ".json";
  if (silentWrite) {
    url += "?print=silent";
  }
  return url;
}

String channelKey(uint8_t channel) {
  char buffer[8];
  snprintf(buffer, sizeof(buffer), "ch%02u", static_cast<unsigned int>(channel));
  return String(buffer);
}

String channelPath(uint8_t channel) {
  return String("/rooms/") + AppConfig::ROOM_ID + "/channels/" + channelKey(channel);
}

String talkersPath(uint8_t channel) {
  return channelPath(channel) + "/talkers";
}

String livePath(uint8_t channel) {
  return talkersPath(channel) + "/" + AppConfig::DEVICE_ID + "/live";
}

String userPath() {
  return String(AvailabilityConfig::RTDB_USERS_PATH) + "/user_" + AppConfig::USER_ID;
}

HttpResponse performHttp(
    RequestType type,
    uint8_t attempt,
    HttpMethod method,
    const String& path,
    const String& payload,
    bool silentWrite,
    const char* conditionalEtag,
    bool requestEtag) {
      Serial.printf(
      "[RTDB_REQUEST][ENTER] type=%s attempt=%u path=%s "
      "freeHeap=%lu stackRemaining=%lu\n",
      requestTypeName(type),
      static_cast<unsigned int>(attempt),
      path.c_str(),
      static_cast<unsigned long>(ESP.getFreeHeap()),
      static_cast<unsigned long>(
          uxTaskGetStackHighWaterMark(nullptr)));
  HttpResponse response;
  const uint32_t startMs = millis();

  Serial.printf(
      "[RTDB_REQUEST] start type=%s attempt=%u path=%s\n",
      requestTypeName(type),
      static_cast<unsigned int>(attempt),
      path.c_str());

  if (!WifiConnection::isConnected()) {
    response.code = HTTPC_ERROR_NOT_CONNECTED;
    Serial.printf(
        "[RTDB_REQUEST] fail type=%s http=%d error=wifi_disconnected durationMs=0\n",
        requestTypeName(type),
        response.code);
    return response;
  }

  WiFiClientSecure client;
  client.setInsecure();
  HTTPClient http;
  http.setFollowRedirects(HTTPC_STRICT_FOLLOW_REDIRECTS);
  http.useHTTP10(true);
  const bool availabilityRequest =
      type == RequestType::AvailabilityHeartbeat ||
      type == RequestType::AvailabilityUsersRead;
  http.setTimeout(
      availabilityRequest ? AvailabilityConfig::RTDB_HTTP_TIMEOUT_MS
                          : RtdbRequestConfig::SHORT_HTTP_TIMEOUT_MS);
  http.setConnectTimeout(
      availabilityRequest ? AvailabilityConfig::RTDB_CONNECT_TIMEOUT_MS
                          : RtdbRequestConfig::SHORT_CONNECT_TIMEOUT_MS);

  logHeap(type, "before_connect");
  const String url = rtdbUrlForPath(path, silentWrite);
  if (!http.begin(client, url)) {
    response.code = HTTPC_ERROR_NO_HTTP_SERVER;
    response.durationMs = millis() - startMs;
    // Keep cleanup explicit on every exit path, including partially initialized
    // HTTP/TLS state.
    http.end();
    client.stop();
    logHeap(type, "after_connect_failed");
    Serial.printf(
        "[RTDB_REQUEST] fail type=%s http=%d error=begin_failed durationMs=%lu\n",
        requestTypeName(type),
        response.code,
        static_cast<unsigned long>(response.durationMs));
    return response;
  }

  // collectHeaders() allocates internal header storage. Session start/end never
  // request an ETag, so avoid that allocation and its heap-fragmentation cost
  // on every control request.
  if (requestEtag) {
    const char* collectedHeaders[] = {"ETag"};
    http.collectHeaders(collectedHeaders, 1);
    http.addHeader("X-Firebase-ETag", "true");
  }
  if (conditionalEtag != nullptr && conditionalEtag[0] != '\0') {
    http.addHeader("If-Match", conditionalEtag);
  }
  http.addHeader("Connection", "close");
  if (method == HttpMethod::Put || method == HttpMethod::Patch) {
    http.addHeader("Content-Type", "application/json");
  }

  switch (method) {
    case HttpMethod::Get:
      response.code = http.GET();
      break;
    case HttpMethod::Put:
      response.code = http.PUT(payload);
      break;
    case HttpMethod::Patch:
      response.code = http.sendRequest(
          "PATCH",
          reinterpret_cast<uint8_t*>(const_cast<char*>(payload.c_str())),
          payload.length());
      break;
    case HttpMethod::Delete:
      response.code = http.sendRequest("DELETE");
      break;
  }

  if (method == HttpMethod::Get || response.code < 200 || response.code >= 300) {
    response.body = http.getString();
  }
  if (requestEtag) {
    response.etag = http.header("ETag");
  }
  response.durationMs = millis() - startMs;
  logHeap(type, "after_connect_request");

  if (response.code >= 200 && response.code < 300) {
    Serial.printf(
        "[RTDB_REQUEST] success type=%s http=%d durationMs=%lu\n",
        requestTypeName(type),
        response.code,
        static_cast<unsigned long>(response.durationMs));
  } else {
    Serial.printf(
        "[RTDB_REQUEST] fail type=%s http=%d error=%s durationMs=%lu\n",
        requestTypeName(type),
        response.code,
        httpErrorName(response.code),
        static_cast<unsigned long>(response.durationMs));
    if (response.body.length() > 0 && RtdbHttpConfig::LOG_HTTP_REQUESTS) {
      Serial.printf(
          "[RTDB_REQUEST] response type=%s bytes=%u body=%s\n",
          requestTypeName(type),
          static_cast<unsigned int>(response.body.length()),
          response.body.c_str());
    }
  }
  http.end();
  client.stop();
  logHeap(type, "after_cleanup");
  return response;
}

RtdbRequestService::Result performSessionStart(uint8_t channel, const char* sessionId) {
  RtdbRequestService::Result result;
  String patch;
  // SESSION_START is serialized ahead of audio uploads, so this atomic PATCH
  // can safely clear stale chunks before authorizing the new session.
  patch.reserve(210);
  patch += "{\"meta\":{\"active\":true,\"sessionId\":\"";
  patch += sessionId;
  patch += "\",\"deviceId\":\"";
  patch += AppConfig::DEVICE_ID;
  patch += "\",\"sampleRate\":";
  patch += AudioConfig::SAMPLE_RATE;
  patch += ",\"chunkMs\":";
  patch += AudioConfig::CHUNK_MS;
  // Audio requests are serialized behind SESSION_START, so clearing stale
  // chunks here cannot race with a new upload. This also keeps the SSE root
  // snapshot bounded across repeated transmissions.
  patch += "},\"chunks\":null}";

  for (uint8_t attempt = 1; attempt <= RtdbHttpConfig::CONTROL_REQUEST_MAX_ATTEMPTS; ++attempt) {
    const HttpResponse response = performHttp(
        RequestType::SessionStart,
        attempt,
        HttpMethod::Patch,
        livePath(channel),
        patch,
        true,
        nullptr,
        false);
    result.httpCode = response.code;
    if (response.code >= 200 && response.code < 300) {
      result.outcome = RtdbRequestService::Outcome::Success;
      return result;
    }
    if (!WifiConnection::isConnected()) {
      result.outcome = RtdbRequestService::Outcome::WifiDisconnected;
      return result;
    }
    if (attempt < RtdbHttpConfig::CONTROL_REQUEST_MAX_ATTEMPTS) {
      vTaskDelay(pdMS_TO_TICKS(RtdbHttpConfig::CONTROL_RETRY_DELAY_MS));
    }
  }
  result.outcome = RtdbRequestService::Outcome::Failed;
  return result;
}

RtdbRequestService::Result performSessionEnd(
    uint8_t channel,
    const char* sessionId,
    uint32_t lastSeq) {
  RtdbRequestService::Result result;
  String patch;
  patch.reserve(210);
  patch += "{\"meta\":{\"active\":false,\"sessionId\":\"";
  patch += sessionId;
  patch += "\",\"deviceId\":\"";
  patch += AppConfig::DEVICE_ID;
  patch += "\",\"lastSeq\":";
  patch += lastSeq;
  patch += ",\"sampleRate\":";
  patch += AudioConfig::SAMPLE_RATE;
  patch += ",\"chunkMs\":";
  patch += AudioConfig::CHUNK_MS;
  // Remove completed PCM after every listener has received the ordered
  // chunk events. Otherwise reconnecting to /talkers downloads an ever-growing
  // historical snapshot and fragments the ESP32 heap.
  patch += "},\"chunks\":null}";

  for (uint8_t attempt = 1; attempt <= RtdbHttpConfig::CONTROL_REQUEST_MAX_ATTEMPTS; ++attempt) {
    const HttpResponse response = performHttp(
        RequestType::SessionEnd,
        attempt,
        HttpMethod::Patch,
        livePath(channel),
        patch,
        true,
        nullptr,
        false);
    result.httpCode = response.code;
    if (response.code >= 200 && response.code < 300) {
      result.outcome = RtdbRequestService::Outcome::Success;
      return result;
    }
    if (!WifiConnection::isConnected()) {
      result.outcome = RtdbRequestService::Outcome::WifiDisconnected;
      return result;
    }
    if (attempt < RtdbHttpConfig::CONTROL_REQUEST_MAX_ATTEMPTS) {
      vTaskDelay(pdMS_TO_TICKS(RtdbHttpConfig::CONTROL_RETRY_DELAY_MS));
    }
  }
  result.outcome = RtdbRequestService::Outcome::Failed;
  return result;
}

bool initializeFirebaseHost() {
  const char* url = FirebaseConfig::DATABASE_URL;
  if (url == nullptr || url[0] == '\0') {
    return false;
  }

  const char* hostStart = strstr(url, "://");
  hostStart = hostStart == nullptr ? url : hostStart + 3;
  const char* hostEnd = strchr(hostStart, '/');
  const size_t hostLength = hostEnd == nullptr
                                ? strlen(hostStart)
                                : static_cast<size_t>(hostEnd - hostStart);
  if (hostLength == 0 || hostLength >= sizeof(firebaseHost)) {
    return false;
  }

  memcpy(firebaseHost, hostStart, hostLength);
  firebaseHost[hostLength] = '\0';
  return true;
}

void resetAudioUploadClient() {
  // stop() releases the active socket/TLS buffers, but some Arduino-ESP32 core
  // versions retain per-client bookkeeping until the client is destroyed.
  // Reconstructing the long-lived client after every closed session guarantees
  // that all internal allocations are returned immediately instead of being
  // carried into the next TLS handshake.
  audioUploadClient.stop();
  audioUploadClient.~WiFiClientSecure();
  new (&audioUploadClient) WiFiClientSecure();
}

void closeAudioUploadConnection(const char* reason) {
  const bool wasOpen = audioUploadConnectionOpen || audioUploadClient.connected();
  resetAudioUploadClient();
  audioUploadConnectionOpen = false;
  if (wasOpen && RtdbHttpConfig::LOG_HTTP_REQUESTS) {
    Serial.printf(
        "[AUDIO_TX] persistent_connection closed reason=%s freeHeap=%lu largestBlock=%lu\n",
        reason == nullptr ? "unspecified" : reason,
        static_cast<unsigned long>(ESP.getFreeHeap()),
        static_cast<unsigned long>(
            heap_caps_get_largest_free_block(MALLOC_CAP_8BIT)));
  }
}

bool prepareAudioUploadSession(uint8_t channel, const char* sessionId) {
  closeAudioUploadConnection("prepare_session");
  const int written = snprintf(
      audioUploadRequestPath,
      sizeof(audioUploadRequestPath),
      "/rooms/%s/channels/ch%02u/talkers/%s/live/chunks.json?print=silent",
      AppConfig::ROOM_ID,
      static_cast<unsigned int>(channel),
      AppConfig::DEVICE_ID);
  if (written <= 0 || static_cast<size_t>(written) >= sizeof(audioUploadRequestPath)) {
    audioUploadRequestPath[0] = '\0';
    audioUploadConnectionSessionId[0] = '\0';
    audioUploadConnectionChannel = 0;
    Serial.println("[AUDIO_TX] session_prepare_failed reason=request_path_too_long");
    return false;
  }

  audioUploadConnectionChannel = channel;
  snprintf(
      audioUploadConnectionSessionId,
      sizeof(audioUploadConnectionSessionId),
      "%s",
      sessionId == nullptr ? "" : sessionId);
  return audioUploadConnectionSessionId[0] != '\0';
}

bool readAudioHttpLine(char* out, size_t outSize, uint32_t timeoutMs) {
  if (out == nullptr || outSize < 2) {
    return false;
  }

  size_t used = 0;
  bool overflow = false;
  const uint32_t startedAtMs = millis();
  while ((millis() - startedAtMs) < timeoutMs) {
    if (isAudioAbortRequested()) {
      return false;
    }
    while (audioUploadClient.available() > 0) {
      const int value = audioUploadClient.read();
      if (value < 0) {
        continue;
      }
      if (value == '\r') {
        continue;
      }
      if (value == '\n') {
        out[used] = '\0';
        return !overflow;
      }
      if (used + 1 < outSize) {
        out[used++] = static_cast<char>(value);
      } else {
        overflow = true;
      }
    }

    if (!audioUploadClient.connected()) {
      return false;
    }
    vTaskDelay(pdMS_TO_TICKS(1));
  }
  return false;
}

bool drainAudioHttpBytes(size_t byteCount, uint32_t timeoutMs) {
  const uint32_t startedAtMs = millis();
  size_t drained = 0;
  while (drained < byteCount && (millis() - startedAtMs) < timeoutMs) {
    if (isAudioAbortRequested()) {
      return false;
    }
    while (audioUploadClient.available() > 0 && drained < byteCount) {
      audioUploadClient.read();
      ++drained;
    }
    if (drained >= byteCount) {
      return true;
    }
    if (!audioUploadClient.connected()) {
      return false;
    }
    vTaskDelay(pdMS_TO_TICKS(1));
  }
  return drained == byteCount;
}

bool drainChunkedAudioResponse(uint32_t timeoutMs) {
  char line[AUDIO_HTTP_LINE_BYTES];
  for (;;) {
    if (!readAudioHttpLine(line, sizeof(line), timeoutMs)) {
      return false;
    }
    char* extension = strchr(line, ';');
    if (extension != nullptr) {
      *extension = '\0';
    }
    const unsigned long chunkBytes = strtoul(line, nullptr, 16);
    if (chunkBytes == 0) {
      // Consume optional trailer headers.
      do {
        if (!readAudioHttpLine(line, sizeof(line), timeoutMs)) {
          return false;
        }
      } while (line[0] != '\0');
      return true;
    }
    if (!drainAudioHttpBytes(static_cast<size_t>(chunkBytes), timeoutMs)) {
      return false;
    }
    if (!readAudioHttpLine(line, sizeof(line), timeoutMs) || line[0] != '\0') {
      return false;
    }
  }
}

int readAudioHttpResponse(bool& outConnectionClose) {
  outConnectionClose = false;
  char line[AUDIO_HTTP_LINE_BYTES];
  if (!readAudioHttpLine(
          line,
          sizeof(line),
          RtdbUploadConfig::UPLOAD_RESPONSE_TIMEOUT_MS)) {
    return HTTPC_ERROR_READ_TIMEOUT;
  }

  int statusCode = 0;
  if (sscanf(line, "HTTP/%*s %d", &statusCode) != 1 || statusCode <= 0) {
    return HTTPC_ERROR_ENCODING;
  }

  bool chunked = false;
  bool haveContentLength = false;
  size_t contentLength = 0;
  for (;;) {
    if (!readAudioHttpLine(
            line,
            sizeof(line),
            RtdbUploadConfig::UPLOAD_RESPONSE_TIMEOUT_MS)) {
      return HTTPC_ERROR_READ_TIMEOUT;
    }
    if (line[0] == '\0') {
      break;
    }
    if (strncasecmp(line, "Content-Length:", 15) == 0) {
      contentLength = static_cast<size_t>(strtoul(line + 15, nullptr, 10));
      haveContentLength = true;
    } else if (strncasecmp(line, "Transfer-Encoding:", 18) == 0 &&
               strstr(line + 18, "chunked") != nullptr) {
      chunked = true;
    } else if (strncasecmp(line, "Connection:", 11) == 0 &&
               strstr(line + 11, "close") != nullptr) {
      outConnectionClose = true;
    }
  }

  bool bodyDrained = true;
  if (chunked) {
    bodyDrained = drainChunkedAudioResponse(
        RtdbUploadConfig::UPLOAD_RESPONSE_TIMEOUT_MS);
  } else if (haveContentLength && contentLength > 0) {
    bodyDrained = drainAudioHttpBytes(
        contentLength,
        RtdbUploadConfig::UPLOAD_RESPONSE_TIMEOUT_MS);
  }
  return bodyDrained ? statusCode : HTTPC_ERROR_READ_TIMEOUT;
}

bool writeAudioClientAll(const uint8_t* data, size_t length, uint32_t timeoutMs) {
  if (data == nullptr && length > 0) {
    return false;
  }
  const uint32_t startedAtMs = millis();
  size_t sent = 0;
  while (sent < length && (millis() - startedAtMs) < timeoutMs) {
    if (isAudioAbortRequested()) {
      return false;
    }
    const size_t written = audioUploadClient.write(data + sent, length - sent);
    if (written > 0) {
      sent += written;
      continue;
    }
    if (!audioUploadClient.connected()) {
      return false;
    }
    vTaskDelay(pdMS_TO_TICKS(1));
  }
  return sent == length;
}

bool ensureAudioUploadConnection() {
  if (isAudioAbortRequested()) {
    closeAudioUploadConnection("abort_before_connect");
    return false;
  }
  if (audioUploadConnectionOpen && audioUploadClient.connected()) {
    return true;
  }
  if (firebaseHost[0] == '\0' || audioUploadRequestPath[0] == '\0') {
    return false;
  }

  closeAudioUploadConnection("reconnect");
  audioUploadClient.setInsecure();
  audioUploadClient.setTimeout(RtdbUploadConfig::UPLOAD_RESPONSE_TIMEOUT_MS);
  audioUploadClient.setHandshakeTimeout(
      RtdbUploadConfig::UPLOAD_TLS_HANDSHAKE_TIMEOUT_SECONDS);
  logHeap(RequestType::AudioBatch, "before_persistent_tls_connect");
  if (!audioUploadClient.connect(
          firebaseHost,
          443,
          RtdbUploadConfig::UPLOAD_CONNECT_TIMEOUT_MS)) {
    // A failed TLS setup may have allocated part of the mbedTLS context. Always
    // destroy that partial context before retrying. Merely setting the open flag
    // to false is not enough on every Arduino-ESP32 core version.
    closeAudioUploadConnection("connect_failed");
    logHeap(RequestType::AudioBatch, "persistent_tls_connect_failed");
    return false;
  }
  if (isAudioAbortRequested()) {
    closeAudioUploadConnection("abort_after_connect");
    return false;
  }
  audioUploadConnectionOpen = true;
  logHeap(RequestType::AudioBatch, "after_persistent_tls_connect");
  Serial.printf(
      "[AUDIO_TX] persistent_connection open host=%s channel=%u session=%s\n",
      firebaseHost,
      static_cast<unsigned int>(audioUploadConnectionChannel),
      audioUploadConnectionSessionId);
  return true;
}

int performPersistentAudioPatch(
    const uint8_t* payload,
    size_t payloadLength,
    uint8_t attempt) {
  const uint32_t startedAtMs = millis();
  Serial.printf(
      "[RTDB_REQUEST] start type=AUDIO_BATCH attempt=%u path=%s persistent=true payloadBytes=%u\n",
      static_cast<unsigned int>(attempt),
      audioUploadRequestPath,
      static_cast<unsigned int>(payloadLength));

  if (!WifiConnection::isConnected()) {
    return HTTPC_ERROR_NOT_CONNECTED;
  }
  if (isAudioAbortRequested()) {
    return HTTPC_ERROR_CONNECTION_LOST;
  }
  if (!ensureAudioUploadConnection()) {
    return HTTPC_ERROR_CONNECTION_REFUSED;
  }

  char header[AUDIO_HTTP_HEADER_BYTES];
  const int headerLength = snprintf(
      header,
      sizeof(header),
      "PATCH %s HTTP/1.1\r\n"
      "Host: %s\r\n"
      "User-Agent: esp32-rtdb-audio/1.0\r\n"
      "Accept: */*\r\n"
      "Content-Type: application/json\r\n"
      "Content-Length: %u\r\n"
      "Connection: keep-alive\r\n\r\n",
      audioUploadRequestPath,
      firebaseHost,
      static_cast<unsigned int>(payloadLength));
  if (headerLength <= 0 || static_cast<size_t>(headerLength) >= sizeof(header)) {
    closeAudioUploadConnection("header_overflow");
    return HTTPC_ERROR_TOO_LESS_RAM;
  }

  const bool headerSent = writeAudioClientAll(
      reinterpret_cast<const uint8_t*>(header),
      static_cast<size_t>(headerLength),
      RtdbUploadConfig::UPLOAD_RESPONSE_TIMEOUT_MS);
  const bool payloadSent = headerSent && writeAudioClientAll(
      payload,
      payloadLength,
      RtdbUploadConfig::UPLOAD_RESPONSE_TIMEOUT_MS);
  if (!payloadSent) {
    closeAudioUploadConnection("request_write_failed");
    return HTTPC_ERROR_SEND_PAYLOAD_FAILED;
  }

  bool serverRequestedClose = false;
  const int statusCode = readAudioHttpResponse(serverRequestedClose);
  const uint32_t durationMs = millis() - startedAtMs;
  if (statusCode >= 200 && statusCode < 300) {
    Serial.printf(
        "[RTDB_REQUEST] success type=AUDIO_BATCH http=%d durationMs=%lu persistent=true\n",
        statusCode,
        static_cast<unsigned long>(durationMs));
  } else {
    Serial.printf(
        "[RTDB_REQUEST] fail type=AUDIO_BATCH http=%d error=%s durationMs=%lu persistent=true\n",
        statusCode,
        httpErrorName(statusCode),
        static_cast<unsigned long>(durationMs));
  }

  if (serverRequestedClose || statusCode < 200 || statusCode >= 300 ||
      !audioUploadClient.connected()) {
    closeAudioUploadConnection(
        serverRequestedClose ? "server_close" : "request_failed");
  }
  return statusCode;
}

bool buildSingleAudioPatchJson(
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
      "{\"%08lu\":{\"seq\":%lu,\"sessionId\":\"%s\",\"deviceId\":\"%s\","
      "\"format\":\"pcm_s16le_base64\",\"sampleRate\":%lu,\"chunkMs\":%u,\"data\":\"",
      static_cast<unsigned long>(block.sequence),
      static_cast<unsigned long>(block.sequence),
      block.sessionId,
      AppConfig::DEVICE_ID,
      static_cast<unsigned long>(AudioConfig::SAMPLE_RATE),
      static_cast<unsigned int>(AudioConfig::CHUNK_MS));
  constexpr size_t suffixBytes = 3;  // quote + inner brace + outer brace
  if (prefixLength <= 0 ||
      static_cast<size_t>(prefixLength) + suffixBytes + 1 >= outSize) {
    return false;
  }

  size_t outputLength = 0;
  const size_t pcmBytes = block.sampleCount * sizeof(int16_t);
  const size_t available = outSize - static_cast<size_t>(prefixLength) - suffixBytes - 1;
  const int result = mbedtls_base64_encode(
      reinterpret_cast<unsigned char*>(out + prefixLength),
      available,
      &outputLength,
      reinterpret_cast<const unsigned char*>(block.samples),
      pcmBytes);
  if (result != 0 || outputLength > available) {
    Serial.printf(
        "[AUDIO_TX] encode_fail seq=%lu rc=%d bytes=%u available=%u\n",
        static_cast<unsigned long>(block.sequence),
        result,
        static_cast<unsigned int>(pcmBytes),
        static_cast<unsigned int>(available));
    return false;
  }

  size_t cursor = static_cast<size_t>(prefixLength) + outputLength;
  out[cursor++] = '"';
  out[cursor++] = '}';
  out[cursor++] = '}';
  out[cursor] = '\0';
  outLength = cursor;
  encodedBytes = outputLength;
  return true;
}

bool isAudioSessionAuthorized(const AudioBlock& block) {
  bool authorized = false;
  portENTER_CRITICAL(&serviceMux);
  authorized = uploadSessionAuthorized &&
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

  const AudioBlock& block = audioBlocks[indices[0]];
  if (!isAudioSessionAuthorized(block)) {
    Serial.printf(
        "[AUDIO_TX] batch_abort reason=session_start_not_authorized channel=%u session=%s firstSeq=%lu\n",
        static_cast<unsigned int>(block.channel),
        block.sessionId,
        static_cast<unsigned long>(block.sequence));
    return AudioBatchOutcome::FatalFailure;
  }
  if (block.channel == 0 || block.sessionId[0] == '\0' ||
      block.channel != audioUploadConnectionChannel ||
      strcmp(block.sessionId, audioUploadConnectionSessionId) != 0) {
    Serial.println("[AUDIO_TX] batch_abort reason=invalid_persistent_session_context");
    return AudioBatchOutcome::FatalFailure;
  }

  const bool recordingNow = isRecordingActiveSnapshot();
  const uint8_t maxAttempts = recordingNow
                                  ? RtdbUploadConfig::LIVE_UPLOAD_MAX_ATTEMPTS
                                  : RtdbUploadConfig::DRAIN_UPLOAD_MAX_ATTEMPTS;

  Serial.printf(
      "[AUDIO_TX] batch channel=%u count=1 firstSeq=%lu lastSeq=%lu recording=%s maxAttempts=%u\n",
      static_cast<unsigned int>(block.channel),
      static_cast<unsigned long>(block.sequence),
      static_cast<unsigned long>(block.sequence),
      recordingNow ? "true" : "false",
      static_cast<unsigned int>(maxAttempts));

  size_t payloadLength = 0;
  size_t encodedBytes = 0;
  if (!buildSingleAudioPatchJson(
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

  int lastStatusCode = HTTPC_ERROR_CONNECTION_REFUSED;
  bool success = false;
  for (uint8_t attempt = 1; attempt <= maxAttempts; ++attempt) {
    lastStatusCode = performPersistentAudioPatch(
        reinterpret_cast<const uint8_t*>(audioUploadJson),
        payloadLength,
        attempt);
    success = lastStatusCode >= 200 && lastStatusCode < 300;
    if (success || !WifiConnection::isConnected() || isAudioAbortRequested()) {
      break;
    }
    if (attempt < maxAttempts) {
      vTaskDelay(pdMS_TO_TICKS(RtdbUploadConfig::UPLOAD_RETRY_DELAY_MS));
    }
  }

  const AudioBatchOutcome outcome =
      success ? AudioBatchOutcome::Success
              : (isTransientAudioStatus(lastStatusCode)
                     ? AudioBatchOutcome::TransientFailure
                     : AudioBatchOutcome::FatalFailure);

  Serial.printf(
      "[AUDIO_TX] batch_result outcome=%s success=%s count=1 firstSeq=%lu lastSeq=%lu http=%d encodedBytes=%u payloadBytes=%u queueDepth=%lu largestBlock=%lu\n",
      audioBatchOutcomeName(outcome),
      success ? "true" : "false",
      static_cast<unsigned long>(block.sequence),
      static_cast<unsigned long>(block.sequence),
      lastStatusCode,
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
  size_t count = 1;
  const uint32_t deadline = millis() + RtdbUploadConfig::BATCH_COLLECTION_WINDOW_MS;

  // Account for the block as soon as the worker removes it from readyQueue.
  // DrainingUploads must not observe an empty queue during the collection window.
  portENTER_CRITICAL(&serviceMux);
  uploadInFlight = true;
  uploadInFlightBlockCount = 1;
  portEXIT_CRITICAL(&serviceMux);

  while (count < RtdbUploadConfig::UPLOAD_BATCH_MAX_CHUNKS) {
    bool stillRecording;
    portENTER_CRITICAL(&serviceMux);
    stillRecording = recordingActive;
    portEXIT_CRITICAL(&serviceMux);
    if (!stillRecording || highDepth() > 0) {
      break;
    }

    const int32_t remaining = static_cast<int32_t>(deadline - millis());
    if (remaining <= 0) {
      break;
    }
    const TickType_t waitTicks = pdMS_TO_TICKS(
        static_cast<uint32_t>(remaining) > 20 ? 20 : static_cast<uint32_t>(remaining));
    uint8_t nextIndex = 0;
    if (xQueueReceive(readyAudioQueue, &nextIndex, waitTicks) == pdTRUE) {
      indices[count++] = nextIndex;
      portENTER_CRITICAL(&serviceMux);
      uploadInFlightBlockCount = static_cast<uint8_t>(count);
      portEXIT_CRITICAL(&serviceMux);
    }
  }

  logQueueEnqueue(RequestType::AudioBatch);
  const AudioBatchOutcome outcome = uploadAudioBatch(indices, count);
  bool requestWifiRecovery = false;
  uint32_t consecutiveFailuresSnapshot = 0;
  uint32_t totalFailuresSnapshot = 0;
  portENTER_CRITICAL(&serviceMux);
  uploadInFlight = false;
  uploadInFlightBlockCount = 0;
  if (outcome == AudioBatchOutcome::Success) {
    consecutiveTransientAudioFailures = 0;
  } else if (outcome == AudioBatchOutcome::TransientFailure) {
    ++consecutiveTransientAudioFailures;
    ++totalTransientAudioFailures;
    consecutiveFailuresSnapshot = consecutiveTransientAudioFailures;
    totalFailuresSnapshot = totalTransientAudioFailures;
    if (!recordingActive &&
        !audioAbortRequested &&
        !wifiRecoveryRequestedForSession &&
        consecutiveTransientAudioFailures >=
            RtdbUploadConfig::DRAIN_FAILURES_BEFORE_WIFI_RECOVERY) {
      wifiRecoveryRequestedForSession = true;
      requestWifiRecovery = true;
    }
  } else {
    uploadFailure = true;
  }
  portEXIT_CRITICAL(&serviceMux);

  for (size_t index = 0; index < count; ++index) {
    returnAudioBlockToFreeQueue(indices[index]);
  }

  if (outcome == AudioBatchOutcome::TransientFailure) {
    Serial.printf(
        "[AUDIO_TX] transient_failure_continue consecutive=%lu total=%lu recording=%s readyDepth=%lu\n",
        static_cast<unsigned long>(consecutiveFailuresSnapshot),
        static_cast<unsigned long>(totalFailuresSnapshot),
        isRecordingActiveSnapshot() ? "true" : "false",
        static_cast<unsigned long>(readyAudioDepth()));
  }
  if (requestWifiRecovery) {
    Serial.println(
        "[AUDIO_TX] transport_recovery action=wifi_reconnect reason=repeated_drain_failures");
    WifiConnection::requestReconnect("repeated_audio_transport_failures");
  }
}

RtdbRequestService::Result processControl(const ControlSlot& slot) {
  switch (slot.type) {
    case RequestType::SessionStart:
      closeAudioUploadConnection("new_session_start");
      return performSessionStart(slot.channel, slot.sessionId);
    case RequestType::SessionEnd:
      // Free the persistent audio TLS context before opening the small control
      // request. Keeping both TLS clients alive caused the minimum heap to fall
      // below one kilobyte on repeated transmissions.
      closeAudioUploadConnection("session_end");
      vTaskDelay(pdMS_TO_TICKS(20));
      return performSessionEnd(slot.channel, slot.sessionId, slot.lastSeq);
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

  // Audio queued while SESSION_START is in flight must never upload before the
  // corresponding metadata PATCH succeeds. The request task handles control
  // items before audio, then authorizes exactly this local session.
  if (slot.type == RequestType::SessionStart) {
    const bool uploadPrepared = result.succeeded() &&
                                prepareAudioUploadSession(slot.channel, slot.sessionId);
    portENTER_CRITICAL(&serviceMux);
    uploadSessionAuthorized = uploadPrepared;
    if (uploadPrepared) {
      authorizedChannel = slot.channel;
      snprintf(authorizedSessionId, sizeof(authorizedSessionId), "%s", slot.sessionId);
    } else {
      authorizedChannel = 0;
      authorizedSessionId[0] = '\0';
      uploadFailure = true;
    }
    portEXIT_CRITICAL(&serviceMux);

    Serial.printf(
        "[AUDIO_TX] session_start_async channel=%u session=%s outcome=%s http=%d uploadPrepared=%s\n",
        static_cast<unsigned int>(slot.channel),
        slot.sessionId,
        RtdbRequestService::outcomeName(result.outcome),
        result.httpCode,
        uploadPrepared ? "true" : "false");
  } else if (slot.type == RequestType::SessionEnd && result.succeeded()) {
    portENTER_CRITICAL(&serviceMux);
    if (uploadSessionAuthorized &&
        authorizedChannel == slot.channel &&
        strcmp(authorizedSessionId, slot.sessionId) == 0) {
      uploadSessionAuthorized = false;
      authorizedChannel = 0;
      authorizedSessionId[0] = '\0';
    }
    portEXIT_CRITICAL(&serviceMux);
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

  // A timed-out synchronous caller no longer owns the result, but a late
  // session start may already have changed RTDB. Compensate before recycling.
  if (slot.type == RequestType::SessionStart && result.succeeded()) {
    performSessionEnd(slot.channel, slot.sessionId, 0);
  }
  portENTER_CRITICAL(&slotMux);
  slot.state = SlotState::Free;
  portEXIT_CRITICAL(&slotMux);
}

bool canRunLow(RequestType type) {
  if (highDepth() > 0 || readyAudioDepth() > 0) {
    return false;
  }
  bool inFlight;
  bool priorityActive;
  portENTER_CRITICAL(&serviceMux);
  inFlight = uploadInFlight;
  priorityActive = audioPriority;
  portEXIT_CRITICAL(&serviceMux);
  if (inFlight) {
    return false;
  }
  (void)type;
  return !priorityActive;
}

bool lowRequestStillPending(RequestType type) {
  bool pending = false;
  xSemaphoreTake(lowDataMutex, portMAX_DELAY);
  pending = type == RequestType::AvailabilityHeartbeat
                ? heartbeatPending
                : usersReadPending;
  xSemaphoreGive(lowDataMutex);
  return pending;
}

void storeHeartbeatResult(bool success) {
  xSemaphoreTake(lowDataMutex, portMAX_DELAY);
  heartbeatPending = false;
  heartbeatResultReady = true;
  heartbeatLastResult = success;
  heartbeatDeferredLogged = false;
  xSemaphoreGive(lowDataMutex);
}

void logHeartbeatDeferredOnce() {
  bool shouldLog = false;
  xSemaphoreTake(lowDataMutex, portMAX_DELAY);
  if (!heartbeatDeferredLogged) {
    heartbeatDeferredLogged = true;
    shouldLog = true;
  }
  xSemaphoreGive(lowDataMutex);
  if (shouldLog) {
    Serial.println("[AVAILABILITY] deferred reason=audio_busy type=heartbeat");
  }
}

void storeUsersResult(bool success, const String& response) {
  xSemaphoreTake(lowDataMutex, portMAX_DELAY);
  usersReadPending = false;
  usersReadResultReady = true;
  usersReadLastResult = success;
  usersReadResponse = success ? response : String();
  xSemaphoreGive(lowDataMutex);
}

void processLow(RequestType type) {
  if (!lowRequestStillPending(type)) {
    return;
  }
  if (type == RequestType::AvailabilityHeartbeat) {
    char payloadBuffer[sizeof(pendingHeartbeatPayload)];
    xSemaphoreTake(lowDataMutex, portMAX_DELAY);
    snprintf(payloadBuffer, sizeof(payloadBuffer), "%s", pendingHeartbeatPayload);
    xSemaphoreGive(lowDataMutex);

    bool success = false;
    for (uint8_t attempt = 1; attempt <= AvailabilityConfig::RTDB_REQUEST_MAX_ATTEMPTS; ++attempt) {
      if (attempt > 1 && !canRunLow(type)) {
        Serial.println("[AVAILABILITY] deferred reason=audio_busy type=heartbeat_retry");
        break;
      }
      const HttpResponse response = performHttp(
          type,
          attempt,
          HttpMethod::Put,
          userPath(),
          String(payloadBuffer),
          true,
          nullptr,
          false);
      success = response.code >= 200 && response.code < 300;
      if (success || !WifiConnection::isConnected()) {
        break;
      }
      if (attempt < AvailabilityConfig::RTDB_REQUEST_MAX_ATTEMPTS) {
        if (!canRunLow(type)) {
          Serial.println("[AVAILABILITY] deferred reason=audio_busy type=heartbeat_retry");
          break;
        }
        vTaskDelay(pdMS_TO_TICKS(AvailabilityConfig::RTDB_RETRY_DELAY_MS));
      }
    }
    storeHeartbeatResult(success);
    return;
  }

  String responseJson;
  bool success = false;
  for (uint8_t attempt = 1; attempt <= AvailabilityConfig::RTDB_REQUEST_MAX_ATTEMPTS; ++attempt) {
    if (attempt > 1 && !canRunLow(type)) {
      Serial.println("[AVAILABILITY] deferred reason=audio_busy type=users_read_retry");
      break;
    }
    const HttpResponse response = performHttp(
        type,
        attempt,
        HttpMethod::Get,
        AvailabilityConfig::RTDB_USERS_PATH,
        String(),
        false,
        nullptr,
        false);
    success = response.code >= 200 && response.code < 300;
    if (success) {
      responseJson = response.body;
      break;
    }
    if (!WifiConnection::isConnected()) {
      break;
    }
    if (attempt < AvailabilityConfig::RTDB_REQUEST_MAX_ATTEMPTS) {
      if (!canRunLow(type)) {
        Serial.println("[AVAILABILITY] deferred reason=audio_busy type=users_read_retry");
        break;
      }
      vTaskDelay(pdMS_TO_TICKS(AvailabilityConfig::RTDB_RETRY_DELAY_MS));
    }
  }
  storeUsersResult(success, responseJson);
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
  if (AvailabilityConfig::ENABLED) {
    usersReadResponse.reserve(3072);
  }

  for (;;) {
    uint8_t controlIndex = 0;
    if (xQueueReceive(highQueue, &controlIndex, 0) == pdTRUE) {
      processControlSlot(controlIndex);
      continue;
    }

    uint8_t audioIndex = 0;
    if (xQueueReceive(readyAudioQueue, &audioIndex, 0) == pdTRUE) {
      processAudioBatch(audioIndex);
      continue;
    }

    RequestType lowType;
    if (AvailabilityConfig::ENABLED && lowQueue != nullptr &&
        xQueueReceive(lowQueue, &lowType, 0) == pdTRUE) {
      if (!lowRequestStillPending(lowType)) {
        continue;
      }
      if (canRunLow(lowType)) {
        processLow(lowType);
      } else {
        if (lowType == RequestType::AvailabilityHeartbeat) {
          logHeartbeatDeferredOnce();
        }
        xQueueSendToBack(lowQueue, &lowType, 0);
        vTaskDelay(pdMS_TO_TICKS(RtdbRequestConfig::TASK_IDLE_DELAY_MS));
      }
      continue;
    }

    ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(RtdbRequestConfig::TASK_IDLE_DELAY_MS));
  }
}

int allocateControlSlot(
    RequestType type,
    uint8_t channel,
    const char* sessionId,
    uint32_t lastSeq) {
  int selected = -1;
  portENTER_CRITICAL(&slotMux);
  for (uint8_t index = 0; index < RtdbRequestConfig::CONTROL_SLOT_COUNT; ++index) {
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
  snprintf(slot.sessionId, sizeof(slot.sessionId), "%s", sessionId == nullptr ? "" : sessionId);
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
  const int slotIndex = allocateControlSlot(type, channel, sessionId, lastSeq);
  if (slotIndex < 0) {
    result.outcome = RtdbRequestService::Outcome::Busy;
    Serial.printf(
        "[RTDB_REQUEST] fail type=%s http=0 error=no_control_slot durationMs=0\n",
        requestTypeName(type));
    return result;
  }

  const uint8_t queuedIndex = static_cast<uint8_t>(slotIndex);
  if (xQueueSend(highQueue, &queuedIndex, 0) != pdTRUE) {
    portENTER_CRITICAL(&slotMux);
    controlSlots[slotIndex].state = SlotState::Free;
    portEXIT_CRITICAL(&slotMux);
    result.outcome = RtdbRequestService::Outcome::Busy;
    Serial.printf(
        "[RTDB_REQUEST] fail type=%s http=0 error=high_queue_full durationMs=0\n",
        requestTypeName(type));
    return result;
  }
  logQueueEnqueue(type);
  if (requestTaskHandle != nullptr) {
    xTaskNotifyGive(requestTaskHandle);
  }

  if (xSemaphoreTake(controlSlots[slotIndex].done, pdMS_TO_TICKS(timeoutMs)) == pdTRUE) {
    portENTER_CRITICAL(&slotMux);
    result = controlSlots[slotIndex].result;
    controlSlots[slotIndex].state = SlotState::Free;
    portEXIT_CRITICAL(&slotMux);
    return result;
  }

  bool completionSignalPending = false;
  portENTER_CRITICAL(&slotMux);
  if (controlSlots[slotIndex].state == SlotState::Complete) {
    // The worker publishes Complete immediately before giving the binary
    // semaphore. Do not recycle this slot until that generation's token has
    // actually been consumed, or a late give could complete a newer request.
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
      "[RTDB_REQUEST] fail type=%s http=0 error=task_notification_timeout durationMs=%lu\n",
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

  if (!initializeFirebaseHost()) {
    Serial.println("[ERROR] Invalid Firebase database URL host");
    return false;
  }

  highQueue = xQueueCreate(RtdbRequestConfig::HIGH_QUEUE_LENGTH, sizeof(uint8_t));
  freeAudioQueue = xQueueCreate(RtdbUploadConfig::TX_QUEUE_LENGTH, sizeof(uint8_t));
  readyAudioQueue = xQueueCreate(RtdbUploadConfig::TX_QUEUE_LENGTH, sizeof(uint8_t));
  if (AvailabilityConfig::ENABLED) {
    lowQueue = xQueueCreate(RtdbRequestConfig::LOW_QUEUE_LENGTH, sizeof(RequestType));
    lowDataMutex = xSemaphoreCreateMutex();
  }
  if (highQueue == nullptr || freeAudioQueue == nullptr ||
      readyAudioQueue == nullptr ||
      (AvailabilityConfig::ENABLED &&
       (lowQueue == nullptr || lowDataMutex == nullptr))) {
    Serial.println("[ERROR] RTDB request service queue creation failed");
    return false;
  }

  for (uint8_t index = 0; index < RtdbRequestConfig::CONTROL_SLOT_COUNT; ++index) {
    controlSlots[index].done = xSemaphoreCreateBinary();
    if (controlSlots[index].done == nullptr) {
      Serial.println("[ERROR] RTDB request service semaphore creation failed");
      return false;
    }
  }
  for (uint8_t index = 0; index < RtdbUploadConfig::TX_QUEUE_LENGTH; ++index) {
    xQueueSend(freeAudioQueue, &index, portMAX_DELAY);
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
      "[READY] RTDB request service highQueue=%u lowQueue=%u audioBlocks=%u blockBytes=%u freeHeap=%lu\n",
      static_cast<unsigned int>(RtdbRequestConfig::HIGH_QUEUE_LENGTH),
      static_cast<unsigned int>(AvailabilityConfig::ENABLED
                                    ? RtdbRequestConfig::LOW_QUEUE_LENGTH
                                    : 0),
      static_cast<unsigned int>(RtdbUploadConfig::TX_QUEUE_LENGTH),
      static_cast<unsigned int>(sizeof(AudioBlock)),
      static_cast<unsigned long>(ESP.getFreeHeap()));
  return true;
}

bool isRunning() {
  portENTER_CRITICAL(&serviceMux);
  const bool running = requestRunning;
  portEXIT_CRITICAL(&serviceMux);
  return running;
}

bool scheduleSessionStart(uint8_t channel, const char* sessionId) {
  const int slotIndex = allocateControlSlot(
      RequestType::SessionStart,
      channel,
      sessionId,
      0);
  if (slotIndex < 0) {
    Serial.println("[RTDB_REQUEST] fail type=SESSION_START http=0 error=no_control_slot durationMs=0");
    return false;
  }

  portENTER_CRITICAL(&serviceMux);
  audioAbortRequested = false;
  uploadFailure = false;
  consecutiveTransientAudioFailures = 0;
  totalTransientAudioFailures = 0;
  wifiRecoveryRequestedForSession = false;
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
    Serial.println("[RTDB_REQUEST] fail type=SESSION_START http=0 error=high_queue_full durationMs=0");
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
      RequestType::SessionEnd, channel, sessionId, lastSeq, timeoutMs);
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

  // The fixed PCM pool is intentionally bounded. During a network stall, reuse
  // the oldest block still waiting in readyAudioQueue instead of discarding the
  // newest microphone audio. The in-flight block cannot be reclaimed here.
  uint8_t reclaimedIndex = 0;
  if (readyAudioQueue != nullptr &&
      xQueueReceive(readyAudioQueue, &reclaimedIndex, 0) == pdTRUE) {
    const uint32_t droppedSequence = audioBlocks[reclaimedIndex].sequence;
    char droppedSessionId[64] = {0};
    snprintf(
        droppedSessionId,
        sizeof(droppedSessionId),
        "%s",
        audioBlocks[reclaimedIndex].sessionId);
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
  if (readyAudioQueue == nullptr || index >= RtdbUploadConfig::TX_QUEUE_LENGTH ||
      channel == 0 || sessionId == nullptr || sessionId[0] == '\0' || sampleCount == 0 ||
      sampleCount > AudioConfig::CHUNK_SAMPLES) {
    releaseAudioBlock(index);
    return false;
  }

  AudioBlock& block = audioBlocks[index];
  block.sampleCount = sampleCount;
  block.channel = channel;
  block.sequence = sequence;
  block.recordedAtMs = recordedAtMs;
  snprintf(block.sessionId, sizeof(block.sessionId), "%s", sessionId);
  if (xQueueSend(readyAudioQueue, &index, 0) != pdTRUE) {
    releaseAudioBlock(index);
    Serial.printf(
        "[AUDIO_TX] queue_full seq=%lu readyDepth=%lu\n",
        static_cast<unsigned long>(sequence),
        static_cast<unsigned long>(readyAudioDepth()));
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

void clearAudioUploadFailure() {
  portENTER_CRITICAL(&serviceMux);
  uploadFailure = false;
  audioAbortRequested = false;
  consecutiveTransientAudioFailures = 0;
  totalTransientAudioFailures = 0;
  wifiRecoveryRequestedForSession = false;
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

uint32_t discardPendingAudio(const char* reason) {
  uint32_t discarded = 0;
  uint8_t index = 0;
  while (readyAudioQueue != nullptr && xQueueReceive(readyAudioQueue, &index, 0) == pdTRUE) {
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

bool scheduleAvailabilityHeartbeat(const char* jsonPayload) {
  if (!AvailabilityConfig::ENABLED || jsonPayload == nullptr ||
      jsonPayload[0] == '\0' || lowQueue == nullptr || lowDataMutex == nullptr) {
    return false;
  }
  xSemaphoreTake(lowDataMutex, portMAX_DELAY);
  if (heartbeatPending) {
    xSemaphoreGive(lowDataMutex);
    Serial.println("[AVAILABILITY] deferred reason=request_pending type=heartbeat");
    return false;
  }
  heartbeatPending = true;
  heartbeatDeferredLogged = false;
  snprintf(pendingHeartbeatPayload, sizeof(pendingHeartbeatPayload), "%s", jsonPayload);
  xSemaphoreGive(lowDataMutex);

  const RequestType type = RequestType::AvailabilityHeartbeat;
  if (xQueueSend(lowQueue, &type, 0) != pdTRUE) {
    xSemaphoreTake(lowDataMutex, portMAX_DELAY);
    heartbeatPending = false;
    xSemaphoreGive(lowDataMutex);
    Serial.println("[AVAILABILITY] deferred reason=queue_full type=heartbeat");
    return false;
  }
  logQueueEnqueue(type);
  if (requestTaskHandle != nullptr) {
    xTaskNotifyGive(requestTaskHandle);
  }
  return true;
}

bool scheduleAvailabilityUsersRead() {
  if (!AvailabilityConfig::ENABLED || lowQueue == nullptr || lowDataMutex == nullptr) {
    return false;
  }
  if (audioPriorityActive() || audioQueueDepth() > 0) {
    Serial.println("[AVAILABILITY] deferred reason=audio_busy type=users_read");
    return false;
  }
  xSemaphoreTake(lowDataMutex, portMAX_DELAY);
  if (usersReadPending) {
    xSemaphoreGive(lowDataMutex);
    Serial.println("[AVAILABILITY] deferred reason=request_pending type=users_read");
    return false;
  }
  usersReadPending = true;
  xSemaphoreGive(lowDataMutex);

  const RequestType type = RequestType::AvailabilityUsersRead;
  if (xQueueSend(lowQueue, &type, 0) != pdTRUE) {
    xSemaphoreTake(lowDataMutex, portMAX_DELAY);
    usersReadPending = false;
    xSemaphoreGive(lowDataMutex);
    return false;
  }
  logQueueEnqueue(type);
  if (requestTaskHandle != nullptr) {
    xTaskNotifyGive(requestTaskHandle);
  }
  return true;
}

bool takeAvailabilityHeartbeatResult(bool& outSuccess) {
  if (!AvailabilityConfig::ENABLED || lowDataMutex == nullptr) {
    return false;
  }
  xSemaphoreTake(lowDataMutex, portMAX_DELAY);
  const bool ready = heartbeatResultReady;
  if (ready) {
    outSuccess = heartbeatLastResult;
    heartbeatResultReady = false;
  }
  xSemaphoreGive(lowDataMutex);
  return ready;
}

bool takeAvailabilityUsersResult(bool& outSuccess, String& outJson) {
  if (!AvailabilityConfig::ENABLED || lowDataMutex == nullptr) {
    return false;
  }
  xSemaphoreTake(lowDataMutex, portMAX_DELAY);
  const bool ready = usersReadResultReady;
  if (ready) {
    outSuccess = usersReadLastResult;
    outJson = usersReadResponse;
    usersReadResultReady = false;
    usersReadResponse = "";
  }
  xSemaphoreGive(lowDataMutex);
  return ready;
}

}  // namespace RtdbRequestService
