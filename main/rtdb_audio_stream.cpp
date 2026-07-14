#include "rtdb_audio_stream.h"

#include <HTTPClient.h>
#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <ctype.h>
#include <esp_heap_caps.h>
#include <mbedtls/base64.h>
#include <new>
#include <utility>

#include "app_config.h"
#include "wifi_connection.h"

namespace {

constexpr size_t SSE_LINE_MAX_BYTES = 8192;
constexpr size_t SSE_EVENT_RESERVE_BYTES = AudioConfig::BASE64_BUFFER_BYTES + 768;

static_assert(
    RtdbUploadConfig::UPLOAD_BATCH_MAX_CHUNKS == 1,
    "The low-allocation SSE decoder currently requires one RTDB chunk per PATCH");

WiFiClientSecure streamClient;
HTTPClient streamHttp;
bool streamActive = false;
uint8_t streamChannel = 0;

String sseLine;
String sseEvent;
String sseData;
bool droppingOversizedSseLine = false;

bool firstSnapshotSeen = false;

bool remoteTalkerSelected = false;
String selectedDeviceId;
String selectedSessionId;
uint32_t selectedLastChunkAtMs = 0;
bool selectedHaveSequence = false;
uint32_t selectedLastSequence = 0;

void releaseStringStorage(String& value) {
  // Assigning an empty literal only changes length and intentionally retains the
  // backing allocation. Move-assigning a fresh String destroys that backing
  // allocation, which is required while SSE is paused for a talker TLS request.
  String empty;
  value = std::move(empty);
}

void resetStreamTransport() {
  // HTTPClient::end() and WiFiClientSecure::stop() close the connection, but
  // long-lived objects may retain URL/header/TLS bookkeeping. Reconstruct both
  // objects so every pause/reconnect starts from a fully released state.
  streamHttp.end();
  streamClient.stop();
  streamHttp.~HTTPClient();
  new (&streamHttp) HTTPClient();
  streamClient.~WiFiClientSecure();
  new (&streamClient) WiFiClientSecure();
}

void releaseStoppedStreamMemory() {
  releaseStringStorage(sseLine);
  releaseStringStorage(sseEvent);
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

String databaseBaseUrl() {
  String base = FirebaseConfig::DATABASE_URL;
  while (base.endsWith("/")) {
    base.remove(base.length() - 1);
  }
  return base;
}

String channelKey(uint8_t channel) {
  char buffer[8];
  snprintf(buffer, sizeof(buffer), "ch%02u", static_cast<unsigned int>(channel));
  return String(buffer);
}

String talkersPath(uint8_t channel) {
  return String("/rooms/") + AppConfig::ROOM_ID +
         "/channels/" + channelKey(channel) +
         "/talkers";
}

String rtdbUrlForPath(const String& path) {
  return databaseBaseUrl() + path + ".json";
}

void setHttpRedirectPolicy(HTTPClient& http) {
  http.setFollowRedirects(HTTPC_STRICT_FOLLOW_REDIRECTS);
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
  const String eventName = trimCopy(sseEvent);
  if (eventName != "put" && eventName != "patch") {
    return false;
  }

  String eventPath;
  if (!extractJsonString(sseData, "path", eventPath)) {
    if (RtdbHttpConfig::LOG_STREAM_EVENTS) {
      Serial.printf(
          "[RTDB][STREAM][EVENT_BAD] reason=missing_path dataBytes=%u freeHeap=%lu largestBlock=%lu\n",
          static_cast<unsigned int>(sseData.length()),
          static_cast<unsigned long>(ESP.getFreeHeap()),
          static_cast<unsigned long>(
              heap_caps_get_largest_free_block(MALLOC_CAP_8BIT)));
    }
    return false;
  }

  if (RtdbHttpConfig::LOG_STREAM_EVENTS) {
    Serial.printf(
        "[RTDB][STREAM][EVENT] name=%s path=%s dataBytes=%u\n",
        eventName.c_str(),
        eventPath.c_str(),
        static_cast<unsigned int>(sseData.length()));
  }

  int dataObjectStart = 0;
  int dataObjectEnd = 0;
  if (!extractOuterDataObjectRange(
          sseData, dataObjectStart, dataObjectEnd)) {
    if (RtdbHttpConfig::LOG_STREAM_EVENTS) {
      Serial.printf(
          "[RTDB][STREAM][EVENT_IGNORE] path=%s reason=%s\n",
          eventPath.c_str(),
          outerDataIsNull(sseData) ? "data_null" : "data_not_object");
    }
    return false;
  }

  if (!firstSnapshotSeen) {
    firstSnapshotSeen = true;
    if (eventName == "put" && eventPath == "/" &&
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
      (eventPath == "/" && eventName == "patch")) {
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
  return WifiConnection::isConnected();
}

void loopMaintenance() {
  // No Firebase Auth is used in this permissive-rules demo version.
}

bool startListening(uint8_t channel) {
  stopListening();

  if (!WifiConnection::isConnected()) {
    Serial.println("[RTDB][STREAM] Start skipped: WiFi is disconnected");
    return false;
  }

  // Allocate the two event buffers before TLS fragments the heap. A normal
  // single-chunk Firebase event is about 4.5 KB and previously had to grow both
  // Strings after the TLS connection left only a similarly-sized free block.
  const bool lineReserved = sseLine.reserve(SSE_EVENT_RESERVE_BYTES);
  const bool dataReserved = sseData.reserve(SSE_EVENT_RESERVE_BYTES);
  const bool eventReserved = sseEvent.reserve(16);
  if (!lineReserved || !dataReserved || !eventReserved) {
    Serial.printf(
        "[RTDB][STREAM] buffer_reserve_failed line=%s data=%s event=%s freeHeap=%lu largestBlock=%lu\n",
        lineReserved ? "true" : "false",
        dataReserved ? "true" : "false",
        eventReserved ? "true" : "false",
        static_cast<unsigned long>(ESP.getFreeHeap()),
        static_cast<unsigned long>(
            heap_caps_get_largest_free_block(MALLOC_CAP_8BIT)));
    return false;
  }

  streamClient.setInsecure();
  setHttpRedirectPolicy(streamHttp);
  streamHttp.setTimeout(RtdbHttpConfig::STREAM_HTTP_TIMEOUT_MS);
  streamHttp.setConnectTimeout(RtdbHttpConfig::STREAM_CONNECT_TIMEOUT_MS);

  const String path = talkersPath(channel);
  const String url = rtdbUrlForPath(path);
  if (RtdbHttpConfig::LOG_STREAM_EVENTS) {
    Serial.printf("[RTDB][STREAM] Connecting path=%s\n", path.c_str());
  }

  logStreamHeap("before_http_begin");
  const bool began = streamHttp.begin(streamClient, url);
  logStreamHeap("after_http_begin");
  if (!began) {
    Serial.println("[RTDB][STREAM] Stream begin failed");
    resetStreamTransport();
    releaseStoppedStreamMemory();
    return false;
  }

  streamHttp.addHeader("Accept", "text/event-stream");
  logStreamHeap("before_tls_connect");
  const int code = streamHttp.GET();
  logStreamHeap("after_tls_connect");
  if (code != HTTP_CODE_OK) {
    Serial.printf("[RTDB][STREAM] Stream failed, HTTP %d\n", code);
    const String response = streamHttp.getString();
    if (response.length() > 0) {
      Serial.print("[RTDB][STREAM] Response: ");
      Serial.println(response);
    }
    resetStreamTransport();
    releaseStoppedStreamMemory();
    return false;
  }

  sseLine = "";
  sseEvent = "";
  sseData = "";
  droppingOversizedSseLine = false;
  firstSnapshotSeen = false;
  clearRemoteTalker("stream_start");
  streamChannel = channel;
  streamActive = true;

  Serial.print("[RTDB][STREAM] Listening on ");
  Serial.println(path);
  return true;
}

void stopListening() {
  const bool wasActive = streamActive;
  streamActive = false;
  streamChannel = 0;

  if (wasActive) {
    Serial.println("[RTDB][STREAM] Stopping stream");
  }

  resetStreamTransport();
  droppingOversizedSseLine = false;
  firstSnapshotSeen = false;
  clearRemoteTalker("stream_stop");
  releaseStoppedStreamMemory();

  Serial.printf(
      "[RTDB][STREAM] stopped freeHeap=%lu largestBlock=%lu\n",
      static_cast<unsigned long>(ESP.getFreeHeap()),
      static_cast<unsigned long>(heap_caps_get_largest_free_block(MALLOC_CAP_8BIT)));
}

bool isListening() {
  return streamActive;
}

bool pollListening(
    int16_t* outSamples,
    size_t maxSamples,
    RtdbPcmChunk& outChunk) {
  if (!streamActive || outSamples == nullptr || maxSamples == 0) {
    return false;
  }

  expireRemoteTalkerIfIdle();
  WiFiClient* stream = streamHttp.getStreamPtr();
  if (stream == nullptr) {
    Serial.println("[RTDB][STREAM] Stream pointer is null");
    stopListening();
    return false;
  }
  if (!stream->connected()) {
    Serial.println("[RTDB][STREAM] Stream disconnected");
    stopListening();
    return false;
  }

  while (stream->available() > 0) {
    const char value = static_cast<char>(stream->read());
    if (value == '\r') {
      continue;
    }

    if (value == '\n') {
      if (droppingOversizedSseLine) {
        droppingOversizedSseLine = false;
        sseLine = "";
        continue;
      }

      if (sseLine.length() == 0) {
        const bool received = processSseEvent(outSamples, maxSamples, outChunk);
        sseEvent = "";
        sseData = "";
        if (received) {
          return true;
        }
      } else if (sseLine.startsWith("event:")) {
        sseEvent = sseLine.substring(6);
        sseEvent.trim();
      } else if (sseLine.startsWith("data:")) {
        if (sseData.length() > 0) {
          sseData += '\n';
        }
        sseData += sseLine.substring(5);
        sseData.trim();
      }
      sseLine = "";
      continue;
    }

    if (!droppingOversizedSseLine && sseLine.length() < SSE_LINE_MAX_BYTES) {
      sseLine += value;
    } else if (!droppingOversizedSseLine) {
      Serial.println("[RTDB][STREAM] SSE line too long; dropping event");
      droppingOversizedSseLine = true;
      sseLine = "";
      sseEvent = "";
      sseData = "";
    }
  }

  (void)streamChannel;
  return false;
}

}  // namespace RtdbAudioStream
