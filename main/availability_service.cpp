#include "availability_service.h"

#include <HTTPClient.h>
#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <esp_now.h>
#include <esp_wifi.h>
#include <esp_idf_version.h>
#if __has_include(<esp_arduino_version.h>)
#include <esp_arduino_version.h>
#endif
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <ctype.h>
#include <limits.h>
#include <string.h>
#include <sys/time.h>
#include <time.h>

#include "app_config.h"
#include "wifi_connection.h"

namespace {

constexpr uint8_t ESPNOW_BROADCAST_MAC[6] = {0xff, 0xff, 0xff, 0xff, 0xff, 0xff};
constexpr uint32_t VALID_EPOCH_MIN_SECONDS = 1700000000UL;
constexpr uint32_t UNKNOWN_AGE_SECONDS = 0xffffffffUL;
constexpr uint64_t NO_P2P_SLOT_HANDLED = 0xffffffffffffffffULL;

struct PackedPresencePacket {
  uint8_t magic0;
  uint8_t magic1;
  uint8_t version;
  uint8_t userNumber;
  uint8_t logicalChannel;
  char userId[3];
  uint32_t uptimeMs;
  uint32_t periodMs;
  uint32_t statsWindowMs;
  uint64_t slotId;
} __attribute__((packed));

struct InternalUserStatus {
  uint8_t userNumber = 0;
  bool voipAvailable = false;
  bool p2pAvailable = false;
  uint32_t voipAgeSeconds = UNKNOWN_AGE_SECONDS;
  uint32_t p2pAgeSeconds = UNKNOWN_AGE_SECONDS;
  uint64_t lastVoipServerMs = 0;
  unsigned long lastP2pSeenMs = 0;
  uint64_t lastP2pSlotId = 0;
};

InternalUserStatus users[AvailabilityConfig::USER_COUNT];
TaskHandle_t availabilityTaskHandle = nullptr;
portMUX_TYPE usersMux = portMUX_INITIALIZER_UNLOCKED;

bool espNowReady = false;
bool timeConfigStarted = false;
unsigned long lastTimeConfigAttemptMs = 0;
uint64_t lastHandledP2pSlot = NO_P2P_SLOT_HANDLED;

String reusablePayload;
String reusableResponse;

bool logEnabled() {
  return AvailabilityConfig::LOG_AVAILABILITY;
}

String databaseBaseUrl() {
  String base = FirebaseConfig::DATABASE_URL;
  while (base.endsWith("/")) {
    base.remove(base.length() - 1);
  }
  return base;
}

String rtdbUrlForPath(const String& path, bool silentWrite) {
  String url = databaseBaseUrl() + path + ".json";
  if (silentWrite) {
    url += "?print=silent";
  }
  return url;
}

String userPath(const char* userId) {
  return String(AvailabilityConfig::RTDB_USERS_PATH) + "/user_" + userId;
}

uint8_t userNumberFromId(const char* id) {
  if (id == nullptr || id[0] < '0' || id[0] > '9' || id[1] < '0' || id[1] > '9') {
    return 0;
  }
  const uint8_t value = static_cast<uint8_t>((id[0] - '0') * 10 + (id[1] - '0'));
  if (value < 1 || value > AvailabilityConfig::USER_COUNT) {
    return 0;
  }
  return value;
}

void userIdFromNumber(uint8_t userNumber, char* out, size_t outSize) {
  if (out == nullptr || outSize < 3) {
    return;
  }
  snprintf(out, outSize, "%02u", static_cast<unsigned int>(userNumber));
}

uint32_t voipOfflineThresholdMs() {
  return static_cast<uint32_t>(
      (AvailabilityConfig::PERIOD_TIME_MS * AvailabilityConfig::VOIP_OFFLINE_THRESHOLD_NUMERATOR) /
      AvailabilityConfig::VOIP_OFFLINE_THRESHOLD_DENOMINATOR);
}

uint32_t p2pOfflineThresholdMs() {
  return static_cast<uint32_t>(
      AvailabilityConfig::PERIOD_TIME_MS * AvailabilityConfig::P2P_OFFLINE_AFTER_MISSED_PERIODS);
}

bool currentEpochMs(uint64_t& outEpochMs) {
  struct timeval tv;
  if (gettimeofday(&tv, nullptr) != 0 ||
      tv.tv_sec < static_cast<time_t>(VALID_EPOCH_MIN_SECONDS)) {
    outEpochMs = 0;
    return false;
  }
  outEpochMs = static_cast<uint64_t>(tv.tv_sec) * 1000ULL +
               static_cast<uint64_t>(tv.tv_usec / 1000);
  return true;
}

void maybeStartTimeSync() {
  const unsigned long nowMs = millis();
  if (!WifiConnection::isConnected()) {
    return;
  }

  uint64_t nowEpochMs = 0;
  if (currentEpochMs(nowEpochMs)) {
    return;
  }

  if (timeConfigStarted && (nowMs - lastTimeConfigAttemptMs) < AvailabilityConfig::TIME_SYNC_CHECK_MS) {
    return;
  }

  lastTimeConfigAttemptMs = nowMs;
  timeConfigStarted = true;
  configTzTime(
      AvailabilityConfig::TIME_ZONE_POSIX,
      AvailabilityConfig::NTP_SERVER_1,
      AvailabilityConfig::NTP_SERVER_2);

  if (logEnabled()) {
    Serial.printf(
        "[Availability][TIME_SYNC_START] tzName=%s tz=%s\n",
        AvailabilityConfig::TIME_ZONE_NAME,
        AvailabilityConfig::TIME_ZONE_POSIX);
  }
}

void buildLocalIso(char* out, size_t outSize) {
  if (out == nullptr || outSize == 0) {
    return;
  }
  out[0] = '\0';

  const time_t now = time(nullptr);
  if (now < static_cast<time_t>(VALID_EPOCH_MIN_SECONDS)) {
    snprintf(out, outSize, "unsynced");
    return;
  }

  struct tm localTm;
  if (localtime_r(&now, &localTm) == nullptr) {
    snprintf(out, outSize, "time_error");
    return;
  }

  strftime(out, outSize, "%Y-%m-%dT%H:%M:%S", &localTm);
}

void buildRtdbHeartbeatPayload(String& payload) {
  payload = "";
  payload.reserve(420);

  uint64_t epochMs = 0;
  const bool haveTime = currentEpochMs(epochMs);

  char localIso[32];
  buildLocalIso(localIso, sizeof(localIso));

  payload += "{\"userId\":\"";
  payload += AppConfig::USER_ID;
  payload += "\",\"deviceId\":\"";
  payload += AppConfig::DEVICE_ID;
  payload += "\",\"roomId\":\"";
  payload += AppConfig::ROOM_ID;
  payload += "\",\"lastSeenServerMs\":{\".sv\":\"timestamp\"},\"deviceEpochMs\":";
  char epochBuf[24];
  snprintf(epochBuf, sizeof(epochBuf), "%llu", static_cast<unsigned long long>(haveTime ? epochMs : 0ULL));
  payload += epochBuf;
  payload += ",\"uptimeMs\":";
  payload += static_cast<unsigned long>(millis());
  payload += ",\"periodMs\":";
  payload += static_cast<unsigned long>(AvailabilityConfig::PERIOD_TIME_MS);
  payload += ",\"timezone\":\"";
  payload += AvailabilityConfig::TIME_ZONE_NAME;
  payload += "\",\"tz\":\"";
  payload += AvailabilityConfig::TIME_ZONE_POSIX;
  payload += "\",\"localTime\":\"";
  payload += localIso;
  payload += "\"}";
}

bool httpPutJson(const String& path, const String& json, bool silentWrite, const char* label) {
  if (!WifiConnection::isConnected()) {
    if (logEnabled()) {
      Serial.printf("[Availability][%s] PUT skipped: WiFi disconnected path=%s\n", label, path.c_str());
    }
    return false;
  }

  WiFiClientSecure client;
  client.setInsecure();

  HTTPClient http;
  http.setFollowRedirects(HTTPC_STRICT_FOLLOW_REDIRECTS);
  http.useHTTP10(true);
  http.setTimeout(AvailabilityConfig::RTDB_HTTP_TIMEOUT_MS);
  http.setConnectTimeout(AvailabilityConfig::RTDB_CONNECT_TIMEOUT_MS);

  const String url = rtdbUrlForPath(path, silentWrite);
  if (!http.begin(client, url)) {
    if (logEnabled()) {
      Serial.printf("[Availability][%s] PUT begin failed path=%s\n", label, path.c_str());
    }
    return false;
  }

  http.addHeader("Content-Type", "application/json");
  http.addHeader("Connection", "close");
  const int code = http.PUT(json);
  String response;
  if (code < 200 || code >= 300) {
    response = http.getString();
  }
  http.end();

  const bool ok = code >= 200 && code < 300;
  if (logEnabled()) {
    Serial.printf(
        "[Availability][%s] PUT_%s path=%s http=%d bytes=%u\n",
        label,
        ok ? "OK" : "FAIL",
        path.c_str(),
        code,
        static_cast<unsigned int>(json.length()));
    if (!ok && response.length() > 0) {
      Serial.print("[Availability][RTDB_RESPONSE] ");
      Serial.println(response);
    }
  }
  return ok;
}

bool httpGetJson(const String& path, String& out, const char* label) {
  out = "";
  if (!WifiConnection::isConnected()) {
    if (logEnabled()) {
      Serial.printf("[Availability][%s] GET skipped: WiFi disconnected path=%s\n", label, path.c_str());
    }
    return false;
  }

  WiFiClientSecure client;
  client.setInsecure();

  HTTPClient http;
  http.setFollowRedirects(HTTPC_STRICT_FOLLOW_REDIRECTS);
  http.useHTTP10(true);
  http.setTimeout(AvailabilityConfig::RTDB_HTTP_TIMEOUT_MS);
  http.setConnectTimeout(AvailabilityConfig::RTDB_CONNECT_TIMEOUT_MS);

  const String url = rtdbUrlForPath(path, false);
  if (!http.begin(client, url)) {
    if (logEnabled()) {
      Serial.printf("[Availability][%s] GET begin failed path=%s\n", label, path.c_str());
    }
    return false;
  }

  http.addHeader("Connection", "close");
  const int code = http.GET();
  if (code >= 200 && code < 300) {
    out = http.getString();
  } else {
    String response = http.getString();
    if (logEnabled()) {
      Serial.printf(
          "[Availability][%s] GET_FAIL path=%s http=%d responseBytes=%u\n",
          label,
          path.c_str(),
          code,
          static_cast<unsigned int>(response.length()));
      if (response.length() > 0) {
        Serial.print("[Availability][RTDB_RESPONSE] ");
        Serial.println(response);
      }
    }
  }
  http.end();

  const bool ok = code >= 200 && code < 300;
  if (ok && logEnabled()) {
    Serial.printf(
        "[Availability][%s] GET_OK path=%s http=%d bytes=%u\n",
        label,
        path.c_str(),
        code,
        static_cast<unsigned int>(out.length()));
  }
  return ok;
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

bool extractJsonUInt64(const String& json, const char* key, uint64_t& out) {
  const String needle = String("\"") + key + "\":";
  int pos = json.indexOf(needle);
  if (pos < 0) {
    return false;
  }

  pos += needle.length();
  while (pos < static_cast<int>(json.length()) && isspace(json[pos])) {
    ++pos;
  }

  uint64_t value = 0;
  bool sawDigit = false;
  while (pos < static_cast<int>(json.length()) && isdigit(json[pos])) {
    sawDigit = true;
    value = value * 10ULL + static_cast<uint64_t>(json[pos] - '0');
    ++pos;
  }

  if (!sawDigit) {
    return false;
  }
  out = value;
  return true;
}

bool writeSelfRtdbHeartbeat() {
  buildRtdbHeartbeatPayload(reusablePayload);
  bool ok = false;

  for (uint8_t attempt = 1; attempt <= AvailabilityConfig::RTDB_REQUEST_MAX_ATTEMPTS; ++attempt) {
    ok = httpPutJson(userPath(AppConfig::USER_ID), reusablePayload, true, "SELF_HEARTBEAT");
    if (ok) {
      break;
    }
    if (attempt < AvailabilityConfig::RTDB_REQUEST_MAX_ATTEMPTS) {
      vTaskDelay(pdMS_TO_TICKS(AvailabilityConfig::RTDB_RETRY_DELAY_MS));
      WifiConnection::ensureConnected();
    }
  }

  return ok;
}

void setVoipStatusFromRtdb(uint8_t userNumber, bool available, uint32_t ageSeconds, uint64_t lastSeenServerMs) {
  if (userNumber < 1 || userNumber > AvailabilityConfig::USER_COUNT) {
    return;
  }

  portENTER_CRITICAL(&usersMux);
  InternalUserStatus& user = users[userNumber - 1];
  user.voipAvailable = available;
  user.voipAgeSeconds = ageSeconds;
  user.lastVoipServerMs = lastSeenServerMs;
  portEXIT_CRITICAL(&usersMux);
}

void markSelfVoipRecentlyWrittenIfNeeded(bool writeOk) {
  if (!writeOk) {
    return;
  }
  const uint8_t selfNumber = userNumberFromId(AppConfig::USER_ID);
  if (selfNumber == 0) {
    return;
  }
  portENTER_CRITICAL(&usersMux);
  users[selfNumber - 1].voipAvailable = true;
  users[selfNumber - 1].voipAgeSeconds = 0;
  portEXIT_CRITICAL(&usersMux);
}

void readAllRtdbHeartbeats() {
  bool ok = false;
  for (uint8_t attempt = 1; attempt <= AvailabilityConfig::RTDB_REQUEST_MAX_ATTEMPTS; ++attempt) {
    ok = httpGetJson(AvailabilityConfig::RTDB_USERS_PATH, reusableResponse, "USERS_READ");
    if (ok) {
      break;
    }
    if (attempt < AvailabilityConfig::RTDB_REQUEST_MAX_ATTEMPTS) {
      vTaskDelay(pdMS_TO_TICKS(AvailabilityConfig::RTDB_RETRY_DELAY_MS));
      WifiConnection::ensureConnected();
    }
  }
  if (!ok) {
    return;
  }

  uint64_t nowEpochMs = 0;
  const bool haveCurrentTime = currentEpochMs(nowEpochMs);
  const uint32_t thresholdMs = voipOfflineThresholdMs();

  for (uint8_t userNumber = 1; userNumber <= AvailabilityConfig::USER_COUNT; ++userNumber) {
    char id[3];
    userIdFromNumber(userNumber, id, sizeof(id));

    String userObject;
    const String key = String("user_") + id;
    if (!extractNamedObject(reusableResponse, key.c_str(), userObject)) {
      setVoipStatusFromRtdb(userNumber, false, UNKNOWN_AGE_SECONDS, 0);
      continue;
    }

    uint64_t lastSeenServerMs = 0;
    if (!extractJsonUInt64(userObject, "lastSeenServerMs", lastSeenServerMs) || lastSeenServerMs == 0 || !haveCurrentTime) {
      setVoipStatusFromRtdb(userNumber, false, UNKNOWN_AGE_SECONDS, lastSeenServerMs);
      continue;
    }

    uint64_t rawAgeMs = 0;
    if (nowEpochMs >= lastSeenServerMs) {
      rawAgeMs = nowEpochMs - lastSeenServerMs;
    }

    const bool available = rawAgeMs <= thresholdMs;
    const uint32_t ageSeconds = static_cast<uint32_t>(rawAgeMs / 1000ULL);
    setVoipStatusFromRtdb(userNumber, available, ageSeconds, lastSeenServerMs);
  }
}

void updateP2pFreshness() {
  const unsigned long now = millis();
  const uint32_t threshold = p2pOfflineThresholdMs();

  portENTER_CRITICAL(&usersMux);
  for (uint8_t i = 0; i < AvailabilityConfig::USER_COUNT; ++i) {
    InternalUserStatus& user = users[i];
    if (user.lastP2pSeenMs == 0) {
      user.p2pAvailable = false;
      user.p2pAgeSeconds = UNKNOWN_AGE_SECONDS;
      continue;
    }

    const unsigned long ageMs = now - user.lastP2pSeenMs;
    user.p2pAvailable = ageMs <= threshold;
    user.p2pAgeSeconds = static_cast<uint32_t>(ageMs / 1000UL);
  }
  portEXIT_CRITICAL(&usersMux);
}

void markP2pSeen(uint8_t userNumber, uint64_t slotId) {
  if (userNumber < 1 || userNumber > AvailabilityConfig::USER_COUNT) {
    return;
  }

  portENTER_CRITICAL(&usersMux);
  InternalUserStatus& user = users[userNumber - 1];
  user.lastP2pSeenMs = millis();
  user.lastP2pSlotId = slotId;
  user.p2pAvailable = true;
  user.p2pAgeSeconds = 0;
  portEXIT_CRITICAL(&usersMux);
}

bool parsePresencePacket(const uint8_t* data, int len, uint8_t& outUserNumber, uint64_t& outSlotId) {
  outUserNumber = 0;
  outSlotId = 0;
  if (data == nullptr || len < static_cast<int>(sizeof(PackedPresencePacket))) {
    return false;
  }

  PackedPresencePacket packet;
  memcpy(&packet, data, sizeof(packet));

  if (packet.magic0 != 'W' || packet.magic1 != 'A' || packet.version != 3) {
    return false;
  }

  if (packet.userNumber < 1 || packet.userNumber > AvailabilityConfig::USER_COUNT) {
    return false;
  }

  if (packet.logicalChannel != AvailabilityConfig::P2P_STATS_LOGICAL_CHANNEL) {
    return false;
  }

  outUserNumber = packet.userNumber;
  outSlotId = packet.slotId;
  return true;
}

#if defined(ESP_ARDUINO_VERSION_MAJOR) && ESP_ARDUINO_VERSION_MAJOR >= 3
void onEspNowReceive(const esp_now_recv_info_t*, const uint8_t* data, int len) {
#else
void onEspNowReceive(const uint8_t*, const uint8_t* data, int len) {
#endif
  uint8_t userNumber = 0;
  uint64_t slotId = 0;
  if (parsePresencePacket(data, len, userNumber, slotId)) {
    markP2pSeen(userNumber, slotId);
  }
}

void initEspNowIfNeeded() {
  if (espNowReady) {
    return;
  }

  if (esp_now_init() != ESP_OK) {
    if (logEnabled()) {
      Serial.println("[Availability][ESPNOW_INIT_FAIL]");
    }
    return;
  }

  esp_now_register_recv_cb(onEspNowReceive);

  esp_now_peer_info_t peerInfo;
  memset(&peerInfo, 0, sizeof(peerInfo));
  memcpy(peerInfo.peer_addr, ESPNOW_BROADCAST_MAC, sizeof(ESPNOW_BROADCAST_MAC));
  peerInfo.channel = AvailabilityConfig::P2P_ESPNOW_CURRENT_WIFI_CHANNEL;
  peerInfo.encrypt = false;
#if defined(ESP_IDF_VERSION_MAJOR) && ESP_IDF_VERSION_MAJOR >= 4
  peerInfo.ifidx = WIFI_IF_STA;
#endif

  const esp_err_t addPeerResult = esp_now_add_peer(&peerInfo);
  if (addPeerResult != ESP_OK && addPeerResult != ESP_ERR_ESPNOW_EXIST) {
    if (logEnabled()) {
      Serial.printf("[Availability][ESPNOW_PEER_FAIL] err=%d\n", static_cast<int>(addPeerResult));
    }
    esp_now_deinit();
    return;
  }

  espNowReady = true;
  if (logEnabled()) {
    uint8_t homeChannel = 0;
    wifi_second_chan_t secondChannel = WIFI_SECOND_CHAN_NONE;
    esp_wifi_get_channel(&homeChannel, &secondChannel);
    Serial.printf(
        "[Availability][ESPNOW_READY] peerChannel=current homeChannel=%u statsLogicalChannel=%u syncWindowMs=%u txOffsetStepMs=%u p2pTimeoutMs=%lu\n",
        static_cast<unsigned int>(homeChannel),
        static_cast<unsigned int>(AvailabilityConfig::P2P_STATS_LOGICAL_CHANNEL),
        static_cast<unsigned int>(AvailabilityConfig::P2P_STATS_SYNC_WINDOW_MS),
        static_cast<unsigned int>(AvailabilityConfig::P2P_STATS_USER_TX_OFFSET_STEP_MS),
        static_cast<unsigned long>(p2pOfflineThresholdMs()));
  }
}

bool sendEspNowPresencePacket(uint64_t slotId) {
  initEspNowIfNeeded();
  if (!espNowReady) {
    return false;
  }

  const uint8_t selfNumber = userNumberFromId(AppConfig::USER_ID);
  if (selfNumber == 0) {
    return false;
  }

  PackedPresencePacket packet;
  memset(&packet, 0, sizeof(packet));
  packet.magic0 = 'W';
  packet.magic1 = 'A';
  packet.version = 3;
  packet.userNumber = selfNumber;
  packet.logicalChannel = AvailabilityConfig::P2P_STATS_LOGICAL_CHANNEL;
  snprintf(packet.userId, sizeof(packet.userId), "%02u", static_cast<unsigned int>(selfNumber));
  packet.uptimeMs = static_cast<uint32_t>(millis());
  packet.periodMs = static_cast<uint32_t>(AvailabilityConfig::PERIOD_TIME_MS);
  packet.statsWindowMs = static_cast<uint32_t>(AvailabilityConfig::P2P_STATS_SYNC_WINDOW_MS);
  packet.slotId = slotId;

  bool anyOk = false;
  esp_err_t lastSendResult = ESP_FAIL;
  for (uint8_t repeat = 0; repeat < AvailabilityConfig::P2P_STATS_BROADCAST_REPEATS; ++repeat) {
    lastSendResult = esp_now_send(
        ESPNOW_BROADCAST_MAC,
        reinterpret_cast<const uint8_t*>(&packet),
        sizeof(packet));

    if (lastSendResult == ESP_OK) {
      anyOk = true;
    }

    if (repeat + 1 < AvailabilityConfig::P2P_STATS_BROADCAST_REPEATS) {
      vTaskDelay(pdMS_TO_TICKS(AvailabilityConfig::P2P_STATS_BROADCAST_GAP_MS));
    }
  }

  if (anyOk) {
    markP2pSeen(selfNumber, slotId);
  }

  if (logEnabled()) {
    Serial.printf(
        "[Availability][P2P_BEACON_%s] user=%02u slot=%llu peerChannel=current statsLogicalChannel=%u repeats=%u lastErr=%d\n",
        anyOk ? "OK" : "FAIL",
        static_cast<unsigned int>(selfNumber),
        static_cast<unsigned long long>(slotId),
        static_cast<unsigned int>(AvailabilityConfig::P2P_STATS_LOGICAL_CHANNEL),
        static_cast<unsigned int>(AvailabilityConfig::P2P_STATS_BROADCAST_REPEATS),
        static_cast<int>(lastSendResult));
  }
  return anyOk;
}


bool runSynchronizedP2pWindowIfDue() {
  uint64_t epochMs = 0;
  if (!currentEpochMs(epochMs)) {
    return false;
  }

  const uint64_t periodMs = AvailabilityConfig::PERIOD_TIME_MS;
  const uint64_t windowMs = AvailabilityConfig::P2P_STATS_SYNC_WINDOW_MS;
  if (periodMs == 0 || windowMs == 0 || windowMs >= periodMs) {
    return false;
  }

  const uint64_t slotId = epochMs / periodMs;
  const uint64_t slotStartMs = slotId * periodMs;
  const uint64_t offsetInSlotMs = epochMs - slotStartMs;
  if (offsetInSlotMs >= windowMs) {
    return false;
  }

  if (slotId == lastHandledP2pSlot) {
    return false;
  }
  lastHandledP2pSlot = slotId;

  initEspNowIfNeeded();
  if (!espNowReady) {
    return false;
  }

  const uint8_t selfNumber = userNumberFromId(AppConfig::USER_ID);
  if (selfNumber == 0) {
    return false;
  }

  uint8_t homeChannel = 0;
  wifi_second_chan_t secondChannel = WIFI_SECOND_CHAN_NONE;
  esp_wifi_get_channel(&homeChannel, &secondChannel);

  const uint16_t txOffsetMs = static_cast<uint16_t>(
      selfNumber * AvailabilityConfig::P2P_STATS_USER_TX_OFFSET_STEP_MS);
  const uint64_t targetTxEpochMs = slotStartMs + txOffsetMs;

  if (epochMs < targetTxEpochMs && targetTxEpochMs < (slotStartMs + windowMs)) {
    const uint64_t waitMs = targetTxEpochMs - epochMs;
    if (waitMs > 0 && waitMs < windowMs) {
      vTaskDelay(pdMS_TO_TICKS(static_cast<uint32_t>(waitMs)));
    }
  }

  const bool sent = sendEspNowPresencePacket(slotId);

  uint64_t afterSendEpochMs = 0;
  if (currentEpochMs(afterSendEpochMs)) {
    const uint64_t windowEndEpochMs = slotStartMs + windowMs;
    if (afterSendEpochMs < windowEndEpochMs) {
      const uint64_t listenLeftMs = windowEndEpochMs - afterSendEpochMs;
      if (listenLeftMs > 0 && listenLeftMs <= windowMs) {
        vTaskDelay(pdMS_TO_TICKS(static_cast<uint32_t>(listenLeftMs)));
      }
    }
  } else {
    vTaskDelay(pdMS_TO_TICKS(AvailabilityConfig::P2P_STATS_SYNC_WINDOW_MS));
  }

  updateP2pFreshness();

  if (logEnabled()) {
    Serial.printf(
        "[Availability][P2P_SYNC_WINDOW] slot=%llu offsetInSlotMs=%llu txOffsetMs=%u sent=%s homeChannel=%u\n",
        static_cast<unsigned long long>(slotId),
        static_cast<unsigned long long>(offsetInSlotMs),
        static_cast<unsigned int>(txOffsetMs),
        sent ? "true" : "false",
        static_cast<unsigned int>(homeChannel));
  }

  return true;
}

void initializeUsers() {
  portENTER_CRITICAL(&usersMux);
  for (uint8_t i = 0; i < AvailabilityConfig::USER_COUNT; ++i) {
    users[i] = InternalUserStatus();
    users[i].userNumber = i + 1;
  }
  portEXIT_CRITICAL(&usersMux);
}

void runRtdbCycle() {
  const bool writeOk = writeSelfRtdbHeartbeat();
  markSelfVoipRecentlyWrittenIfNeeded(writeOk);
  vTaskDelay(pdMS_TO_TICKS(AvailabilityConfig::TASK_SHORT_YIELD_MS));
  readAllRtdbHeartbeats();
}

void availabilityTask(void*) {
  reusablePayload.reserve(512);
  reusableResponse.reserve(2048);

  unsigned long lastRtdbCycleMs = 0;

  for (;;) {
    maybeStartTimeSync();
    runSynchronizedP2pWindowIfDue();
    updateP2pFreshness();

    const unsigned long nowMs = millis();
    const bool rtdbDue = lastRtdbCycleMs == 0 ||
                         (nowMs - lastRtdbCycleMs) >= AvailabilityConfig::PERIOD_TIME_MS;

    // Avoid intentionally starting a blocking RTDB GET/PUT inside the P2P sync
    // window. If an HTTP call runs long it may still skip a window, which is why
    // P2P availability tolerates several missed windows before marking offline.
    uint64_t epochMs = 0;
    bool insideP2pWindow = false;
    if (currentEpochMs(epochMs)) {
      insideP2pWindow = (epochMs % AvailabilityConfig::PERIOD_TIME_MS) <
                        AvailabilityConfig::P2P_STATS_SYNC_WINDOW_MS;
    }

    if (rtdbDue && !insideP2pWindow) {
      lastRtdbCycleMs = millis();
      runRtdbCycle();
      updateP2pFreshness();
    }

    vTaskDelay(pdMS_TO_TICKS(AvailabilityConfig::TASK_LOOP_DELAY_MS));
  }
}

}  // namespace

namespace AvailabilityService {

bool begin() {
  if (availabilityTaskHandle != nullptr) {
    return true;
  }

  initializeUsers();
  maybeStartTimeSync();
  initEspNowIfNeeded();

  const BaseType_t created = xTaskCreatePinnedToCore(
      availabilityTask,
      "availability",
      AvailabilityConfig::TASK_STACK_BYTES,
      nullptr,
      AvailabilityConfig::TASK_PRIORITY,
      &availabilityTaskHandle,
      AvailabilityConfig::TASK_CORE);

  if (created != pdPASS) {
    availabilityTaskHandle = nullptr;
    Serial.println("[ERROR] Failed creating availability task");
    return false;
  }

  Serial.printf(
      "[READY] Availability task initialized users=%u periodMs=%lu voipThresholdMs=%lu p2pTimeoutMs=%lu syncWindowMs=%u freeHeap=%lu\n",
      static_cast<unsigned int>(AvailabilityConfig::USER_COUNT),
      static_cast<unsigned long>(AvailabilityConfig::PERIOD_TIME_MS),
      static_cast<unsigned long>(voipOfflineThresholdMs()),
      static_cast<unsigned long>(p2pOfflineThresholdMs()),
      static_cast<unsigned int>(AvailabilityConfig::P2P_STATS_SYNC_WINDOW_MS),
      static_cast<unsigned long>(ESP.getFreeHeap()));
  return true;
}

bool isRunning() {
  return availabilityTaskHandle != nullptr;
}

size_t copyUserStatuses(UserStatus* outStatuses, size_t maxStatuses) {
  if (outStatuses == nullptr || maxStatuses == 0) {
    return 0;
  }

  const size_t count = (maxStatuses < AvailabilityConfig::USER_COUNT) ? maxStatuses : AvailabilityConfig::USER_COUNT;

  portENTER_CRITICAL(&usersMux);
  for (size_t i = 0; i < count; ++i) {
    outStatuses[i].userNumber = users[i].userNumber;
    outStatuses[i].voipAvailable = users[i].voipAvailable;
    outStatuses[i].p2pAvailable = users[i].p2pAvailable;
    outStatuses[i].voipAgeSeconds = users[i].voipAgeSeconds;
    outStatuses[i].p2pAgeSeconds = users[i].p2pAgeSeconds;
  }
  portEXIT_CRITICAL(&usersMux);

  return count;
}

}  // namespace AvailabilityService
