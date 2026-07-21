#include "rtdb_audio_stream.h"

#include <ctype.h>
#include <esp_heap_caps.h>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>
#include <mbedtls/base64.h>
#include <string.h>
#include <utility>

#include "app_config.h"
#include "rtdb_request_service.h"
#include "wifi_connection.h"

namespace {

constexpr size_t SSE_EVENT_RESERVE_BYTES = AudioConfig::BASE64_BUFFER_BYTES + 640;
constexpr size_t SSE_PATH_RESERVE_BYTES = 192;

static_assert(
    RtdbUploadConfig::UPLOAD_BATCH_MAX_CHUNKS == 1,
    "The low-allocation SSE decoder requires one RTDB chunk per write");

SemaphoreHandle_t eventMutex = nullptr;
bool streamActive = false;
uint8_t streamChannel = 0;

// FirebaseClient already owns one response buffer. Keep only one additional
// parser buffer and overwrite an older unconsumed event instead of queuing
// multiple 4.4 KB JSON payloads.
enum class PendingEventKind : uint8_t {
  None,
  Put,
  Patch,
};

PendingEventKind pendingEventKind = PendingEventKind::None;
String ssePath;
String sseData;
bool pendingEventReady = false;
uint32_t overwrittenEventCount = 0;

bool firstSnapshotSeen = false;

bool remoteTalkerSelected = false;
String selectedDeviceId;
String selectedSessionId;
uint32_t selectedLastChunkAtMs = 0;
bool selectedHaveSequence = false;
uint32_t selectedLastSequence = 0;

void releaseStringStorage(String& value) {
  String empty;
  value = std::move(empty);
}

void releaseStoppedStreamMemory() {
  releaseStringStorage(ssePath);
  releaseStringStorage(sseData);
  releaseStringStorage(selectedDeviceId);
  releaseStringStorage(selectedSessionId);
}

const char* boolText(bool value) {
  return value ? "true" : "false";
}

String trimCopy(String value) {
  value.trim();
  return value;
}

void logStreamHeap(const char* stage) {
  if (!RtdbHttpConfig::LOG_STREAM_EVENTS) {
    return;
  }

  Serial.printf(
      "[RTDB_HEAP] type=SSE_STREAM stage=%s freeHeap=%lu largestBlock=%lu minimumFree=%lu\n",
      stage,
      static_cast<unsigned long>(ESP.getFreeHeap()),
      static_cast<unsigned long>(heap_caps_get_largest_free_block(MALLOC_CAP_8BIT)),
      static_cast<unsigned long>(heap_caps_get_minimum_free_size(MALLOC_CAP_8BIT)));
}

bool extractJsonString(const String& json, const char* key, String& out) {
  const String needle = String("\"") + key + "\":";
  int position = json.indexOf(needle);
  if (position < 0) {
    return false;
  }

  position += needle.length();
  while (position < static_cast<int>(json.length()) && isspace(json[position])) {
    ++position;
  }
  if (position >= static_cast<int>(json.length()) || json[position] != '"') {
    return false;
  }
  ++position;

  out = "";
  bool escaping = false;
  for (; position < static_cast<int>(json.length()); ++position) {
    const char value = json[position];
    if (escaping) {
      switch (value) {
        case '"': out += '"'; break;
        case '\\': out += '\\'; break;
        case '/': out += '/'; break;
        case 'b': out += '\b'; break;
        case 'f': out += '\f'; break;
        case 'n': out += '\n'; break;
        case 'r': out += '\r'; break;
        case 't': out += '\t'; break;
        default: out += value; break;
      }
      escaping = false;
      continue;
    }

    if (value == '\\') {
      escaping = true;
    } else if (value == '"') {
      return true;
    } else {
      out += value;
    }
  }
  return false;
}

bool extractJsonUInt(const String& json, const char* key, uint32_t& out) {
  const String needle = String("\"") + key + "\":";
  int position = json.indexOf(needle);
  if (position < 0) {
    return false;
  }

  position += needle.length();
  while (position < static_cast<int>(json.length()) && isspace(json[position])) {
    ++position;
  }

  uint32_t value = 0;
  bool sawDigit = false;
  while (position < static_cast<int>(json.length()) && isdigit(json[position])) {
    sawDigit = true;
    value = value * 10U + static_cast<uint32_t>(json[position] - '0');
    ++position;
  }
  if (!sawDigit) {
    return false;
  }
  out = value;
  return true;
}

bool extractJsonBool(const String& json, const char* key, bool& out) {
  const String needle = String("\"") + key + "\":";
  int position = json.indexOf(needle);
  if (position < 0) {
    return false;
  }

  position += needle.length();
  while (position < static_cast<int>(json.length()) && isspace(json[position])) {
    ++position;
  }

  if (json.substring(position, position + 4) == "true") {
    out = true;
    return true;
  }
  if (json.substring(position, position + 5) == "false") {
    out = false;
    return true;
  }
  return false;
}

bool outerDataIsNull(const String& eventJson) {
  const String needle = "\"data\":";
  int position = eventJson.indexOf(needle);
  if (position < 0) {
    return false;
  }

  position += needle.length();
  while (position < static_cast<int>(eventJson.length()) &&
         isspace(eventJson[position])) {
    ++position;
  }
  return eventJson.substring(position, position + 4) == "null";
}

String deviceIdFromEventPath(const String& eventPath) {
  if (!eventPath.startsWith("/") || eventPath.length() < 2) {
    return String();
  }
  const int separator = eventPath.indexOf('/', 1);
  if (separator < 0) {
    return eventPath.substring(1);
  }
  return eventPath.substring(1, separator);
}

void clearRemoteTalker(const char* reason) {
  if (remoteTalkerSelected && RtdbHttpConfig::LOG_STREAM_EVENTS) {
    Serial.printf(
        "[RTDB][STREAM][REMOTE_RELEASE] device=%s session=%s reason=%s\n",
        selectedDeviceId.c_str(),
        selectedSessionId.c_str(),
        reason == nullptr ? "unspecified" : reason);
  }
  remoteTalkerSelected = false;
  selectedDeviceId = "";
  selectedSessionId = "";
  selectedLastChunkAtMs = 0;
  selectedHaveSequence = false;
  selectedLastSequence = 0;
}

void expireRemoteTalkerIfIdle() {
  if (!remoteTalkerSelected) {
    return;
  }
  if ((millis() - selectedLastChunkAtMs) >
      CommunicationConfig::REMOTE_TALKER_IDLE_TIMEOUT_MS) {
    clearRemoteTalker("chunk_timeout");
  }
}

bool selectOrAcceptRemoteTalker(
    const String& deviceId,
    const String& sessionId,
    uint32_t sequence) {
  expireRemoteTalkerIfIdle();

  if (!remoteTalkerSelected) {
    remoteTalkerSelected = true;
    selectedDeviceId = deviceId;
    selectedSessionId = sessionId;
    selectedLastChunkAtMs = millis();
    selectedHaveSequence = false;
    selectedLastSequence = 0;
    if (RtdbHttpConfig::LOG_STREAM_EVENTS) {
      Serial.printf(
          "[RTDB][STREAM][REMOTE_SELECT] device=%s session=%s\n",
          selectedDeviceId.c_str(),
          selectedSessionId.c_str());
    }
  }

  if (sessionId != selectedSessionId ||
      (selectedDeviceId.length() > 0 &&
       deviceId.length() > 0 &&
       deviceId != selectedDeviceId)) {
    if (RtdbHttpConfig::LOG_STREAM_EVENTS) {
      Serial.printf(
          "[RTDB][STREAM][CHUNK_IGNORE] device=%s session=%s seq=%lu reason=another_talker_active selectedDevice=%s selectedSession=%s\n",
          deviceId.c_str(),
          sessionId.c_str(),
          static_cast<unsigned long>(sequence),
          selectedDeviceId.c_str(),
          selectedSessionId.c_str());
    }
    return false;
  }

  if (selectedHaveSequence && sequence <= selectedLastSequence) {
    if (RtdbHttpConfig::LOG_STREAM_EVENTS) {
      Serial.printf(
          "[RTDB][STREAM][CHUNK_SKIP_OLD] device=%s session=%s seq=%lu lastSeq=%lu\n",
          deviceId.c_str(),
          sessionId.c_str(),
          static_cast<unsigned long>(sequence),
          static_cast<unsigned long>(selectedLastSequence));
    }
    return false;
  }

  selectedLastChunkAtMs = millis();
  selectedHaveSequence = true;
  selectedLastSequence = sequence;
  return true;
}

void processMetaObject(const String& metaObject, const String& pathDeviceId) {
  bool active = false;
  const bool haveActive = extractJsonBool(metaObject, "active", active);
  String sessionId;
  const bool haveSession = extractJsonString(metaObject, "sessionId", sessionId);
  String deviceId;
  if (!extractJsonString(metaObject, "deviceId", deviceId)) {
    deviceId = pathDeviceId;
  }

  if (RtdbHttpConfig::LOG_STREAM_EVENTS) {
    Serial.printf(
        "[RTDB][STREAM][META] device=%s haveActive=%s active=%s haveSession=%s session=%s\n",
        deviceId.c_str(),
        boolText(haveActive),
        boolText(active),
        boolText(haveSession),
        sessionId.c_str());
  }

  if (!haveActive || active || !remoteTalkerSelected) {
    return;
  }

  const bool sessionMatches = !haveSession || sessionId == selectedSessionId;
  const bool deviceMatches = deviceId.length() == 0 ||
                             selectedDeviceId.length() == 0 ||
                             deviceId == selectedDeviceId;
  if (sessionMatches && deviceMatches) {
    clearRemoteTalker("session_end");
  }
}

bool findStringEnd(
    const String& json,
    int quoteStart,
    int limit,
    int& outQuoteEnd) {
  if (quoteStart < 0 || quoteStart >= limit || json[quoteStart] != '"') {
    return false;
  }

  bool escaping = false;
  for (int position = quoteStart + 1; position < limit; ++position) {
    const char value = json[position];
    if (escaping) {
      escaping = false;
    } else if (value == '\\') {
      escaping = true;
    } else if (value == '"') {
      outQuoteEnd = position;
      return true;
    }
  }
  return false;
}

bool findCompositeEnd(
    const String& json,
    int startPosition,
    int limit,
    char opening,
    char closing,
    int& outEndExclusive) {
  if (startPosition < 0 || startPosition >= limit ||
      json[startPosition] != opening) {
    return false;
  }

  bool inString = false;
  bool escaping = false;
  int depth = 0;
  for (int position = startPosition; position < limit; ++position) {
    const char value = json[position];
    if (inString) {
      if (escaping) {
        escaping = false;
      } else if (value == '\\') {
        escaping = true;
      } else if (value == '"') {
        inString = false;
      }
      continue;
    }

    if (value == '"') {
      inString = true;
    } else if (value == opening) {
      ++depth;
    } else if (value == closing) {
      --depth;
      if (depth == 0) {
        outEndExclusive = position + 1;
        return true;
      }
    }
  }
  return false;
}

bool findJsonValueEnd(
    const String& json,
    int valueStart,
    int objectEnd,
    int& outValueEnd) {
  if (valueStart < 0 || valueStart >= objectEnd) {
    return false;
  }

  const char first = json[valueStart];
  if (first == '{') {
    return findCompositeEnd(json, valueStart, objectEnd, '{', '}', outValueEnd);
  }
  if (first == '[') {
    return findCompositeEnd(json, valueStart, objectEnd, '[', ']', outValueEnd);
  }
  if (first == '"') {
    int quoteEnd = 0;
    if (!findStringEnd(json, valueStart, objectEnd, quoteEnd)) {
      return false;
    }
    outValueEnd = quoteEnd + 1;
    return true;
  }

  int position = valueStart;
  while (position < objectEnd && json[position] != ',' && json[position] != '}') {
    ++position;
  }
  outValueEnd = position;
  return outValueEnd > valueStart;
}

bool nextObjectProperty(
    const String& json,
    int objectStart,
    int objectEnd,
    int& cursor,
    int& keyStart,
    int& keyEnd,
    int& valueStart,
    int& valueEnd) {
  int position = cursor < objectStart + 1 ? objectStart + 1 : cursor;
  while (position < objectEnd &&
         (isspace(static_cast<unsigned char>(json[position])) ||
          json[position] == ',')) {
    ++position;
  }
  if (position >= objectEnd || json[position] == '}') {
    return false;
  }
  if (json[position] != '"') {
    return false;
  }

  int keyQuoteEnd = 0;
  if (!findStringEnd(json, position, objectEnd, keyQuoteEnd)) {
    return false;
  }
  keyStart = position + 1;
  keyEnd = keyQuoteEnd;

  position = keyQuoteEnd + 1;
  while (position < objectEnd &&
         isspace(static_cast<unsigned char>(json[position]))) {
    ++position;
  }
  if (position >= objectEnd || json[position] != ':') {
    return false;
  }
  ++position;
  while (position < objectEnd &&
         isspace(static_cast<unsigned char>(json[position]))) {
    ++position;
  }
  valueStart = position;
  if (!findJsonValueEnd(json, valueStart, objectEnd, valueEnd)) {
    return false;
  }
  cursor = valueEnd;
  return true;
}

bool keyEquals(
    const String& json,
    int keyStart,
    int keyEnd,
    const char* expected) {
  if (expected == nullptr) {
    return false;
  }
  const size_t expectedLength = strlen(expected);
  if (keyEnd - keyStart != static_cast<int>(expectedLength)) {
    return false;
  }
  for (size_t index = 0; index < expectedLength; ++index) {
    if (json[keyStart + static_cast<int>(index)] != expected[index]) {
      return false;
    }
  }
  return true;
}

bool findTopLevelProperty(
    const String& json,
    int objectStart,
    int objectEnd,
    const char* key,
    int& valueStart,
    int& valueEnd) {
  int cursor = objectStart + 1;
  int keyStart = 0;
  int keyEnd = 0;
  while (nextObjectProperty(
      json,
      objectStart,
      objectEnd,
      cursor,
      keyStart,
      keyEnd,
      valueStart,
      valueEnd)) {
    if (keyEquals(json, keyStart, keyEnd, key)) {
      return true;
    }
  }
  return false;
}

bool extractTopLevelStringBounds(
    const String& json,
    int objectStart,
    int objectEnd,
    const char* key,
    int& contentStart,
    int& contentEnd) {
  int valueStart = 0;
  int valueEnd = 0;
  if (!findTopLevelProperty(
          json, objectStart, objectEnd, key, valueStart, valueEnd) ||
      valueStart >= valueEnd || json[valueStart] != '"') {
    return false;
  }
  int quoteEnd = 0;
  if (!findStringEnd(json, valueStart, valueEnd, quoteEnd)) {
    return false;
  }
  contentStart = valueStart + 1;
  contentEnd = quoteEnd;
  return true;
}

bool extractTopLevelString(
    const String& json,
    int objectStart,
    int objectEnd,
    const char* key,
    String& out) {
  int contentStart = 0;
  int contentEnd = 0;
  if (!extractTopLevelStringBounds(
          json, objectStart, objectEnd, key, contentStart, contentEnd)) {
    return false;
  }
  out = json.substring(contentStart, contentEnd);
  return true;
}

bool extractTopLevelUInt(
    const String& json,
    int objectStart,
    int objectEnd,
    const char* key,
    uint32_t& out) {
  int valueStart = 0;
  int valueEnd = 0;
  if (!findTopLevelProperty(
          json, objectStart, objectEnd, key, valueStart, valueEnd)) {
    return false;
  }

  uint32_t value = 0;
  bool sawDigit = false;
  int position = valueStart;
  while (position < valueEnd &&
         isspace(static_cast<unsigned char>(json[position]))) {
    ++position;
  }
  while (position < valueEnd && isdigit(static_cast<unsigned char>(json[position]))) {
    sawDigit = true;
    value = value * 10U + static_cast<uint32_t>(json[position] - '0');
    ++position;
  }
  if (!sawDigit) {
    return false;
  }
  out = value;
  return true;
}

bool extractOuterDataObjectRange(
    const String& eventJson,
    int& objectStart,
    int& objectEnd) {
  const String needle = "\"data\":";
  int position = eventJson.indexOf(needle);
  if (position < 0) {
    return false;
  }
  position += needle.length();
  while (position < static_cast<int>(eventJson.length()) &&
         isspace(static_cast<unsigned char>(eventJson[position]))) {
    ++position;
  }
  if (position >= static_cast<int>(eventJson.length()) ||
      eventJson[position] != '{') {
    return false;
  }
  objectStart = position;
  return findCompositeEnd(
      eventJson,
      objectStart,
      static_cast<int>(eventJson.length()),
      '{',
      '}',
      objectEnd);
}

bool decodeChunkObjectRange(
    const String& json,
    int objectStart,
    int objectEnd,
    const String& pathDeviceId,
    int16_t* outSamples,
    size_t maxSamples,
    RtdbPcmChunk& outChunk) {
  uint32_t sequence = 0;
  String sessionId;
  String deviceId;
  int encodedStart = 0;
  int encodedEnd = 0;

  if (!extractTopLevelUInt(json, objectStart, objectEnd, "seq", sequence) ||
      !extractTopLevelStringBounds(
          json, objectStart, objectEnd, "data", encodedStart, encodedEnd) ||
      !extractTopLevelString(
          json, objectStart, objectEnd, "sessionId", sessionId)) {
    return false;
  }
  if (!extractTopLevelString(
          json, objectStart, objectEnd, "deviceId", deviceId)) {
    deviceId = pathDeviceId;
  }

  if (deviceId == AppConfig::DEVICE_ID) {
    if (RtdbHttpConfig::LOG_STREAM_EVENTS) {
      Serial.printf(
          "[RTDB][STREAM][CHUNK_IGNORE] device=%s session=%s seq=%lu reason=self\n",
          deviceId.c_str(),
          sessionId.c_str(),
          static_cast<unsigned long>(sequence));
    }
    return false;
  }

  const size_t encodedLength =
      static_cast<size_t>(encodedEnd - encodedStart);
  const size_t capacityBytes = maxSamples * sizeof(int16_t);
  size_t decodedBytes = 0;
  const int result = mbedtls_base64_decode(
      reinterpret_cast<unsigned char*>(outSamples),
      capacityBytes,
      &decodedBytes,
      reinterpret_cast<const unsigned char*>(json.c_str() + encodedStart),
      encodedLength);
  if (result != 0 || decodedBytes == 0 ||
      (decodedBytes % sizeof(int16_t)) != 0) {
    Serial.printf(
        "[RTDB][STREAM][CHUNK_DECODE_FAIL] device=%s session=%s seq=%lu rc=%d base64Bytes=%u decodedBytes=%u capacityBytes=%u\n",
        deviceId.c_str(),
        sessionId.c_str(),
        static_cast<unsigned long>(sequence),
        result,
        static_cast<unsigned int>(encodedLength),
        static_cast<unsigned int>(decodedBytes),
        static_cast<unsigned int>(capacityBytes));
    return false;
  }

  if (!selectOrAcceptRemoteTalker(deviceId, sessionId, sequence)) {
    return false;
  }

  uint32_t sampleRate = AudioConfig::SAMPLE_RATE;
  uint32_t chunkMs = AudioConfig::CHUNK_MS;
  extractTopLevelUInt(json, objectStart, objectEnd, "sampleRate", sampleRate);
  extractTopLevelUInt(json, objectStart, objectEnd, "chunkMs", chunkMs);

  outChunk.seq = sequence;
  outChunk.sampleRate = sampleRate;
  outChunk.chunkMs = static_cast<uint16_t>(chunkMs);
  outChunk.sampleCount = decodedBytes / sizeof(int16_t);
  outChunk.sessionId = sessionId;
  outChunk.deviceId = deviceId;

  if (RtdbHttpConfig::LOG_STREAM_EVENTS) {
    Serial.printf(
        "[RTDB][STREAM][CHUNK_DECODE_OK] device=%s session=%s seq=%lu samples=%u sampleRate=%lu chunkMs=%lu base64Bytes=%u\n",
        deviceId.c_str(),
        sessionId.c_str(),
        static_cast<unsigned long>(sequence),
        static_cast<unsigned int>(outChunk.sampleCount),
        static_cast<unsigned long>(outChunk.sampleRate),
        static_cast<unsigned long>(outChunk.chunkMs),
        static_cast<unsigned int>(encodedLength));
  }
  return true;
}

bool decodeFirstChunkInObjectTree(
    const String& json,
    int objectStart,
    int objectEnd,
    const String& pathDeviceId,
    int16_t* outSamples,
    size_t maxSamples,
    RtdbPcmChunk& outChunk,
    uint8_t depth = 0) {
  if (decodeChunkObjectRange(
          json,
          objectStart,
          objectEnd,
          pathDeviceId,
          outSamples,
          maxSamples,
          outChunk)) {
    return true;
  }
  if (depth >= 4) {
    return false;
  }

  int cursor = objectStart + 1;
  int keyStart = 0;
  int keyEnd = 0;
  int valueStart = 0;
  int valueEnd = 0;
  while (nextObjectProperty(
      json,
      objectStart,
      objectEnd,
      cursor,
      keyStart,
      keyEnd,
      valueStart,
      valueEnd)) {
    if (valueStart < valueEnd && json[valueStart] == '{' &&
        decodeFirstChunkInObjectTree(
            json,
            valueStart,
            valueEnd,
            pathDeviceId,
            outSamples,
            maxSamples,
            outChunk,
            depth + 1)) {
      return true;
    }
  }
  return false;
}

bool processSseEvent(
    int16_t* outSamples,
    size_t maxSamples,
    RtdbPcmChunk& outChunk) {
  const char* eventName =
      pendingEventKind == PendingEventKind::Put
          ? "put"
          : (pendingEventKind == PendingEventKind::Patch ? "patch" : "");
  if (eventName[0] == '\0') {
    return false;
  }
  const String& eventPath = ssePath;

  if (RtdbHttpConfig::LOG_STREAM_EVENTS) {
    Serial.printf(
        "[RTDB][STREAM][EVENT] name=%s path=%s dataBytes=%u\n",
        eventName,
        eventPath.c_str(),
        static_cast<unsigned int>(sseData.length()));
  }

  int dataObjectStart = 0;
  while (dataObjectStart < static_cast<int>(sseData.length()) &&
         isspace(static_cast<unsigned char>(sseData[dataObjectStart]))) {
    ++dataObjectStart;
  }
  int dataObjectEnd = 0;
  const bool dataIsObject =
      dataObjectStart < static_cast<int>(sseData.length()) &&
      sseData[dataObjectStart] == '{' &&
      findCompositeEnd(
          sseData,
          dataObjectStart,
          static_cast<int>(sseData.length()),
          '{',
          '}',
          dataObjectEnd);
  if (!dataIsObject) {
    if (RtdbHttpConfig::LOG_STREAM_EVENTS) {
      Serial.printf(
          "[RTDB][STREAM][EVENT_IGNORE] path=%s reason=%s\n",
          eventPath.c_str(),
          sseData == "null" ? "data_null" : "data_not_object");
    }
    return false;
  }

  if (!firstSnapshotSeen) {
    firstSnapshotSeen = true;
    if (strcmp(eventName, "put") == 0 && eventPath == "/" &&
        !RtdbBufferConfig::PLAY_EXISTING_CHUNKS_ON_CONNECT) {
      if (RtdbHttpConfig::LOG_STREAM_EVENTS) {
        Serial.println("[RTDB][STREAM][ROOT_IGNORE] initial talkers snapshot ignored");
      }
      return false;
    }
  }

  const String pathDeviceId = deviceIdFromEventPath(eventPath);

  if (eventPath.endsWith("/live/meta")) {
    const String metaObject =
        sseData.substring(dataObjectStart, dataObjectEnd);
    processMetaObject(metaObject, pathDeviceId);
    return false;
  }

  if (eventPath.endsWith("/live")) {
    int metaStart = 0;
    int metaEnd = 0;
    if (findTopLevelProperty(
            sseData,
            dataObjectStart,
            dataObjectEnd,
            "meta",
            metaStart,
            metaEnd) &&
        metaStart < metaEnd && sseData[metaStart] == '{') {
      const String metaObject = sseData.substring(metaStart, metaEnd);
      processMetaObject(metaObject, pathDeviceId);
    }

    return decodeFirstChunkInObjectTree(
        sseData,
        dataObjectStart,
        dataObjectEnd,
        pathDeviceId,
        outSamples,
        maxSamples,
        outChunk);
  }

  if (eventPath.endsWith("/live/chunks") ||
      eventPath.indexOf("/live/chunks/") >= 0 ||
      (eventPath == "/" && strcmp(eventName, "patch") == 0)) {
    const bool decoded = decodeFirstChunkInObjectTree(
        sseData,
        dataObjectStart,
        dataObjectEnd,
        pathDeviceId,
        outSamples,
        maxSamples,
        outChunk);
    if (!decoded && RtdbHttpConfig::LOG_STREAM_EVENTS) {
      Serial.printf(
          "[RTDB][STREAM][CHUNK_PARSE_FAIL] path=%s objectBytes=%u freeHeap=%lu largestBlock=%lu\n",
          eventPath.c_str(),
          static_cast<unsigned int>(dataObjectEnd - dataObjectStart),
          static_cast<unsigned long>(ESP.getFreeHeap()),
          static_cast<unsigned long>(
              heap_caps_get_largest_free_block(MALLOC_CAP_8BIT)));
    }
    return decoded;
  }

  if (RtdbHttpConfig::LOG_STREAM_EVENTS) {
    Serial.printf(
        "[RTDB][STREAM][EVENT_IGNORE] path=%s reason=unhandled_path\n",
        eventPath.c_str());
  }
  return false;
}

}  // namespace

namespace RtdbAudioStream {

bool begin() {
  if (eventMutex == nullptr) {
    eventMutex = xSemaphoreCreateMutex();
  }
  return eventMutex != nullptr;
}

void loopMaintenance() {
  // FirebaseClient is serviced exclusively by the RTDB request task.
}

bool startListening(uint8_t channel) {
  stopListening();
  if (channel == 0) {
    Serial.println("[RTDB][STREAM] start skipped: no channel joined");
    return false;
  }
  if (eventMutex == nullptr && !begin()) {
    Serial.println("[RTDB][STREAM] event mutex creation failed");
    return false;
  }
  if (!WifiConnection::isConnected()) {
    Serial.println("[RTDB][STREAM] Start skipped: WiFi is disconnected");
    return false;
  }

  const bool pathReserved = ssePath.reserve(SSE_PATH_RESERVE_BYTES);
  const bool dataReserved = sseData.reserve(SSE_EVENT_RESERVE_BYTES);
  if (!pathReserved || !dataReserved) {
    Serial.printf(
        "[RTDB][STREAM] buffer_reserve_failed path=%s data=%s freeHeap=%lu largestBlock=%lu\n",
        pathReserved ? "true" : "false",
        dataReserved ? "true" : "false",
        static_cast<unsigned long>(ESP.getFreeHeap()),
        static_cast<unsigned long>(
            heap_caps_get_largest_free_block(MALLOC_CAP_8BIT)));
    releaseStoppedStreamMemory();
    return false;
  }

  xSemaphoreTake(eventMutex, portMAX_DELAY);
  pendingEventReady = false;
  pendingEventKind = PendingEventKind::None;
  ssePath = "";
  sseData = "";
  firstSnapshotSeen = false;
  clearRemoteTalker("stream_start");
  streamChannel = channel;
  streamActive = true;
  xSemaphoreGive(eventMutex);

  if (!RtdbRequestService::startAudioStream(
          channel, FirebaseClientConfig::STREAM_START_TIMEOUT_MS)) {
    xSemaphoreTake(eventMutex, portMAX_DELAY);
    streamActive = false;
    streamChannel = 0;
    pendingEventReady = false;
    xSemaphoreGive(eventMutex);
    releaseStoppedStreamMemory();
    return false;
  }

  Serial.printf(
      "[RTDB][STREAM] listening_via_firebaseclient channel=%u freeHeap=%lu largestBlock=%lu\n",
      static_cast<unsigned int>(channel),
      static_cast<unsigned long>(ESP.getFreeHeap()),
      static_cast<unsigned long>(
          heap_caps_get_largest_free_block(MALLOC_CAP_8BIT)));
  return true;
}

void stopListening() {
  bool wasActive = false;
  if (eventMutex != nullptr) {
    xSemaphoreTake(eventMutex, portMAX_DELAY);
    wasActive = streamActive;
    streamActive = false;
    streamChannel = 0;
    pendingEventReady = false;
    xSemaphoreGive(eventMutex);
  }

  if (wasActive) {
    Serial.println("[RTDB][STREAM] stopping FirebaseClient stream");
    RtdbRequestService::stopAudioStream(
        FirebaseClientConfig::STREAM_STOP_TIMEOUT_MS);
  }

  firstSnapshotSeen = false;
  clearRemoteTalker("stream_stop");
  releaseStoppedStreamMemory();
  Serial.printf(
      "[RTDB][STREAM] stopped freeHeap=%lu largestBlock=%lu\n",
      static_cast<unsigned long>(ESP.getFreeHeap()),
      static_cast<unsigned long>(
          heap_caps_get_largest_free_block(MALLOC_CAP_8BIT)));
}

bool isListening() {
  if (eventMutex == nullptr) {
    return false;
  }
  xSemaphoreTake(eventMutex, portMAX_DELAY);
  const bool active = streamActive;
  xSemaphoreGive(eventMutex);
  return active;
}

void ingestFirebaseEvent(
    const char* eventName,
    const char* eventPath,
    const char* eventData) {
  if (eventMutex == nullptr || eventName == nullptr || eventPath == nullptr) {
    return;
  }
  if (strcmp(eventName, "put") != 0 && strcmp(eventName, "patch") != 0) {
    if (RtdbHttpConfig::LOG_STREAM_EVENTS &&
        strcmp(eventName, "keep-alive") != 0) {
      Serial.printf(
          "[RTDB][STREAM][EVENT_STATUS] event=%s path=%s data=%s\n",
          eventName,
          eventPath,
          eventData == nullptr ? "" : eventData);
    }
    return;
  }

  const char* data = eventData == nullptr || eventData[0] == '\0'
                         ? "null"
                         : eventData;
  const size_t required = strlen(data) + 1;
  if (required >= SSE_EVENT_RESERVE_BYTES) {
    Serial.printf(
        "[RTDB][STREAM][EVENT_DROP] reason=payload_too_large bytes=%u capacity=%u path=%s\n",
        static_cast<unsigned int>(required),
        static_cast<unsigned int>(SSE_EVENT_RESERVE_BYTES),
        eventPath);
    return;
  }

  // Never block FirebaseClient's result processing behind audio decoding.
  if (xSemaphoreTake(eventMutex, 0) != pdTRUE) {
    ++overwrittenEventCount;
    if (RtdbHttpConfig::LOG_STREAM_EVENTS) {
      Serial.printf(
          "[RTDB][STREAM][EVENT_DROP] reason=decoder_busy dropped=%lu path=%s\n",
          static_cast<unsigned long>(overwrittenEventCount),
          eventPath);
    }
    return;
  }
  if (!streamActive) {
    xSemaphoreGive(eventMutex);
    return;
  }

  if (pendingEventReady) {
    ++overwrittenEventCount;
  }
  pendingEventKind = strcmp(eventName, "put") == 0
                         ? PendingEventKind::Put
                         : PendingEventKind::Patch;
  ssePath = eventPath;
  sseData = data;
  pendingEventReady = true;
  xSemaphoreGive(eventMutex);
}

bool pollListening(
    int16_t* outSamples,
    size_t maxSamples,
    RtdbPcmChunk& outChunk) {
  if (eventMutex == nullptr || outSamples == nullptr || maxSamples == 0) {
    return false;
  }

  if (xSemaphoreTake(eventMutex, 0) != pdTRUE) {
    return false;
  }
  if (!streamActive || !pendingEventReady) {
    xSemaphoreGive(eventMutex);
    return false;
  }

  expireRemoteTalkerIfIdle();
  const bool decoded = processSseEvent(outSamples, maxSamples, outChunk);
  pendingEventReady = false;
  pendingEventKind = PendingEventKind::None;
  ssePath = "";
  sseData = "";
  xSemaphoreGive(eventMutex);
  return decoded;
}

}  // namespace RtdbAudioStream
