#include "rtdb_audio_stream.h"

#include <HTTPClient.h>
#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <ctype.h>
#include <mbedtls/base64.h>

#include "app_config.h"
#include "wifi_connection.h"

namespace {

WiFiClientSecure streamClient;
HTTPClient streamHttp;
bool streamActive = false;
uint8_t streamChannel = 0;

String sseLine;
String sseEvent;
String sseData;

bool haveLastChunk = false;
String lastSessionId;
uint32_t lastSeq = 0;

bool firstSnapshotSeen = false;
String pendingSnapshot;
int pendingSnapshotPos = 0;

bool listenerHaveMeta = false;
bool listenerSessionActive = false;
String listenerSessionId;

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
  char buf[8];
  snprintf(buf, sizeof(buf), "ch%02u", channel);
  return String(buf);
}

String livePath(uint8_t channel) {
  return String("/rooms/") + AppConfig::ROOM_ID +
         "/channels/" + channelKey(channel) +
         "/live";
}

String chunksPath(uint8_t channel) {
  return livePath(channel) + "/chunks";
}

String metadataPath(uint8_t channel) {
  return livePath(channel) + "/meta";
}

String chunkKey(uint32_t seq) {
  char buf[12];
  snprintf(buf, sizeof(buf), "%08lu", static_cast<unsigned long>(seq));
  return String(buf);
}

String chunkPath(uint8_t channel, uint32_t seq) {
  return chunksPath(channel) + "/" + chunkKey(seq);
}

String rtdbUrlForPath(const String& path, bool silentWrite) {
  String url = databaseBaseUrl() + path + ".json";
  if (silentWrite) {
    url += "?print=silent";
  }
  return url;
}

void setHttpRedirectPolicy(HTTPClient& http) {
  http.setFollowRedirects(HTTPC_STRICT_FOLLOW_REDIRECTS);
}

void logStream(const char* message) {
  if (RtdbHttpConfig::LOG_STREAM_EVENTS) {
    Serial.print("[RTDB][STREAM] ");
    Serial.println(message);
  }
}

void logStreamf(const char* fmt, const char* a) {
  if (RtdbHttpConfig::LOG_STREAM_EVENTS) {
    Serial.print("[RTDB][STREAM] ");
    Serial.printf(fmt, a);
  }
}

bool extractJsonString(const String& json, const char* key, String& out) {
  const String needle = String("\"") + key + "\":";
  int pos = json.indexOf(needle);
  if (pos < 0) {
    return false;
  }

  pos += needle.length();
  while (pos < static_cast<int>(json.length()) && isspace(json[pos])) {
    ++pos;
  }

  if (pos >= static_cast<int>(json.length()) || json[pos] != '"') {
    return false;
  }
  ++pos;

  out = "";
  bool escaping = false;
  for (; pos < static_cast<int>(json.length()); ++pos) {
    const char c = json[pos];
    if (escaping) {
      switch (c) {
        case '"': out += '"'; break;
        case '\\': out += '\\'; break;
        case '/': out += '/'; break;
        case 'b': out += '\b'; break;
        case 'f': out += '\f'; break;
        case 'n': out += '\n'; break;
        case 'r': out += '\r'; break;
        case 't': out += '\t'; break;
        default: out += c; break;
      }
      escaping = false;
      continue;
    }

    if (c == '\\') {
      escaping = true;
    } else if (c == '"') {
      return true;
    } else {
      out += c;
    }
  }

  return false;
}

bool extractJsonUInt(const String& json, const char* key, uint32_t& out) {
  const String needle = String("\"") + key + "\":";
  int pos = json.indexOf(needle);
  if (pos < 0) {
    return false;
  }

  pos += needle.length();
  while (pos < static_cast<int>(json.length()) && isspace(json[pos])) {
    ++pos;
  }

  uint32_t value = 0;
  bool sawDigit = false;
  while (pos < static_cast<int>(json.length()) && isdigit(json[pos])) {
    sawDigit = true;
    value = value * 10 + static_cast<uint32_t>(json[pos] - '0');
    ++pos;
  }

  if (!sawDigit) {
    return false;
  }

  out = value;
  return true;
}

bool extractJsonBool(const String& json, const char* key, bool& out) {
  const String needle = String("\"") + key + "\":";
  int pos = json.indexOf(needle);
  if (pos < 0) {
    return false;
  }

  pos += needle.length();
  while (pos < static_cast<int>(json.length()) && isspace(json[pos])) {
    ++pos;
  }

  if (json.substring(pos, pos + 4) == "true") {
    out = true;
    return true;
  }
  if (json.substring(pos, pos + 5) == "false") {
    out = false;
    return true;
  }
  return false;
}

bool extractObjectAt(const String& json, int startPos, String& outObject) {
  if (startPos < 0 || startPos >= static_cast<int>(json.length()) || json[startPos] != '{') {
    return false;
  }

  bool inString = false;
  bool escaping = false;
  int depth = 0;

  for (int pos = startPos; pos < static_cast<int>(json.length()); ++pos) {
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
        outObject = json.substring(startPos, pos + 1);
        return true;
      }
    }
  }

  return false;
}

bool extractNamedObject(const String& json, const char* key, String& outObject) {
  const String needle = String("\"") + key + "\":";
  int pos = json.indexOf(needle);
  if (pos < 0) {
    return false;
  }

  pos += needle.length();
  while (pos < static_cast<int>(json.length()) && isspace(json[pos])) {
    ++pos;
  }

  if (pos >= static_cast<int>(json.length()) || json[pos] != '{') {
    return false;
  }

  return extractObjectAt(json, pos, outObject);
}

bool extractOuterDataObject(const String& eventJson, String& outObject) {
  const String needle = "\"data\":";
  int pos = eventJson.indexOf(needle);
  if (pos < 0) {
    return false;
  }

  pos += needle.length();
  while (pos < static_cast<int>(eventJson.length()) && isspace(eventJson[pos])) {
    ++pos;
  }

  if (pos >= static_cast<int>(eventJson.length()) || eventJson[pos] != '{') {
    return false;
  }

  return extractObjectAt(eventJson, pos, outObject);
}

bool outerDataIsNull(const String& eventJson) {
  const String needle = "\"data\":";
  int pos = eventJson.indexOf(needle);
  if (pos < 0) {
    return false;
  }

  pos += needle.length();
  while (pos < static_cast<int>(eventJson.length()) && isspace(eventJson[pos])) {
    ++pos;
  }

  return eventJson.substring(pos, pos + 4) == "null";
}

bool appendUploadChunkJsonObject(
    String& json,
    const RtdbUploadChunk& chunk,
    size_t& outPcmBytes,
    size_t& outEncodedLen) {
  outPcmBytes = 0;
  outEncodedLen = 0;

  if (chunk.sessionId == nullptr || chunk.sessionId[0] == '\0' ||
      chunk.samples == nullptr || chunk.sampleCount == 0) {
    Serial.printf(
        "[Talker][PACKET_BUILD_FAIL] seq=%lu reason=invalid_args session=%s samplesPtr=%s sampleCount=%u\n",
        static_cast<unsigned long>(chunk.seq),
        (chunk.sessionId == nullptr) ? "(null)" : chunk.sessionId,
        (chunk.samples == nullptr) ? "null" : "ok",
        static_cast<unsigned int>(chunk.sampleCount));
    return false;
  }

  if (chunk.sampleCount > AudioConfig::CHUNK_SAMPLES) {
    Serial.printf(
        "[Talker][PACKET_BUILD_FAIL] session=%s seq=%lu reason=oversized samples=%u maxSamples=%u\n",
        chunk.sessionId,
        static_cast<unsigned long>(chunk.seq),
        static_cast<unsigned int>(chunk.sampleCount),
        static_cast<unsigned int>(AudioConfig::CHUNK_SAMPLES));
    return false;
  }

  if (chunk.seq >= RtdbBufferConfig::MAX_CHUNKS_PER_SESSION) {
    Serial.printf(
        "[Talker][PACKET_BUILD_FAIL] session=%s seq=%lu reason=max_chunks_reached maxChunks=%lu\n",
        chunk.sessionId,
        static_cast<unsigned long>(chunk.seq),
        static_cast<unsigned long>(RtdbBufferConfig::MAX_CHUNKS_PER_SESSION));
    return false;
  }

  static char encoded[AudioConfig::BASE64_BUFFER_BYTES];
  outPcmBytes = chunk.sampleCount * sizeof(int16_t);

  const int rc = mbedtls_base64_encode(
      reinterpret_cast<unsigned char*>(encoded),
      sizeof(encoded),
      &outEncodedLen,
      reinterpret_cast<const unsigned char*>(chunk.samples),
      outPcmBytes);

  if (rc != 0 || outEncodedLen >= sizeof(encoded)) {
    Serial.printf(
        "[Talker][PACKET_BUILD_FAIL] session=%s seq=%lu reason=base64_encode_failed rc=%d pcmBytes=%u encodedLen=%u capacity=%u\n",
        chunk.sessionId,
        static_cast<unsigned long>(chunk.seq),
        rc,
        static_cast<unsigned int>(outPcmBytes),
        static_cast<unsigned int>(outEncodedLen),
        static_cast<unsigned int>(sizeof(encoded)));
    return false;
  }
  encoded[outEncodedLen] = '\0';

  json += "{\"seq\":";
  json += chunk.seq;
  json += ",\"sessionId\":\"";
  json += chunk.sessionId;
  json += "\",\"deviceId\":\"";
  json += AppConfig::DEVICE_ID;
  json += "\",\"format\":\"pcm_s16le_base64\",\"sampleRate\":";
  json += AudioConfig::SAMPLE_RATE;
  json += ",\"chunkMs\":";
  json += AudioConfig::CHUNK_MS;
  json += ",\"data\":\"";
  json += encoded;
  json += "\"}";

  return true;
}

bool httpPutJson(const String& path, const String& json, bool silentWrite, const char* label) {
  const unsigned long startMs = millis();

  if (!WifiConnection::isConnected()) {
    Serial.printf("[RTDB][%s] PUT skipped: WiFi is disconnected, path=%s\n", label, path.c_str());
    return false;
  }

  WiFiClientSecure client;
  client.setInsecure();

  HTTPClient http;
  setHttpRedirectPolicy(http);
  http.setTimeout(RtdbHttpConfig::HTTP_TIMEOUT_MS);
  http.setConnectTimeout(RtdbHttpConfig::CONNECT_TIMEOUT_MS);

  const String url = rtdbUrlForPath(path, silentWrite);
  if (RtdbHttpConfig::LOG_HTTP_REQUESTS) {
    Serial.printf(
        "[RTDB][%s] PUT start path=%s payloadBytes=%u silent=%s\n",
        label,
        path.c_str(),
        static_cast<unsigned int>(json.length()),
        boolText(silentWrite));
  }

  if (!http.begin(client, url)) {
    const unsigned long elapsedMs = millis() - startMs;
    Serial.printf(
        "[RTDB][%s] PUT begin failed path=%s durationMs=%lu\n",
        label,
        path.c_str(),
        static_cast<unsigned long>(elapsedMs));
    return false;
  }

  http.addHeader("Content-Type", "application/json");
  const int code = http.PUT(json);
  String response;
  if (code < 200 || code >= 300) {
    response = http.getString();
  }
  http.end();

  const unsigned long elapsedMs = millis() - startMs;
  if (code < 200 || code >= 300) {
    Serial.printf(
        "[RTDB][%s] PUT failed path=%s http=%d durationMs=%lu payloadBytes=%u\n",
        label,
        path.c_str(),
        code,
        static_cast<unsigned long>(elapsedMs),
        static_cast<unsigned int>(json.length()));
    if (response.length() > 0) {
      Serial.print("[RTDB] Error response: ");
      Serial.println(response);
    }
    return false;
  }

  if (RtdbHttpConfig::LOG_HTTP_REQUESTS) {
    Serial.printf(
        "[RTDB][%s] PUT ok path=%s http=%d durationMs=%lu payloadBytes=%u\n",
        label,
        path.c_str(),
        code,
        static_cast<unsigned long>(elapsedMs),
        static_cast<unsigned int>(json.length()));
  }
  return true;
}

bool httpPatchJson(const String& path, const String& json, bool silentWrite, const char* label) {
  const unsigned long startMs = millis();

  if (!WifiConnection::isConnected()) {
    Serial.printf("[RTDB][%s] PATCH skipped: WiFi is disconnected, path=%s\n", label, path.c_str());
    return false;
  }

  WiFiClientSecure client;
  client.setInsecure();

  HTTPClient http;
  setHttpRedirectPolicy(http);
  http.setTimeout(RtdbHttpConfig::HTTP_TIMEOUT_MS);
  http.setConnectTimeout(RtdbHttpConfig::CONNECT_TIMEOUT_MS);

  const String url = rtdbUrlForPath(path, silentWrite);
  if (RtdbHttpConfig::LOG_HTTP_REQUESTS) {
    Serial.printf(
        "[RTDB][%s] PATCH start path=%s payloadBytes=%u silent=%s\n",
        label,
        path.c_str(),
        static_cast<unsigned int>(json.length()),
        boolText(silentWrite));
  }

  if (!http.begin(client, url)) {
    const unsigned long elapsedMs = millis() - startMs;
    Serial.printf(
        "[RTDB][%s] PATCH begin failed path=%s durationMs=%lu\n",
        label,
        path.c_str(),
        static_cast<unsigned long>(elapsedMs));
    return false;
  }

  http.addHeader("Content-Type", "application/json");
  const int code = http.sendRequest(
      "PATCH",
      reinterpret_cast<uint8_t*>(const_cast<char*>(json.c_str())),
      json.length());
  String response;
  if (code < 200 || code >= 300) {
    response = http.getString();
  }
  http.end();

  const unsigned long elapsedMs = millis() - startMs;
  if (code < 200 || code >= 300) {
    Serial.printf(
        "[RTDB][%s] PATCH failed path=%s http=%d durationMs=%lu payloadBytes=%u\n",
        label,
        path.c_str(),
        code,
        static_cast<unsigned long>(elapsedMs),
        static_cast<unsigned int>(json.length()));
    if (response.length() > 0) {
      Serial.print("[RTDB] Error response: ");
      Serial.println(response);
    }
    return false;
  }

  if (RtdbHttpConfig::LOG_HTTP_REQUESTS) {
    Serial.printf(
        "[RTDB][%s] PATCH ok path=%s http=%d durationMs=%lu payloadBytes=%u\n",
        label,
        path.c_str(),
        code,
        static_cast<unsigned long>(elapsedMs),
        static_cast<unsigned int>(json.length()));
  }
  return true;
}

bool httpDeletePath(const String& path, bool silentWrite, const char* label) {
  const unsigned long startMs = millis();

  if (!WifiConnection::isConnected()) {
    Serial.printf("[RTDB][%s] DELETE skipped: WiFi is disconnected, path=%s\n", label, path.c_str());
    return false;
  }

  WiFiClientSecure client;
  client.setInsecure();

  HTTPClient http;
  setHttpRedirectPolicy(http);
  http.setTimeout(RtdbHttpConfig::HTTP_TIMEOUT_MS);
  http.setConnectTimeout(RtdbHttpConfig::CONNECT_TIMEOUT_MS);

  const String url = rtdbUrlForPath(path, silentWrite);
  if (RtdbHttpConfig::LOG_HTTP_REQUESTS) {
    Serial.printf(
        "[RTDB][%s] DELETE start path=%s silent=%s\n",
        label,
        path.c_str(),
        boolText(silentWrite));
  }

  if (!http.begin(client, url)) {
    const unsigned long elapsedMs = millis() - startMs;
    Serial.printf(
        "[RTDB][%s] DELETE begin failed path=%s durationMs=%lu\n",
        label,
        path.c_str(),
        static_cast<unsigned long>(elapsedMs));
    return false;
  }

  const int code = http.sendRequest("DELETE");
  String response;
  if (code < 200 || code >= 300) {
    response = http.getString();
  }
  http.end();

  const unsigned long elapsedMs = millis() - startMs;
  if (code < 200 || code >= 300) {
    Serial.printf(
        "[RTDB][%s] DELETE failed path=%s http=%d durationMs=%lu\n",
        label,
        path.c_str(),
        code,
        static_cast<unsigned long>(elapsedMs));
    if (response.length() > 0) {
      Serial.print("[RTDB] Error response: ");
      Serial.println(response);
    }
    return false;
  }

  if (RtdbHttpConfig::LOG_HTTP_REQUESTS) {
    Serial.printf(
        "[RTDB][%s] DELETE ok path=%s http=%d durationMs=%lu\n",
        label,
        path.c_str(),
        code,
        static_cast<unsigned long>(elapsedMs));
  }
  return true;
}

bool shouldSkipChunk(const String& sessionId, uint32_t seq) {
  if (!haveLastChunk) {
    return false;
  }

  if (sessionId != lastSessionId) {
    return false;
  }

  return seq <= lastSeq;
}

bool decodeChunkObject(
    const String& chunkObject,
    int16_t* outSamples,
    size_t maxSamples,
    RtdbPcmChunk& outChunk) {
  uint32_t seq = 0;
  String encoded;
  String sessionId;

  if (!extractJsonUInt(chunkObject, "seq", seq)) {
    if (RtdbHttpConfig::LOG_STREAM_EVENTS) {
      Serial.println("[RTDB][STREAM][CHUNK_IGNORE] missing seq field");
    }
    return false;
  }

  if (!extractJsonString(chunkObject, "data", encoded)) {
    if (RtdbHttpConfig::LOG_STREAM_EVENTS) {
      Serial.printf(
          "[RTDB][STREAM][CHUNK_IGNORE] session=? seq=%lu missing data field\n",
          static_cast<unsigned long>(seq));
    }
    return false;
  }

  if (!extractJsonString(chunkObject, "sessionId", sessionId)) {
    sessionId = "legacy";
  }

  if (shouldSkipChunk(sessionId, seq)) {
    if (RtdbHttpConfig::LOG_STREAM_EVENTS) {
      Serial.printf(
          "[RTDB][STREAM][CHUNK_SKIP_OLD] session=%s seq=%lu lastSession=%s lastSeq=%lu\n",
          sessionId.c_str(),
          static_cast<unsigned long>(seq),
          lastSessionId.c_str(),
          static_cast<unsigned long>(lastSeq));
    }
    return false;
  }

  const size_t outCapacityBytes = maxSamples * sizeof(int16_t);
  size_t decodedBytes = 0;
  const int rc = mbedtls_base64_decode(
      reinterpret_cast<unsigned char*>(outSamples),
      outCapacityBytes,
      &decodedBytes,
      reinterpret_cast<const unsigned char*>(encoded.c_str()),
      encoded.length());

  if (rc != 0 || decodedBytes == 0 || (decodedBytes % sizeof(int16_t)) != 0) {
    Serial.printf(
        "[RTDB][STREAM][CHUNK_DECODE_FAIL] session=%s seq=%lu rc=%d base64Bytes=%u decodedBytes=%u capacityBytes=%u\n",
        sessionId.c_str(),
        static_cast<unsigned long>(seq),
        rc,
        static_cast<unsigned int>(encoded.length()),
        static_cast<unsigned int>(decodedBytes),
        static_cast<unsigned int>(outCapacityBytes));
    return false;
  }

  uint32_t sampleRate = AudioConfig::SAMPLE_RATE;
  uint32_t chunkMs = AudioConfig::CHUNK_MS;
  extractJsonUInt(chunkObject, "sampleRate", sampleRate);
  extractJsonUInt(chunkObject, "chunkMs", chunkMs);

  lastSessionId = sessionId;
  lastSeq = seq;
  haveLastChunk = true;

  outChunk.seq = seq;
  outChunk.sampleRate = sampleRate;
  outChunk.chunkMs = static_cast<uint16_t>(chunkMs);
  outChunk.sampleCount = decodedBytes / sizeof(int16_t);
  outChunk.sessionId = sessionId;

  if (RtdbHttpConfig::LOG_STREAM_EVENTS) {
    Serial.printf(
        "[RTDB][STREAM][CHUNK_DECODE_OK] session=%s seq=%lu samples=%u sampleRate=%lu chunkMs=%lu base64Bytes=%u\n",
        outChunk.sessionId.c_str(),
        static_cast<unsigned long>(outChunk.seq),
        static_cast<unsigned int>(outChunk.sampleCount),
        static_cast<unsigned long>(outChunk.sampleRate),
        static_cast<unsigned long>(outChunk.chunkMs),
        static_cast<unsigned int>(encoded.length()));
  }
  return true;
}

void updateListenerMetaFromObject(const String& metaObject) {
  bool active = false;
  const bool haveActive = extractJsonBool(metaObject, "active", active);
  String sessionId;
  const bool haveSession = extractJsonString(metaObject, "sessionId", sessionId);

  if (haveActive) {
    listenerSessionActive = active;
    listenerHaveMeta = true;
  }
  if (haveSession) {
    listenerSessionId = sessionId;
    listenerHaveMeta = true;
  }

  if (RtdbHttpConfig::LOG_STREAM_EVENTS) {
    Serial.printf(
        "[RTDB][STREAM][META] haveActive=%s active=%s haveSession=%s session=%s\n",
        boolText(haveActive),
        boolText(listenerSessionActive),
        boolText(haveSession),
        listenerSessionId.c_str());
  }
}

bool shouldDecodeSnapshotChunks() {
  // Normal behavior: do not replay old RTDB chunks when the listener boots.
  // But if the listener reconnects during an active session, the root snapshot is
  // useful because it lets the receiver catch up on chunks missed while offline.
  if (RtdbBufferConfig::PLAY_EXISTING_CHUNKS_ON_CONNECT) {
    return true;
  }
  return listenerHaveMeta && listenerSessionActive;
}

bool decodeNextPendingSnapshot(
    int16_t* outSamples,
    size_t maxSamples,
    RtdbPcmChunk& outChunk) {
  while (pendingSnapshot.length() > 0 &&
         pendingSnapshotPos < static_cast<int>(pendingSnapshot.length())) {
    int keyStart = pendingSnapshot.indexOf('"', pendingSnapshotPos);
    if (keyStart < 0) {
      pendingSnapshot = "";
      pendingSnapshotPos = 0;
      return false;
    }

    int keyEnd = pendingSnapshot.indexOf('"', keyStart + 1);
    if (keyEnd < 0) {
      pendingSnapshot = "";
      pendingSnapshotPos = 0;
      return false;
    }

    const String key = pendingSnapshot.substring(keyStart + 1, keyEnd);

    int colon = pendingSnapshot.indexOf(':', keyEnd + 1);
    if (colon < 0) {
      pendingSnapshot = "";
      pendingSnapshotPos = 0;
      return false;
    }

    int objectStart = colon + 1;
    while (objectStart < static_cast<int>(pendingSnapshot.length()) &&
           isspace(pendingSnapshot[objectStart])) {
      ++objectStart;
    }

    if (objectStart >= static_cast<int>(pendingSnapshot.length()) ||
        pendingSnapshot[objectStart] != '{') {
      pendingSnapshotPos = objectStart + 1;
      continue;
    }

    String chunkObject;
    if (!extractObjectAt(pendingSnapshot, objectStart, chunkObject)) {
      pendingSnapshot = "";
      pendingSnapshotPos = 0;
      return false;
    }

    pendingSnapshotPos = objectStart + chunkObject.length();
    if (RtdbHttpConfig::LOG_STREAM_EVENTS) {
      Serial.printf(
          "[RTDB][STREAM][SNAPSHOT_CHUNK] key=%s objectBytes=%u\n",
          key.c_str(),
          static_cast<unsigned int>(chunkObject.length()));
    }

    if (decodeChunkObject(chunkObject, outSamples, maxSamples, outChunk)) {
      return true;
    }
  }

  pendingSnapshot = "";
  pendingSnapshotPos = 0;
  return false;
}

bool setPendingSnapshotFromChunkMap(const String& chunkMap) {
  if (chunkMap.length() == 0) {
    return false;
  }

  pendingSnapshot = chunkMap;
  pendingSnapshotPos = 0;
  if (RtdbHttpConfig::LOG_STREAM_EVENTS) {
    Serial.printf(
        "[RTDB][STREAM][SNAPSHOT_QUEUE] chunkMapBytes=%u active=%s session=%s\n",
        static_cast<unsigned int>(pendingSnapshot.length()),
        boolText(listenerSessionActive),
        listenerSessionId.c_str());
  }
  return true;
}

bool processLiveRootSnapshot(
    const String& liveObject,
    int16_t* outSamples,
    size_t maxSamples,
    RtdbPcmChunk& outChunk) {
  String metaObject;
  if (extractNamedObject(liveObject, "meta", metaObject)) {
    updateListenerMetaFromObject(metaObject);
  } else if (RtdbHttpConfig::LOG_STREAM_EVENTS) {
    Serial.println("[RTDB][STREAM][ROOT] no meta object in initial snapshot");
  }

  String chunkMap;
  if (!extractNamedObject(liveObject, "chunks", chunkMap)) {
    if (RtdbHttpConfig::LOG_STREAM_EVENTS) {
      Serial.println("[RTDB][STREAM][ROOT] no chunks object in initial snapshot");
    }
    return false;
  }

  if (!firstSnapshotSeen) {
    firstSnapshotSeen = true;
    if (!shouldDecodeSnapshotChunks()) {
      if (RtdbHttpConfig::LOG_STREAM_EVENTS) {
        Serial.printf(
            "[RTDB][STREAM][ROOT_IGNORE] first snapshot ignored because active=%s playExisting=%s\n",
            boolText(listenerSessionActive),
            boolText(RtdbBufferConfig::PLAY_EXISTING_CHUNKS_ON_CONNECT));
      }
      return false;
    }
  }

  setPendingSnapshotFromChunkMap(chunkMap);
  return decodeNextPendingSnapshot(outSamples, maxSamples, outChunk);
}

bool processChunksObject(
    const String& dataObject,
    int16_t* outSamples,
    size_t maxSamples,
    RtdbPcmChunk& outChunk) {
  uint32_t dummySeq = 0;
  if (extractJsonUInt(dataObject, "seq", dummySeq)) {
    return decodeChunkObject(dataObject, outSamples, maxSamples, outChunk);
  }

  // A PUT/PATCH can occasionally provide a map of chunks instead of a single
  // chunk. Queue it and decode one chunk per poll call.
  setPendingSnapshotFromChunkMap(dataObject);
  return decodeNextPendingSnapshot(outSamples, maxSamples, outChunk);
}

bool processSseEvent(
    int16_t* outSamples,
    size_t maxSamples,
    RtdbPcmChunk& outChunk) {
  const String eventName = trimCopy(sseEvent);
  if (eventName.length() == 0) {
    return false;
  }

  if (eventName != "put" && eventName != "patch") {
    if (RtdbHttpConfig::LOG_STREAM_EVENTS) {
      Serial.printf("[RTDB][STREAM][EVENT_IGNORE] name=%s dataBytes=%u\n", eventName.c_str(), static_cast<unsigned int>(sseData.length()));
    }
    return false;
  }

  String eventPath;
  if (!extractJsonString(sseData, "path", eventPath)) {
    if (RtdbHttpConfig::LOG_STREAM_EVENTS) {
      Serial.printf("[RTDB][STREAM][EVENT_BAD] name=%s reason=missing_path dataBytes=%u\n", eventName.c_str(), static_cast<unsigned int>(sseData.length()));
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

  String dataObject;
  if (!extractOuterDataObject(sseData, dataObject)) {
    if (RtdbHttpConfig::LOG_STREAM_EVENTS) {
      Serial.printf(
          "[RTDB][STREAM][EVENT_IGNORE] path=%s reason=%s\n",
          eventPath.c_str(),
          outerDataIsNull(sseData) ? "data_null" : "data_not_object");
    }
    return false;
  }

  // Listener now streams /live instead of only /chunks. This lets it use
  // /meta.active to decide whether a root snapshot is stale or belongs to an
  // ongoing session.
  if (eventPath == "/") {
    return processLiveRootSnapshot(dataObject, outSamples, maxSamples, outChunk);
  }

  if (eventPath == "/meta") {
    updateListenerMetaFromObject(dataObject);
    return false;
  }

  if (eventPath == "/chunks" || eventPath.startsWith("/chunks/")) {
    if (!listenerSessionActive && !RtdbBufferConfig::PLAY_EXISTING_CHUNKS_ON_CONNECT) {
      if (RtdbHttpConfig::LOG_STREAM_EVENTS) {
        Serial.printf(
            "[RTDB][STREAM][CHUNK_IGNORE] path=%s reason=session_not_active knownSession=%s\n",
            eventPath.c_str(),
            listenerSessionId.c_str());
      }
      return false;
    }
    return processChunksObject(dataObject, outSamples, maxSamples, outChunk);
  }

  if (RtdbHttpConfig::LOG_STREAM_EVENTS) {
    Serial.printf("[RTDB][STREAM][EVENT_IGNORE] path=%s reason=unhandled_path\n", eventPath.c_str());
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

bool beginTransmission(uint8_t channel, const char* sessionId) {
  if (sessionId == nullptr || sessionId[0] == '\0') {
    return false;
  }

  // Clear the old buffered chunks when a new push-to-talk session starts.
  // The listener ignores delete/null stream events and waits for new chunks.
  httpDeletePath(chunksPath(channel), true, "SESSION_CLEAR");

  String meta;
  meta.reserve(180);
  meta += "{\"active\":true,\"sessionId\":\"";
  meta += sessionId;
  meta += "\",\"deviceId\":\"";
  meta += AppConfig::DEVICE_ID;
  meta += "\",\"sampleRate\":";
  meta += AudioConfig::SAMPLE_RATE;
  meta += ",\"chunkMs\":";
  meta += AudioConfig::CHUNK_MS;
  meta += "}";

  const bool ok = httpPutJson(metadataPath(channel), meta, true, "SESSION_META");
  if (ok) {
    Serial.print("[RTDB] Started session ");
    Serial.println(sessionId);
  }
  return ok;
}

bool endTransmission(uint8_t channel, const char* sessionId, uint32_t lastSeqInSession) {
  if (sessionId == nullptr || sessionId[0] == '\0') {
    return false;
  }

  String meta;
  meta.reserve(180);
  meta += "{\"active\":false,\"sessionId\":\"";
  meta += sessionId;
  meta += "\",\"deviceId\":\"";
  meta += AppConfig::DEVICE_ID;
  meta += "\",\"lastSeq\":";
  meta += lastSeqInSession;
  meta += ",\"sampleRate\":";
  meta += AudioConfig::SAMPLE_RATE;
  meta += ",\"chunkMs\":";
  meta += AudioConfig::CHUNK_MS;
  meta += "}";

  const bool ok = httpPutJson(metadataPath(channel), meta, true, "SESSION_META");
  if (ok) {
    Serial.print("[RTDB] Ended session ");
    Serial.println(sessionId);
  }
  return ok;
}

bool uploadPcmChunk(
    uint8_t channel,
    const char* sessionId,
    uint32_t seq,
    const int16_t* samples,
    size_t sampleCount) {
  const unsigned long packetStartMs = millis();

  if (!WifiConnection::isConnected()) {
    Serial.printf(
        "[Talker][PACKET_SEND_FAIL] seq=%lu reason=wifi_disconnected\n",
        static_cast<unsigned long>(seq));
    return false;
  }

  if (sessionId == nullptr || sessionId[0] == '\0' || samples == nullptr || sampleCount == 0) {
    Serial.printf(
        "[Talker][PACKET_SEND_FAIL] seq=%lu reason=invalid_packet_args session=%s samplesPtr=%s sampleCount=%u\n",
        static_cast<unsigned long>(seq),
        (sessionId == nullptr) ? "(null)" : sessionId,
        (samples == nullptr) ? "null" : "ok",
        static_cast<unsigned int>(sampleCount));
    return false;
  }

  if (sampleCount > AudioConfig::CHUNK_SAMPLES) {
    Serial.printf(
        "[Talker][PACKET_SEND_FAIL] session=%s seq=%lu reason=oversized samples=%u maxSamples=%u\n",
        sessionId,
        static_cast<unsigned long>(seq),
        static_cast<unsigned int>(sampleCount),
        static_cast<unsigned int>(AudioConfig::CHUNK_SAMPLES));
    return false;
  }

  if (seq >= RtdbBufferConfig::MAX_CHUNKS_PER_SESSION) {
    Serial.printf(
        "[Talker][PACKET_SEND_FAIL] session=%s seq=%lu reason=max_chunks_reached maxChunks=%lu\n",
        sessionId,
        static_cast<unsigned long>(seq),
        static_cast<unsigned long>(RtdbBufferConfig::MAX_CHUNKS_PER_SESSION));
    return false;
  }

  static char encoded[AudioConfig::BASE64_BUFFER_BYTES];
  size_t encodedLen = 0;
  const size_t pcmBytes = sampleCount * sizeof(int16_t);

  if (RtdbHttpConfig::LOG_EVERY_AUDIO_PACKET) {
    Serial.printf(
        "[Talker][PACKET_PREP] session=%s seq=%lu channel=%u samples=%u pcmBytes=%u\n",
        sessionId,
        static_cast<unsigned long>(seq),
        static_cast<unsigned int>(channel),
        static_cast<unsigned int>(sampleCount),
        static_cast<unsigned int>(pcmBytes));
  }

  const int rc = mbedtls_base64_encode(
      reinterpret_cast<unsigned char*>(encoded),
      sizeof(encoded),
      &encodedLen,
      reinterpret_cast<const unsigned char*>(samples),
      pcmBytes);

  if (rc != 0 || encodedLen >= sizeof(encoded)) {
    Serial.printf(
        "[Talker][PACKET_SEND_FAIL] session=%s seq=%lu reason=base64_encode_failed rc=%d pcmBytes=%u encodedLen=%u capacity=%u\n",
        sessionId,
        static_cast<unsigned long>(seq),
        rc,
        static_cast<unsigned int>(pcmBytes),
        static_cast<unsigned int>(encodedLen),
        static_cast<unsigned int>(sizeof(encoded)));
    return false;
  }
  encoded[encodedLen] = '\0';

  String json;
  json.reserve(encodedLen + 260);
  json += "{\"seq\":";
  json += seq;
  json += ",\"sessionId\":\"";
  json += sessionId;
  json += "\",\"deviceId\":\"";
  json += AppConfig::DEVICE_ID;
  json += "\",\"format\":\"pcm_s16le_base64\",\"sampleRate\":";
  json += AudioConfig::SAMPLE_RATE;
  json += ",\"chunkMs\":";
  json += AudioConfig::CHUNK_MS;
  json += ",\"data\":\"";
  json += encoded;
  json += "\"}";

  const String path = chunkPath(channel, seq);
  if (RtdbHttpConfig::LOG_EVERY_AUDIO_PACKET) {
    Serial.printf(
        "[Talker][PACKET_SEND_START] session=%s seq=%lu path=%s base64Bytes=%u jsonBytes=%u\n",
        sessionId,
        static_cast<unsigned long>(seq),
        path.c_str(),
        static_cast<unsigned int>(encodedLen),
        static_cast<unsigned int>(json.length()));
  }

  const bool ok = httpPutJson(path, json, true, "AUDIO_PACKET");
  const unsigned long totalMs = millis() - packetStartMs;

  Serial.printf(
      "[Talker][PACKET_SEND_%s] session=%s seq=%lu channel=%u samples=%u pcmBytes=%u base64Bytes=%u jsonBytes=%u totalMs=%lu\n",
      ok ? "OK" : "FAIL",
      sessionId,
      static_cast<unsigned long>(seq),
      static_cast<unsigned int>(channel),
      static_cast<unsigned int>(sampleCount),
      static_cast<unsigned int>(pcmBytes),
      static_cast<unsigned int>(encodedLen),
      static_cast<unsigned int>(json.length()),
      static_cast<unsigned long>(totalMs));

  return ok;
}

bool uploadPcmChunkBatch(
    uint8_t channel,
    const RtdbUploadChunk* chunks,
    size_t chunkCount) {
  const unsigned long batchStartMs = millis();

  if (!WifiConnection::isConnected()) {
    Serial.printf(
        "[Talker][BATCH_SEND_FAIL] reason=wifi_disconnected chunkCount=%u\n",
        static_cast<unsigned int>(chunkCount));
    return false;
  }

  if (chunks == nullptr || chunkCount == 0) {
    Serial.println("[Talker][BATCH_SEND_FAIL] reason=empty_or_null_batch");
    return false;
  }

  if (chunkCount > RtdbUploadConfig::UPLOAD_BATCH_MAX_CHUNKS) {
    Serial.printf(
        "[Talker][BATCH_SEND_FAIL] reason=batch_too_large chunkCount=%u max=%u\n",
        static_cast<unsigned int>(chunkCount),
        static_cast<unsigned int>(RtdbUploadConfig::UPLOAD_BATCH_MAX_CHUNKS));
    return false;
  }

  const uint32_t firstSeq = chunks[0].seq;
  const uint32_t lastSeq = chunks[chunkCount - 1].seq;

  if (RtdbHttpConfig::LOG_EVERY_AUDIO_PACKET) {
    Serial.printf(
        "[Talker][BATCH_PREP] count=%u firstSeq=%lu lastSeq=%lu freeHeap=%lu\n",
        static_cast<unsigned int>(chunkCount),
        static_cast<unsigned long>(firstSeq),
        static_cast<unsigned long>(lastSeq),
        static_cast<unsigned long>(ESP.getFreeHeap()));
  }

  String json;
  json.reserve((AudioConfig::BASE64_BUFFER_BYTES + 280) * chunkCount + 2);
  json += "{";

  size_t totalPcmBytes = 0;
  size_t totalEncodedBytes = 0;

  for (size_t i = 0; i < chunkCount; ++i) {
    if (i > 0) {
      json += ",";
    }

    json += "\"";
    json += chunkKey(chunks[i].seq);
    json += "\":";

    size_t pcmBytes = 0;
    size_t encodedLen = 0;
    if (!appendUploadChunkJsonObject(json, chunks[i], pcmBytes, encodedLen)) {
      Serial.printf(
          "[Talker][BATCH_BUILD_FAIL] count=%u failedIndex=%u firstSeq=%lu lastSeq=%lu\n",
          static_cast<unsigned int>(chunkCount),
          static_cast<unsigned int>(i),
          static_cast<unsigned long>(firstSeq),
          static_cast<unsigned long>(lastSeq));
      return false;
    }

    totalPcmBytes += pcmBytes;
    totalEncodedBytes += encodedLen;
  }

  json += "}";

  const String path = chunksPath(channel);
  Serial.printf(
      "[Talker][BATCH_SEND_START] count=%u firstSeq=%lu lastSeq=%lu path=%s pcmBytes=%u base64Bytes=%u jsonBytes=%u freeHeap=%lu\n",
      static_cast<unsigned int>(chunkCount),
      static_cast<unsigned long>(firstSeq),
      static_cast<unsigned long>(lastSeq),
      path.c_str(),
      static_cast<unsigned int>(totalPcmBytes),
      static_cast<unsigned int>(totalEncodedBytes),
      static_cast<unsigned int>(json.length()),
      static_cast<unsigned long>(ESP.getFreeHeap()));

  const bool ok = httpPatchJson(path, json, true, "AUDIO_BATCH");
  const unsigned long totalMs = millis() - batchStartMs;

  Serial.printf(
      "[Talker][BATCH_SEND_%s] count=%u firstSeq=%lu lastSeq=%lu channel=%u pcmBytes=%u base64Bytes=%u jsonBytes=%u totalMs=%lu freeHeap=%lu\n",
      ok ? "OK" : "FAIL",
      static_cast<unsigned int>(chunkCount),
      static_cast<unsigned long>(firstSeq),
      static_cast<unsigned long>(lastSeq),
      static_cast<unsigned int>(channel),
      static_cast<unsigned int>(totalPcmBytes),
      static_cast<unsigned int>(totalEncodedBytes),
      static_cast<unsigned int>(json.length()),
      static_cast<unsigned long>(totalMs),
      static_cast<unsigned long>(ESP.getFreeHeap()));

  for (size_t i = 0; i < chunkCount; ++i) {
    Serial.printf(
        "[Talker][PACKET_SEND_%s] session=%s seq=%lu via=batch batchCount=%u totalMs=%lu\n",
        ok ? "OK" : "FAIL",
        chunks[i].sessionId,
        static_cast<unsigned long>(chunks[i].seq),
        static_cast<unsigned int>(chunkCount),
        static_cast<unsigned long>(totalMs));
  }

  return ok;
}

bool startListening(uint8_t channel) {
  stopListening();

  if (!WifiConnection::isConnected()) {
    Serial.println("[RTDB][STREAM] Start skipped: WiFi is disconnected");
    return false;
  }

  streamClient.setInsecure();
  setHttpRedirectPolicy(streamHttp);
  streamHttp.setTimeout(RtdbHttpConfig::STREAM_HTTP_TIMEOUT_MS);
  streamHttp.setConnectTimeout(RtdbHttpConfig::STREAM_CONNECT_TIMEOUT_MS);

  // Stream the whole /live object, not only /chunks. This lets the receiver use
  // /meta.active to avoid replaying stale chunks while still catching up after a
  // reconnect during an active transmission.
  const String path = livePath(channel);
  const String url = rtdbUrlForPath(path, false);

  if (RtdbHttpConfig::LOG_STREAM_EVENTS) {
    Serial.printf("[RTDB][STREAM] Connecting path=%s\n", path.c_str());
  }

  if (!streamHttp.begin(streamClient, url)) {
    Serial.println("[RTDB][STREAM] Stream begin failed");
    return false;
  }

  streamHttp.addHeader("Accept", "text/event-stream");
  const int code = streamHttp.GET();
  if (code != HTTP_CODE_OK) {
    Serial.printf("[RTDB][STREAM] Stream failed, HTTP %d\n", code);
    String response = streamHttp.getString();
    if (response.length() > 0) {
      Serial.print("[RTDB][STREAM] Response: ");
      Serial.println(response);
    }
    streamHttp.end();
    return false;
  }

  sseLine = "";
  sseEvent = "";
  sseData = "";
  pendingSnapshot = "";
  pendingSnapshotPos = 0;
  firstSnapshotSeen = false;
  listenerHaveMeta = false;
  listenerSessionActive = false;
  listenerSessionId = "";
  streamChannel = channel;
  streamActive = true;

  Serial.print("[RTDB][STREAM] Listening on ");
  Serial.println(path);
  return true;
}

void stopListening() {
  if (streamActive) {
    Serial.println("[RTDB][STREAM] Stopping stream");
    streamHttp.end();
  }
  streamActive = false;
  streamChannel = 0;
  sseLine = "";
  sseEvent = "";
  sseData = "";
  pendingSnapshot = "";
  pendingSnapshotPos = 0;
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

  if (decodeNextPendingSnapshot(outSamples, maxSamples, outChunk)) {
    return true;
  }

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
    const char c = static_cast<char>(stream->read());

    if (c == '\r') {
      continue;
    }

    if (c == '\n') {
      if (sseLine.length() == 0) {
        const bool gotChunk = processSseEvent(outSamples, maxSamples, outChunk);
        sseEvent = "";
        sseData = "";
        if (gotChunk) {
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
      } else if (RtdbHttpConfig::LOG_STREAM_EVENTS) {
        Serial.printf("[RTDB][STREAM][LINE_IGNORE] %s\n", sseLine.c_str());
      }

      sseLine = "";
    } else {
      // One audio event line is roughly 4.5 KB with the current chunk size.
      // Root snapshots can be much larger, so allow a larger line before dropping.
      if (sseLine.length() < 32768) {
        sseLine += c;
      } else {
        Serial.println("[RTDB][STREAM] SSE line too long; dropping event");
        sseLine = "";
        sseEvent = "";
        sseData = "";
        pendingSnapshot = "";
        pendingSnapshotPos = 0;
      }
    }
  }

  (void)streamChannel;
  return false;
}

}  // namespace RtdbAudioStream
