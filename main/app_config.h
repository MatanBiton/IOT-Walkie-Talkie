#pragma once

#include <Arduino.h>

namespace AppConfig {

// Compile/upload the same code twice, changing only this value:
// - Talker:   reads microphone chunks and uploads them to RTDB while MAIN_BUTTON is held.
// - Listener: continuously listens to the RTDB chunk buffer and plays new chunks.
enum class DeviceRole : uint8_t {
  Talker,
  Listener,
};

constexpr DeviceRole ROLE = DeviceRole::Talker;

constexpr uint8_t DEFAULT_CHANNEL = 1;
constexpr const char* ROOM_ID = "room1";
constexpr const char* DEVICE_ID = "esp32-Talker-01";

}  // namespace AppConfig

namespace WifiConfig {

// Fill locally before uploading. Do not commit real Wi-Fi credentials.
constexpr const char* SSID = "GalaxyA31948B";
constexpr const char* PASSWORD = "enxu9794";

constexpr unsigned long CONNECT_TIMEOUT_MS = 20000;
constexpr unsigned long RECONNECT_INTERVAL_MS = 5000;

}  // namespace WifiConfig

namespace FirebaseConfig {

// Keep the URL without a trailing slash.
// This patch assumes permissive RTDB rules for the demo, so no Firebase Auth,
// API key, email, or password are used.
constexpr const char* DATABASE_URL =
    "https://walki-talkie-37ed3-default-rtdb.europe-west1.firebasedatabase.app";

}  // namespace FirebaseConfig


namespace RtdbHttpConfig {

// HTTP requests are blocking on the ESP32. These limits prevent one bad RTDB
// PUT from freezing the push-to-talk loop for minutes.
constexpr uint16_t HTTP_TIMEOUT_MS = 15000;
constexpr int32_t CONNECT_TIMEOUT_MS = 8000;

// Keep enabled while debugging RTDB audio. It prints one clear success/failure
// line per uploaded audio packet and request-level timing/status details.
constexpr bool LOG_HTTP_REQUESTS = true;
constexpr bool LOG_EVERY_AUDIO_PACKET = true;

// Listener-side stream diagnostics. Keep this enabled until packets are heard
// reliably; it reports connection, SSE events, parsed paths, decode decisions,
// and stream reconnects.
constexpr bool LOG_STREAM_EVENTS = true;
constexpr uint16_t STREAM_HTTP_TIMEOUT_MS = 65000;
constexpr int32_t STREAM_CONNECT_TIMEOUT_MS = 8000;
constexpr unsigned long STREAM_RECONNECT_INTERVAL_MS = 3000;

}  // namespace RtdbHttpConfig


namespace RtdbUploadConfig {

// Upload runs in a separate FreeRTOS task so recording does not wait for HTTPS.
// Queue length 24 covers about 4.8 seconds of 200 ms chunks even if the network
// temporarily stalls. Keep an eye on free heap in the startup log before raising it.
constexpr uint8_t TX_QUEUE_LENGTH = 24;
constexpr uint8_t UPLOAD_BATCH_MAX_CHUNKS = 6;
constexpr uint8_t UPLOAD_MAX_ATTEMPTS = 2;
constexpr unsigned long UPLOAD_RETRY_DELAY_MS = 250;
constexpr uint32_t UPLOAD_TASK_STACK_BYTES = 16384;
constexpr uint8_t UPLOAD_TASK_PRIORITY = 1;
constexpr uint8_t UPLOAD_TASK_CORE = 1;

}  // namespace RtdbUploadConfig

namespace AudioConfig {

constexpr uint32_t SAMPLE_RATE = 8000;
constexpr uint16_t CHUNK_MS = 200;
constexpr size_t CHUNK_SAMPLES = (SAMPLE_RATE * CHUNK_MS) / 1000;
constexpr size_t PCM_BYTES = CHUNK_SAMPLES * sizeof(int16_t);
constexpr size_t BASE64_BUFFER_BYTES = ((PCM_BYTES + 2) / 3) * 4 + 1;

constexpr int I2S_DMA_BUFFER_COUNT = 4;
constexpr int I2S_DMA_BUFFER_LEN = 256;
constexpr size_t SPEAKER_WRITE_FRAMES = 256;

}  // namespace AudioConfig

namespace RtdbBufferConfig {

// The talker writes chunks as /chunks/00000000, /chunks/00000001, ...
// A new press creates a new session and clears the previous session buffer.
constexpr uint32_t MAX_CHUNKS_PER_SESSION = 1200;  // 4 minutes at 200 ms/chunk.

// Keep this false for normal use, so the listener does not replay stale chunks
// already present in RTDB when it first connects. It still plays all new chunks
// that arrive after the stream opens.
constexpr bool PLAY_EXISTING_CHUNKS_ON_CONNECT = false;

}  // namespace RtdbBufferConfig
