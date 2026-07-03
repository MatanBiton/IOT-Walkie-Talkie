#include <Arduino.h>
#include <WiFi.h>
#include <HTTPClient.h>
#include <WiFiClientSecure.h>
#include <driver/i2s.h>
#include <esp_err.h>
#include <esp_system.h>
#include "mbedtls/base64.h"

// =====================================================
// ESP32 MIC -> LIVE FIREBASE RTDB WALKIE-TALKIE SENDER
//
// Flow:
//   1. Hold button: create a live session under /rooms/<ROOM_ID>
//   2. While button is held: record ~200 ms PCM chunk, Base64 it, upload immediately
//   3. Release button: mark the live session complete
//
// Audio format stored in each chunk:
//   pcm_s16le_base64
//   8000 Hz
//   mono
//   signed int16 little-endian
//
// Receiver should listen/read:
//   /rooms/<ROOM_ID>/live
//   /rooms/<ROOM_ID>/sessions/<sessionId>/chunks/<000000,000001,...>
// =====================================================

// ---------------- Wi-Fi ----------------
// Fill these locally. Do not paste real credentials into logs/chats.
const char* WIFI_SSID = "Adi";
const char* WIFI_PASSWORD = "0502092099";

// ---------------- Firebase ----------------
// Example:
// https://your-project-id-default-rtdb.europe-west1.firebasedatabase.app
// or:
// https://your-project-id-default-rtdb.firebaseio.com
// Do not include a trailing slash and do not include .json here.
const char* FIREBASE_DATABASE_URL = "https://walki-talkie-37ed3-default-rtdb.europe-west1.firebasedatabase.app/";


// Optional Auth. Since your public RTDB test worked, keep this disabled until
// the live flow is stable. Then set USE_AUTH_FOR_RTDB_UPLOAD to 1 and fill these.
#define USE_AUTH_FOR_RTDB_UPLOAD 0
const char* FIREBASE_API_KEY = "AIzaSyBd_mLyDFaZesLOGV7K0pxPD_M3LD2fQ6U";
const char* FIREBASE_EMAIL = "elironaviron@gmail.com";
const char* FIREBASE_PASSWORD = "Arduino123";

// ---------------- Room/device ----------------
const char* ROOM_ID = "room1";
const char* DEVICE_ID = "esp32-audio-01";

// ---------------- Audio settings from your original sketch ----------------
#define SAMPLE_RATE                 8000

#define AUDIO_SAMPLES_PER_PACKET    118
#define MIC_READ_SAMPLES            AUDIO_SAMPLES_PER_PACKET

#define MIC_SHIFT                   13
#define MIC_GAIN                    1

#define ENABLE_NOISE_GATE           1
#define NOISE_GATE_LEVEL            80

#define ENABLE_TX_LIMITER           1
#define TX_LIMIT_ABS                22000

// ---------------- Live chunking ----------------
/// 125 ms at 8 kHz = 1000 samples = 2000 raw bytes = ~2668 Base64 chars.
// Smaller chunks reduce per-packet latency and make packet loss less noticeable.
#define LIVE_CHUNK_MS               125
#define LIVE_CHUNK_SAMPLES          ((SAMPLE_RATE * LIVE_CHUNK_MS) / 1000)
#define LIVE_CHUNK_BYTES            (LIVE_CHUNK_SAMPLES * sizeof(int16_t))

// Safety cap in case the button is stuck.
#define MAX_LIVE_SECONDS            20
#define MAX_LIVE_CHUNKS             ((1000 / LIVE_CHUNK_MS) * MAX_LIVE_SECONDS)

// The recorder and uploader run independently.
// This queue buffers audio while HTTPS requests are in progress.
// 12 chunks * 4000 bytes/chunk ~= 48 KB of audio buffer = about 3 seconds.
// This gives the uploader time to recover from temporary RTDB/HTTPS stalls.
#define LIVE_UPLOAD_QUEUE_LEN       12
#define UPLOAD_TASK_STACK_BYTES     16384

// For real-time audio, do not block too long on a bad Firebase request.
#define RTDB_HTTP_TIMEOUT_MS        6000
#define RTDB_SEND_RETRIES           2
#define RTDB_RETRY_BASE_DELAY_MS    250

// Real-time policy:
// Prefer dropping old backlog over delaying current audio.
#define REALTIME_DROP_OLD_BACKLOG   1

// For live walkie-talkie behavior, one failed chunk should not kill the session.
#define ABORT_ON_CHUNK_UPLOAD_FAIL  0

// ---------------- Button + LED from your sketch ----------------
#define BUTTON_PIN                  GPIO_NUM_14
#define TX_LED_PIN                  GPIO_NUM_4
#define BUTTON_DEBOUNCE_MS          45

// ---------------- I2S microphone pins from your sketch ----------------
#define I2S_MIC_PORT                I2S_NUM_0
#define I2S_MIC_CHANNEL             I2S_CHANNEL_FMT_ONLY_RIGHT
#define I2S_MIC_BCLK                GPIO_NUM_32
#define I2S_MIC_WS                  GPIO_NUM_25
#define I2S_MIC_SD                  GPIO_NUM_33

static int32_t micRaw[MIC_READ_SAMPLES];
static int16_t livePcmChunk[LIVE_CHUNK_SAMPLES];  // scratch buffer used when dropping chunks

typedef struct {
  uint32_t seq;
  uint16_t sampleCount;
  uint32_t capturedAtMillis;
  int16_t samples[LIVE_CHUNK_SAMPLES];
} LiveAudioChunk;

typedef struct {
  String sessionId;
  QueueHandle_t freeQueue;
  QueueHandle_t filledQueue;
  SemaphoreHandle_t doneSemaphore;
  volatile bool recorderDone;
  volatile bool uploadError;
  volatile uint32_t capturedChunks;
  volatile uint32_t droppedChunks;
  volatile uint32_t uploadedChunks;
  volatile uint32_t lastUploadedSeq;
  volatile size_t uploadedSamples;
} LiveUploadContext;

static LiveAudioChunk liveChunkPool[LIVE_UPLOAD_QUEUE_LEN];
static LiveUploadContext liveUploadCtx;

String firebaseIdToken = "";
unsigned long tokenExpiresAtMs = 0;
static int32_t dcEstimate = 0;

static WiFiClientSecure rtdbSecureClient;
static bool rtdbSecureClientReady = false;

WiFiClientSecure& getRtdbClient() {
  if (!rtdbSecureClientReady) {
    rtdbSecureClient.setInsecure();

    rtdbSecureClientReady = true;
  }

  return rtdbSecureClient;
}

void resetRtdbClient() {
  rtdbSecureClient.stop();
  rtdbSecureClientReady = false;
}

// ---------------- Small helpers ----------------
static int16_t clampToInt16(int32_t value) {
  if (value > 32767) return 32767;
  if (value < -32768) return -32768;
  return (int16_t)value;
}

static int32_t limitAudioSample(int32_t value) {
#if ENABLE_NOISE_GATE
  if (value > -NOISE_GATE_LEVEL && value < NOISE_GATE_LEVEL) {
    value = 0;
  }
#endif

#if ENABLE_TX_LIMITER
  if (value > TX_LIMIT_ABS) return TX_LIMIT_ABS;
  if (value < -TX_LIMIT_ABS) return -TX_LIMIT_ABS;
#endif

  return value;
}

void printErr(const char *label, esp_err_t err) {
  if (err == ESP_OK) {
    Serial.printf("[OK]   %s\n", label);
  } else {
    Serial.printf("[FAIL] %s: %d / %s\n", label, err, esp_err_to_name(err));
  }
}

String extractJsonString(const String& json, const String& key) {
  String pattern = "\"" + key + "\":\"";
  int start = json.indexOf(pattern);
  if (start < 0) return "";

  start += pattern.length();
  int end = json.indexOf("\"", start);
  if (end < 0) return "";

  return json.substring(start, end);
}

String jsonEscape(const String& input) {
  String out;
  out.reserve(input.length() + 8);

  for (size_t i = 0; i < input.length(); i++) {
    char c = input[i];
    switch (c) {
      case '\\': out += "\\\\"; break;
      case '"':  out += "\\\""; break;
      case '\n': out += "\\n"; break;
      case '\r': out += "\\r"; break;
      case '\t': out += "\\t"; break;
      default:   out += c; break;
    }
  }

  return out;
}

String sanitizeRtdbKey(String key) {
  // RTDB keys cannot contain: . # $ [ ] /
  key.replace(".", "_");
  key.replace("#", "_");
  key.replace("$", "_");
  key.replace("[", "_");
  key.replace("]", "_");
  key.replace("/", "_");
  key.replace(" ", "_");
  return key;
}

String chunkName(uint32_t index) {
  char buf[12];
  snprintf(buf, sizeof(buf), "%06lu", (unsigned long)index);
  return String(buf);
}

String makeSessionId() {
  char buf[96];
  snprintf(
    buf,
    sizeof(buf),
    "%s_%lu_%08lx",
    DEVICE_ID,
    (unsigned long)millis(),
    (unsigned long)esp_random()
  );
  return sanitizeRtdbKey(String(buf));
}

String roomBasePath() {
  return String("/rooms/") + sanitizeRtdbKey(String(ROOM_ID));
}

// ---------------- Firebase/Auth/RTDB helpers ----------------
String normalizedFirebaseBaseUrl() {
  String baseUrl = String(FIREBASE_DATABASE_URL);

  while (baseUrl.endsWith("/")) {
    baseUrl.remove(baseUrl.length() - 1);
  }

  if (baseUrl.endsWith(".json")) {
    Serial.println("[RTDB] ERROR: FIREBASE_DATABASE_URL should be the database root URL, not a .json path.");
  }

  return baseUrl;
}

bool signInFirebase() {
#if !USE_AUTH_FOR_RTDB_UPLOAD
  return true;
#else
  if (firebaseIdToken.length() > 0 && millis() < tokenExpiresAtMs) {
    return true;
  }

  Serial.println("[Firebase] Signing in...");

  WiFiClientSecure client;
  client.setInsecure();

  HTTPClient https;

  String url = "https://identitytoolkit.googleapis.com/v1/accounts:signInWithPassword?key=";
  url += FIREBASE_API_KEY;

  if (!https.begin(client, url)) {
    Serial.println("[Firebase] Auth HTTPS begin failed");
    return false;
  }

  https.addHeader("Content-Type", "application/json");
  https.addHeader("Connection", "keep-alive");

  String payload = "{";
  payload += String("\"email\":\"") + jsonEscape(String(FIREBASE_EMAIL)) + "\",";
  payload += String("\"password\":\"") + jsonEscape(String(FIREBASE_PASSWORD)) + "\",";
  payload += "\"returnSecureToken\":true";
  payload += "}";

  int httpCode = https.POST(payload);
  String response = https.getString();

  https.end();

  if (httpCode != 200) {
    Serial.printf("[Firebase] Auth failed. HTTP %d\n", httpCode);
    Serial.println(response);
    return false;
  }

  firebaseIdToken = extractJsonString(response, "idToken");
  String expiresInStr = extractJsonString(response, "expiresIn");

  if (firebaseIdToken.length() == 0) {
    Serial.println("[Firebase] Could not parse idToken");
    return false;
  }

  unsigned long expiresInSec = expiresInStr.toInt();
  if (expiresInSec == 0) expiresInSec = 3600;

  tokenExpiresAtMs = millis() + (expiresInSec - 60) * 1000UL;

  Serial.println("[Firebase] Sign-in OK");
  return true;
#endif
}

String rtdbUrl(const String& rawPath) {
  String path = rawPath;

  if (!path.startsWith("/")) {
    path = "/" + path;
  }

  if (path.endsWith(".json")) {
    path.replace(".json", "");
  }

  String url = normalizedFirebaseBaseUrl() + path + ".json";

  bool hasQuery = false;

#if USE_AUTH_FOR_RTDB_UPLOAD
  if (firebaseIdToken.length() > 0) {
    url += "?auth=";
    url += firebaseIdToken;
    hasQuery = true;
  }
#endif

  // Important: prevents Firebase from echoing the uploaded JSON back.
  // This saves bandwidth and time for Base64 audio chunks.
  url += hasQuery ? "&print=silent" : "?print=silent";

  return url;
}

bool shouldRetryRtdbCode(int httpCode) {
  // Negative values are ESP32/HTTPClient errors. -11 is read timeout.
  if (httpCode < 0) return true;

  // Retry transient server/rate-limit conditions.
  if (httpCode == 408 || httpCode == 429) return true;
  if (httpCode >= 500 && httpCode <= 599) return true;

  return false;
}

bool rtdbSendJson(const char* method, const String& path, const String& jsonPayload, bool printFailureResponse = true) {
  if (!signInFirebase()) {
    return false;
  }

  String url = rtdbUrl(path);

  Serial.print("[RTDB ");
  Serial.print(method);
  Serial.print("] Path: ");
  Serial.println(path);
  Serial.printf("[RTDB %s] Payload bytes: %u\n", method, (unsigned)jsonPayload.length());

  for (uint8_t attempt = 1; attempt <= RTDB_SEND_RETRIES; attempt++) {
    if (WiFi.status() != WL_CONNECTED) {
      Serial.println("[RTDB] Wi-Fi disconnected; reconnecting...");
      WiFi.disconnect();
      WiFi.mode(WIFI_STA);
      WiFi.setSleep(false);
      WiFi.begin(WIFI_SSID, WIFI_PASSWORD);

      uint32_t startMs = millis();
      while (WiFi.status() != WL_CONNECTED && millis() - startMs < 8000) {
        delay(250);
        Serial.print(".");
      }
      Serial.println();

      // Wi-Fi reconnect invalidates any old TLS connection.
      resetRtdbClient();
    }

    WiFiClientSecure& client = getRtdbClient();

    HTTPClient https;
    https.setFollowRedirects(HTTPC_STRICT_FOLLOW_REDIRECTS);
    https.setTimeout(RTDB_HTTP_TIMEOUT_MS);
    https.setReuse(true);

    Serial.printf("[RTDB %s] Attempt %u/%u\n", method, attempt, RTDB_SEND_RETRIES);

    if (!https.begin(client, url)) {
      Serial.print("[RTDB ");
      Serial.print(method);
      Serial.println("] https.begin() failed");
      https.end();
      resetRtdbClient();
      delay(RTDB_RETRY_BASE_DELAY_MS * attempt);
      continue;
    }

    https.addHeader("Content-Type", "application/json");
    https.addHeader("Connection", "keep-alive");

    int httpCode = https.sendRequest(method, (uint8_t*)jsonPayload.c_str(), jsonPayload.length());

    Serial.print("[RTDB ");
    Serial.print(method);
    Serial.printf("] HTTP code: %d\n", httpCode);

    // With print=silent, Firebase returns 204 No Content on success.
    // Without print=silent, typical successful REST writes return 200.
    if (httpCode == 200 || httpCode == 204) {
      https.end();
      return true;
    }

    if (httpCode < 0) {
      Serial.print("[RTDB ");
      Serial.print(method);
      Serial.print("] HTTPClient error: ");
      Serial.println(https.errorToString(httpCode));
    }

    String response = https.getString();

    if (printFailureResponse) {
      Serial.print("[RTDB ");
      Serial.print(method);
      Serial.println("] Response:");
      Serial.println(response);
    }

    bool retry = shouldRetryRtdbCode(httpCode);
    https.end();

    // On failed requests, do not reuse a possibly stale TLS connection.
    resetRtdbClient();

    if (!retry || attempt == RTDB_SEND_RETRIES) {
      return false;
    }

    uint32_t waitMs = RTDB_RETRY_BASE_DELAY_MS * attempt;
    Serial.printf("[RTDB %s] Retrying after %u ms\n", method, (unsigned)waitMs);
    delay(waitMs);
  }

  return false;
}

bool rtdbPutJson(const String& path, const String& jsonPayload) {
  return rtdbSendJson("PUT", path, jsonPayload);
}

bool rtdbPatchJson(const String& path, const String& jsonPayload) {
  return rtdbSendJson("PATCH", path, jsonPayload);
}

// ---------------- Wi-Fi + I2S setup ----------------
void connectWiFi() {
  Serial.printf("[WiFi] Connecting to %s", WIFI_SSID);

  WiFi.mode(WIFI_STA);
  WiFi.setSleep(false);
  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);

  while (WiFi.status() != WL_CONNECTED) {
    delay(300);
    Serial.print(".");
  }

  Serial.println();
  Serial.print("[WiFi] Connected. IP: ");
  Serial.println(WiFi.localIP());
}

void setupMic() {
  Serial.println("[setup] Installing microphone I2S RX driver...");

  i2s_config_t micConfig = {
      .mode = (i2s_mode_t)(I2S_MODE_MASTER | I2S_MODE_RX),
      .sample_rate = SAMPLE_RATE,
      .bits_per_sample = I2S_BITS_PER_SAMPLE_32BIT,
      .channel_format = I2S_MIC_CHANNEL,
      .communication_format = I2S_COMM_FORMAT_I2S,
      .intr_alloc_flags = ESP_INTR_FLAG_LEVEL1,
      .dma_buf_count = 4,
      .dma_buf_len = 128,
      .use_apll = false,
      .tx_desc_auto_clear = false,
      .fixed_mclk = 0
  };

  i2s_pin_config_t micPins = {
      .bck_io_num = I2S_MIC_BCLK,
      .ws_io_num = I2S_MIC_WS,
      .data_out_num = I2S_PIN_NO_CHANGE,
      .data_in_num = I2S_MIC_SD
  };

  esp_err_t err = i2s_driver_install(I2S_MIC_PORT, &micConfig, 0, NULL);
  printErr("mic i2s_driver_install", err);

  err = i2s_set_pin(I2S_MIC_PORT, &micPins);
  printErr("mic i2s_set_pin", err);

  err = i2s_set_clk(I2S_MIC_PORT, SAMPLE_RATE, I2S_BITS_PER_SAMPLE_32BIT, I2S_CHANNEL_MONO);
  printErr("mic i2s_set_clk", err);

  i2s_zero_dma_buffer(I2S_MIC_PORT);
}

// ---------------- Base64 + audio recording ----------------
char* base64Encode(const uint8_t* data, size_t dataLen, size_t* outLen) {
  size_t encodedCapacity = 4 * ((dataLen + 2) / 3) + 1;

  char* encoded = (char*)malloc(encodedCapacity);
  if (!encoded) {
    Serial.printf("[Base64] Failed to allocate encoded buffer. Need=%u FreeHeap=%u MaxAlloc=%u\n",
                  (unsigned)encodedCapacity,
                  (unsigned)ESP.getFreeHeap(),
                  (unsigned)ESP.getMaxAllocHeap());
    return nullptr;
  }

  size_t actualLen = 0;

  int rc = mbedtls_base64_encode(
    (unsigned char*)encoded,
    encodedCapacity,
    &actualLen,
    data,
    dataLen
  );

  if (rc != 0) {
    Serial.printf("[Base64] Encode failed: %d\n", rc);
    free(encoded);
    return nullptr;
  }

  encoded[actualLen] = '\0';
  *outLen = actualLen;
  return encoded;
}

bool buttonReleasedDebounced(uint32_t stableMs = BUTTON_DEBOUNCE_MS) {
  // Button is active-low. A single HIGH read can be contact bounce/noise,
  // so only end the live session if the button remains HIGH for stableMs.
  if (digitalRead(BUTTON_PIN) == LOW) {
    return false;
  }

  uint32_t startMs = millis();
  while ((millis() - startMs) < stableMs) {
    if (digitalRead(BUTTON_PIN) == LOW) {
      return false;
    }
    delay(1);
  }

  return true;
}

size_t recordOneLiveChunk(int16_t* pcmBuffer, size_t targetSamples) {
  size_t writtenSamples = 0;

  // Important:
  // Do NOT check the button inside this loop. Once a 200 ms chunk starts,
  // record the full chunk. Otherwise a single noisy HIGH read on GPIO14 can
  // truncate the chunk, e.g. 118 samples instead of 1600.
  while (writtenSamples < targetSamples) {
    size_t bytesRead = 0;

    esp_err_t err = i2s_read(
      I2S_MIC_PORT,
      micRaw,
      sizeof(micRaw),
      &bytesRead,
      pdMS_TO_TICKS(25)
    );

    if (err != ESP_OK || bytesRead == 0) {
      continue;
    }

    uint16_t samplesRead = bytesRead / sizeof(int32_t);
    if (samplesRead > MIC_READ_SAMPLES) {
      samplesRead = MIC_READ_SAMPLES;
    }

    for (uint16_t i = 0; i < samplesRead && writtenSamples < targetSamples; i++) {
      // Same conversion logic as your ESP-NOW transmitAudioStep().
      int32_t shifted = micRaw[i] >> MIC_SHIFT;

      // Online DC removal / high-pass behavior.
      int32_t centered = shifted - dcEstimate;
      dcEstimate += centered >> 8;

      int32_t amplified = centered * MIC_GAIN;
      amplified = limitAudioSample(amplified);

      pcmBuffer[writtenSamples++] = clampToInt16(amplified);
    }
  }

  return writtenSamples;
}

// ---------------- Live session upload ----------------
String sessionPath(const String& sessionId) {
  return roomBasePath() + "/sessions/" + sessionId;
}

bool startLiveSession(const String& sessionId) {
  String roomPath = roomBasePath();

  String liveJson = "{";
  liveJson += "\"active\":true,";
  liveJson += "\"status\":\"live\",";
  liveJson += String("\"senderDeviceId\":\"") + jsonEscape(String(DEVICE_ID)) + "\",";
  liveJson += String("\"sessionId\":\"") + jsonEscape(sessionId) + "\",";
  liveJson += String("\"sampleRate\":") + String(SAMPLE_RATE) + ",";
  liveJson += "\"format\":\"pcm_s16le\",";
  liveJson += "\"encoding\":\"chunked_pcm_s16le_base64\",";
  liveJson += "\"latestSeq\":-1,";
  liveJson += String("\"startedAtMillis\":") + String(millis());
  liveJson += "}";

  String sessionJson = "{";
  sessionJson += "\"type\":\"live_audio_session\",";
  sessionJson += String("\"deviceId\":\"") + jsonEscape(String(DEVICE_ID)) + "\",";
  sessionJson += "\"active\":true,";
  sessionJson += "\"done\":false,";
  sessionJson += "\"status\":\"live\",";
  sessionJson += "\"format\":\"pcm_s16le\",";
  sessionJson += "\"encoding\":\"chunked_pcm_s16le_base64\",";
  sessionJson += String("\"sampleRate\":") + String(SAMPLE_RATE) + ",";
  sessionJson += "\"channels\":1,";
  sessionJson += "\"bitsPerSample\":16,";
  sessionJson += "\"signed\":true,";
  sessionJson += "\"endianness\":\"little\",";
  sessionJson += String("\"liveChunkMs\":") + String(LIVE_CHUNK_MS) + ",";
  sessionJson += String("\"liveChunkSamples\":") + String(LIVE_CHUNK_SAMPLES) + ",";
  sessionJson += String("\"micShift\":") + String(MIC_SHIFT) + ",";
  sessionJson += String("\"micGain\":") + String(MIC_GAIN) + ",";
  sessionJson += String("\"noiseGateEnabled\":") + String(ENABLE_NOISE_GATE ? "true" : "false") + ",";
  sessionJson += String("\"noiseGateLevel\":") + String(NOISE_GATE_LEVEL) + ",";
  sessionJson += String("\"txLimiterEnabled\":") + String(ENABLE_TX_LIMITER ? "true" : "false") + ",";
  sessionJson += String("\"txLimitAbs\":") + String(TX_LIMIT_ABS) + ",";
  sessionJson += "\"latestSeq\":-1,";
  sessionJson += "\"chunkCount\":0,";
  sessionJson += "\"sampleCount\":0,";
  sessionJson += String("\"startedAtMillis\":") + String(millis());
  sessionJson += "}";

  // One PATCH creates/updates both the public live pointer and the session metadata.
  String patch = "{";
  patch += String("\"live\":") + liveJson + ",";
  patch += String("\"sessions/") + jsonEscape(sessionId) + "\":" + sessionJson;
  patch += "}";

  Serial.print("[Live] Publishing RTDB session metadata for sessionId: ");
  Serial.println(sessionId);

  return rtdbPatchJson(roomPath, patch);
}

bool uploadLiveChunk(const String& sessionId,
                     uint32_t seq,
                     const int16_t* pcmBuffer,
                     size_t sampleCount,
                     size_t totalSamplesAfterThisChunk) {
  (void)totalSamplesAfterThisChunk;

  size_t rawBytes = sampleCount * sizeof(int16_t);
  size_t base64Len = 0;

  char* base64Chunk = base64Encode((const uint8_t*)pcmBuffer, rawBytes, &base64Len);
  if (!base64Chunk) {
    return false;
  }

  String chunkJson;
  chunkJson.reserve(base64Len + 220);
  chunkJson += "{";
  chunkJson += String("\"seq\":") + String(seq) + ",";
  chunkJson += String("\"sampleCount\":") + String(sampleCount) + ",";
  chunkJson += String("\"rawBytes\":") + String(rawBytes) + ",";
  chunkJson += String("\"durationMs\":") + String((sampleCount * 1000UL) / SAMPLE_RATE) + ",";
  chunkJson += String("\"createdAtMillis\":") + String(millis()) + ",";
  chunkJson += "\"data\":\"";
  chunkJson += base64Chunk;
  chunkJson += "\"";
  chunkJson += "}";

  free(base64Chunk);

  String seqName = chunkName(seq);
  String chunkPath = sessionPath(sessionId) + "/chunks/" + seqName;

  Serial.printf("[Live] PUT chunk seq=%lu samples=%u rawBytes=%u base64=%u\n",
                (unsigned long)seq,
                (unsigned)sampleCount,
                (unsigned)rawBytes,
                (unsigned)base64Len);

  // Important reliability change:
  // Upload the chunk directly to its own RTDB path with PUT.
  // The previous version used one large PATCH to /rooms/<roomId> that also
  // updated live/latestSeq and counters. With 500 ms chunks this produced
  // ~11 KB request bodies and caused ESP32 HTTPClient "send payload failed".
  // This PUT keeps each request body small and avoids multi-location PATCH
  // overhead. Counters are finalized once at the end of the session.
  return rtdbPutJson(chunkPath, chunkJson);
}

bool finishLiveSession(const String& sessionId,
                       uint32_t uploadedChunks,
                       uint32_t capturedChunks,
                       uint32_t droppedChunks,
                       uint32_t lastUploadedSeq,
                       size_t uploadedSamples,
                       bool error) {
  String roomPath = roomBasePath();
  uint32_t durationMs = (uint32_t)((uploadedSamples * 1000ULL) / SAMPLE_RATE);

  String status = error ? "error" : "complete";

  String patch = "{";
  patch += String("\"sessions/") + jsonEscape(sessionId) + "/active\":false,";
  patch += String("\"sessions/") + jsonEscape(sessionId) + "/done\":true,";
  patch += String("\"sessions/") + jsonEscape(sessionId) + "/status\":\"" + status + "\",";
  patch += String("\"sessions/") + jsonEscape(sessionId) + "/chunkCount\":" + String(uploadedChunks) + ",";
  patch += String("\"sessions/") + jsonEscape(sessionId) + "/capturedChunks\":" + String(capturedChunks) + ",";
  patch += String("\"sessions/") + jsonEscape(sessionId) + "/droppedChunks\":" + String(droppedChunks) + ",";
  patch += String("\"sessions/") + jsonEscape(sessionId) + "/latestSeq\":" + String(uploadedChunks == 0 ? -1 : (int32_t)lastUploadedSeq) + ",";
  patch += String("\"sessions/") + jsonEscape(sessionId) + "/sampleCount\":" + String(uploadedSamples) + ",";
  patch += String("\"sessions/") + jsonEscape(sessionId) + "/durationMs\":" + String(durationMs) + ",";
  patch += String("\"sessions/") + jsonEscape(sessionId) + "/endedAtMillis\":" + String(millis()) + ",";
  patch += "\"live/active\":false,";
  patch += String("\"live/status\":\"") + status + "\",";
  patch += String("\"live/doneSessionId\":\"") + jsonEscape(sessionId) + "\",";
  patch += String("\"live/uploadedChunks\":") + String(uploadedChunks) + ",";
  patch += String("\"live/droppedChunks\":") + String(droppedChunks) + ",";
  patch += String("\"live/endedAtMillis\":") + String(millis());
  patch += "}";

  bool ok = rtdbPatchJson(roomPath, patch);

  Serial.printf("[Live] Finished session=%s uploaded=%lu captured=%lu dropped=%lu samples=%u durationMs=%u status=%s\n",
                sessionId.c_str(),
                (unsigned long)uploadedChunks,
                (unsigned long)capturedChunks,
                (unsigned long)droppedChunks,
                (unsigned)uploadedSamples,
                (unsigned)durationMs,
                status.c_str());

  Serial.println("=========== LIVE SESSION END ===========");
  Serial.println();

  return ok;
}

void liveUploadTask(void* param) {
  LiveUploadContext* ctx = (LiveUploadContext*)param;

  Serial.println("[UploadTask] Started. Creating RTDB live session while recorder keeps capturing.");

  if (!startLiveSession(ctx->sessionId)) {
    ctx->uploadError = true;
    ctx->recorderDone = true;
    Serial.println("[UploadTask] Failed to create RTDB live session");
    finishLiveSession(
      ctx->sessionId,
      ctx->uploadedChunks,
      ctx->capturedChunks,
      ctx->droppedChunks,
      ctx->lastUploadedSeq,
      ctx->uploadedSamples,
      true
    );
    xSemaphoreGive(ctx->doneSemaphore);
    vTaskDelete(NULL);
  }

  while (true) {
    LiveAudioChunk* chunk = nullptr;

    if (xQueueReceive(ctx->filledQueue, &chunk, pdMS_TO_TICKS(100)) == pdTRUE) {
      size_t totalAfter = ctx->uploadedSamples + chunk->sampleCount;

      bool ok = uploadLiveChunk(
        ctx->sessionId,
        chunk->seq,
        chunk->samples,
        chunk->sampleCount,
        totalAfter
      );

      if (ok) {
        ctx->uploadedChunks++;
        ctx->lastUploadedSeq = chunk->seq;
        ctx->uploadedSamples = totalAfter;
      } else {
        ctx->droppedChunks++;
        Serial.printf("[UploadTask] Upload failed at seq=%lu; dropping this chunk and continuing\n",
                      (unsigned long)chunk->seq);

      #if ABORT_ON_CHUNK_UPLOAD_FAIL
        ctx->uploadError = true;
      #endif
      }

      // Return the buffer to the recorder whether upload succeeded or failed.
      xQueueSend(ctx->freeQueue, &chunk, portMAX_DELAY);

      if (ctx->uploadError) {
        break;
      }
    } else {
      if (ctx->recorderDone && uxQueueMessagesWaiting(ctx->filledQueue) == 0) {
        break;
      }
    }
  }

  finishLiveSession(
    ctx->sessionId,
    ctx->uploadedChunks,
    ctx->capturedChunks,
    ctx->droppedChunks,
    ctx->lastUploadedSeq,
    ctx->uploadedSamples,
    ctx->uploadError
  );

  Serial.println("[UploadTask] Done");
  xSemaphoreGive(ctx->doneSemaphore);
  vTaskDelete(NULL);
}

void runLiveTalkSession() {
  String sessionId = makeSessionId();

  liveUploadCtx.sessionId = sessionId;
  liveUploadCtx.freeQueue = nullptr;
  liveUploadCtx.filledQueue = nullptr;
  liveUploadCtx.doneSemaphore = nullptr;
  liveUploadCtx.recorderDone = false;
  liveUploadCtx.uploadError = false;
  liveUploadCtx.capturedChunks = 0;
  liveUploadCtx.droppedChunks = 0;
  liveUploadCtx.uploadedChunks = 0;
  liveUploadCtx.lastUploadedSeq = 0;
  liveUploadCtx.uploadedSamples = 0;

  liveUploadCtx.freeQueue = xQueueCreate(LIVE_UPLOAD_QUEUE_LEN, sizeof(LiveAudioChunk*));
  liveUploadCtx.filledQueue = xQueueCreate(LIVE_UPLOAD_QUEUE_LEN, sizeof(LiveAudioChunk*));
  liveUploadCtx.doneSemaphore = xSemaphoreCreateBinary();

  if (!liveUploadCtx.freeQueue || !liveUploadCtx.filledQueue || !liveUploadCtx.doneSemaphore) {
    Serial.println("[Live] Failed to create queues/semaphore. Reduce LIVE_UPLOAD_QUEUE_LEN.");
    if (liveUploadCtx.freeQueue) vQueueDelete(liveUploadCtx.freeQueue);
    if (liveUploadCtx.filledQueue) vQueueDelete(liveUploadCtx.filledQueue);
    if (liveUploadCtx.doneSemaphore) vSemaphoreDelete(liveUploadCtx.doneSemaphore);
    return;
  }

  for (uint32_t i = 0; i < LIVE_UPLOAD_QUEUE_LEN; i++) {
    LiveAudioChunk* p = &liveChunkPool[i];
    xQueueSend(liveUploadCtx.freeQueue, &p, 0);
  }

  digitalWrite(TX_LED_PIN, HIGH);
  i2s_zero_dma_buffer(I2S_MIC_PORT);
  dcEstimate = 0;

  Serial.println();
  Serial.println("========== LIVE SESSION START ==========");
  Serial.print("[Live] sessionId: ");
  Serial.println(sessionId);

  // Important latency fix:
  // Do not perform the initial RTDB start PATCH here. HTTPS can take longer than
  // a short button hold. The upload task creates the RTDB session in parallel
  // while this recorder loop immediately starts capturing microphone chunks.
  BaseType_t taskOk = xTaskCreatePinnedToCore(
    liveUploadTask,
    "firebase_upload",
    UPLOAD_TASK_STACK_BYTES,
    &liveUploadCtx,
    1,
    nullptr,
    0
  );

  if (taskOk != pdPASS) {
    Serial.println("[Live] Failed to start upload task");
    digitalWrite(TX_LED_PIN, LOW);
    vQueueDelete(liveUploadCtx.freeQueue);
    vQueueDelete(liveUploadCtx.filledQueue);
    vSemaphoreDelete(liveUploadCtx.doneSemaphore);
    return;
  }

  uint32_t seq = 0;

  Serial.println("[Recorder] Started immediately. Initial RTDB PATCH is running in upload task.");
  Serial.printf("[Recorder] Initial button raw=%d pressed=%s\n",
                (int)digitalRead(BUTTON_PIN),
                digitalRead(BUTTON_PIN) == LOW ? "yes" : "no");

  while (!buttonReleasedDebounced() && seq < MAX_LIVE_CHUNKS && !liveUploadCtx.uploadError) {
    LiveAudioChunk* chunk = nullptr;

    if (xQueueReceive(liveUploadCtx.freeQueue, &chunk, 0) != pdTRUE) {
    #if REALTIME_DROP_OLD_BACKLOG
      // Real-time behavior:
      // If uploader is behind, discard the oldest queued chunk and reuse its buffer
      // for the newest audio. This keeps latency lower.
      LiveAudioChunk* oldChunk = nullptr;

      if (xQueueReceive(liveUploadCtx.filledQueue, &oldChunk, 0) == pdTRUE) {
        liveUploadCtx.droppedChunks++;

        Serial.printf("[Recorder] Queue full; dropping old queued seq=%lu to keep live audio current\n",
                      (unsigned long)oldChunk->seq);

        chunk = oldChunk;
      } else {
        // Extremely unlikely: no free buffer and no filled buffer.
        size_t droppedSamples = recordOneLiveChunk(livePcmChunk, LIVE_CHUNK_SAMPLES);
        if (droppedSamples > 0) {
          liveUploadCtx.droppedChunks++;
          liveUploadCtx.capturedChunks++;
          Serial.printf("[Recorder] Queue full; dropped current seq=%lu samples=%u\n",
                        (unsigned long)seq,
                        (unsigned)droppedSamples);
          seq++;
        }
        continue;
      }
      #else
        // Old behavior: drop the newest audio.
        size_t droppedSamples = recordOneLiveChunk(livePcmChunk, LIVE_CHUNK_SAMPLES);
        if (droppedSamples > 0) {
          liveUploadCtx.droppedChunks++;
          liveUploadCtx.capturedChunks++;
          Serial.printf("[Recorder] Upload queue full; dropped chunk seq=%lu samples=%u\n",
                        (unsigned long)seq,
                        (unsigned)droppedSamples);
          seq++;
        }
        continue;
    #endif
    }

    chunk->seq = seq;
    chunk->capturedAtMillis = millis();
    chunk->sampleCount = recordOneLiveChunk(chunk->samples, LIVE_CHUNK_SAMPLES);

    if (chunk->sampleCount == 0) {
      xQueueSend(liveUploadCtx.freeQueue, &chunk, portMAX_DELAY);
      continue;
    }

    liveUploadCtx.capturedChunks++;

    if (xQueueSend(liveUploadCtx.filledQueue, &chunk, 0) != pdTRUE) {
      liveUploadCtx.droppedChunks++;
      Serial.printf("[Recorder] Filled queue unexpectedly full; dropped seq=%lu\n",
                    (unsigned long)seq);
      xQueueSend(liveUploadCtx.freeQueue, &chunk, portMAX_DELAY);
    } else {
      Serial.printf("[Recorder] Queued chunk seq=%lu samples=%u freeBuffers=%u queued=%u\n",
                    (unsigned long)seq,
                    (unsigned)chunk->sampleCount,
                    (unsigned)uxQueueMessagesWaiting(liveUploadCtx.freeQueue),
                    (unsigned)uxQueueMessagesWaiting(liveUploadCtx.filledQueue));
    }

    seq++;
  }

  if (seq >= MAX_LIVE_CHUNKS) {
    Serial.println("[Live] Max live chunks reached; ending session");
  }

  liveUploadCtx.recorderDone = true;

  Serial.printf("[Recorder] Done. captured=%lu dropped=%lu queuedLeft=%u. Waiting for uploader to finish...\n",
                (unsigned long)liveUploadCtx.capturedChunks,
                (unsigned long)liveUploadCtx.droppedChunks,
                (unsigned)uxQueueMessagesWaiting(liveUploadCtx.filledQueue));

  // Keep LED on until queued chunks are uploaded and the session is marked complete.
  xSemaphoreTake(liveUploadCtx.doneSemaphore, portMAX_DELAY);

  digitalWrite(TX_LED_PIN, LOW);

  vQueueDelete(liveUploadCtx.freeQueue);
  vQueueDelete(liveUploadCtx.filledQueue);
  vSemaphoreDelete(liveUploadCtx.doneSemaphore);
}

// ---------------- Arduino setup/loop ----------------
void setup() {
  Serial.begin(115200);
  delay(1000);

  pinMode(BUTTON_PIN, INPUT_PULLUP);
  pinMode(TX_LED_PIN, OUTPUT);
  digitalWrite(TX_LED_PIN, LOW);

  Serial.println();
  Serial.println("=== ESP32 LIVE FIREBASE WALKIE-TALKIE SENDER ===");
  Serial.printf("[INFO] Sample rate: %d Hz\n", SAMPLE_RATE);
  Serial.printf("[INFO] Live chunk: %d ms, %d samples, %u raw bytes\n",
                LIVE_CHUNK_MS,
                LIVE_CHUNK_SAMPLES,
                (unsigned)LIVE_CHUNK_BYTES);
  Serial.printf("[INFO] Button GPIO: %d, LED GPIO: %d\n",
                (int)BUTTON_PIN,
                (int)TX_LED_PIN);

  connectWiFi();
  setupMic();

  Serial.println("[READY] Hold button to stream live chunks. Release to end session.");
}

void loop() {
  static bool lastRawPressed = false;
  static bool stablePressed = false;
  static uint32_t lastButtonChangeMs = 0;

  bool rawPressed = digitalRead(BUTTON_PIN) == LOW;
  uint32_t now = millis();

  if (rawPressed != lastRawPressed) {
    lastRawPressed = rawPressed;
    lastButtonChangeMs = now;
  }

  if ((now - lastButtonChangeMs) >= BUTTON_DEBOUNCE_MS && rawPressed != stablePressed) {
    stablePressed = rawPressed;

    if (stablePressed) {
      Serial.println("[Button] Pressed. Starting live stream...");
      runLiveTalkSession();

      while (digitalRead(BUTTON_PIN) == LOW) {
        delay(10);
      }

      delay(BUTTON_DEBOUNCE_MS);
      stablePressed = false;
      lastRawPressed = false;
      lastButtonChangeMs = millis();

      Serial.println("[READY] Hold button to stream live chunks. Release to end session.");
    }
  }

  delay(5);
}