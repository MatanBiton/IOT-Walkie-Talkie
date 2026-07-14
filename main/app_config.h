#pragma once

#include <Arduino.h>

namespace AppConfig {

constexpr uint8_t DEFAULT_CHANNEL = 1;
constexpr const char* ROOM_ID = "room1";
constexpr const char* DEVICE_ID = "esp32-02";

// Logical project user id. Compile/upload the same code to each ESP after
// changing only this value and DEVICE_ID, for example: "01" ... "05".
constexpr const char* USER_ID = "02";

}  // namespace AppConfig

namespace WifiConfig {

// Fill locally before uploading. Do not commit real Wi-Fi credentials.
constexpr const char* SSID = "ICST";
constexpr const char* PASSWORD = "arduino123";

constexpr unsigned long CONNECT_TIMEOUT_MS = 20000;
constexpr unsigned long RECONNECT_INTERVAL_MS = 5000;

}  // namespace WifiConfig

namespace FirebaseConfig {

// Keep the URL without a trailing slash.
// This patch assumes permissive RTDB rules for the demo, so no Firebase Auth,
// API key, email, or password are used.
constexpr const char* DATABASE_URL =
    "https://walki-talkie-37ed3-default-rtdb.europe-west1.firebasedatabase.app/";

}  // namespace FirebaseConfig


namespace AvailabilityConfig {

constexpr bool ENABLED = false;
constexpr uint8_t USER_COUNT = 5;

// Background availability period. Both transports tolerate missed periods so
// a delayed, serialized RTDB request does not make the GUI flicker.
constexpr unsigned long PERIOD_TIME_MS = 5000;
constexpr uint8_t VOIP_OFFLINE_AFTER_MISSED_PERIODS = 3;
constexpr uint8_t P2P_OFFLINE_AFTER_MISSED_PERIODS = 3;

constexpr const char* RTDB_USERS_PATH = "/statistics/users";

// Local time metadata written with every RTDB heartbeat. POSIX TZ format is used
// by configTzTime(); the readable name is stored for display/debugging.
constexpr const char* TIME_ZONE_NAME = "Asia/Jerusalem";
constexpr const char* TIME_ZONE_POSIX = "IST-2IDT,M3.4.4/26,M10.5.0";

constexpr const char* NTP_SERVER_1 = "pool.ntp.org";
constexpr const char* NTP_SERVER_2 = "time.nist.gov";
constexpr unsigned long TIME_SYNC_CHECK_MS = 30000;
constexpr uint32_t CLOCK_VALID_EPOCH_SECONDS = 1700000000UL;

// ESP-NOW physical channel policy.
//
// Important ESP32 limitation: while the ESP is connected to a Wi-Fi router,
// ESP-NOW must use the STA "home" channel, meaning the router/AP channel.
// A separate physical stats channel causes send failures such as:
//   ESPNOW: Peer channel is not equal to the home channel
// and can also break HTTPS/RTDB by moving the radio away from the AP.
//
// Therefore this project keeps ESP-NOW presence on the current Wi-Fi channel
// and separates traffic by packet type + synchronized time windows, not by
// physical radio channel. Value 0 means "current Wi-Fi channel".
constexpr uint8_t P2P_ESPNOW_CURRENT_WIFI_CHANNEL = 0;

// Logical identifiers kept for future local P2P talk implementation. They are
// not physical Wi-Fi channels while RTDB/Wi-Fi is active.
constexpr uint8_t P2P_TALK_LOGICAL_CHANNEL = 1;
constexpr uint8_t P2P_STATS_LOGICAL_CHANNEL = 250;

// P2P stats windows are based on epoch time, not millis() since boot:
//   inWindow = (epochMs % PERIOD_TIME_MS) < P2P_STATS_SYNC_WINDOW_MS
// Therefore ESPs that boot at different times still hop to the stats channel
// together after NTP time is available.
constexpr uint16_t P2P_STATS_SYNC_WINDOW_MS = 300;
constexpr uint8_t P2P_STATS_BROADCAST_REPEATS = 3;
constexpr uint16_t P2P_STATS_BROADCAST_GAP_MS = 35;
constexpr uint16_t P2P_STATS_USER_TX_OFFSET_STEP_MS = 35;

constexpr uint16_t RTDB_HTTP_TIMEOUT_MS = 5000;
constexpr int32_t RTDB_CONNECT_TIMEOUT_MS = 3000;
constexpr uint8_t RTDB_REQUEST_MAX_ATTEMPTS = 2;
constexpr unsigned long RTDB_RETRY_DELAY_MS = 250;

constexpr uint32_t TASK_STACK_BYTES = 12288;
constexpr uint8_t TASK_PRIORITY = 1;
constexpr uint8_t TASK_CORE = 0;
constexpr unsigned long TASK_SHORT_YIELD_MS = 20;
constexpr unsigned long TASK_LOOP_DELAY_MS = 20;
constexpr unsigned long GUI_REFRESH_MS = 500;

constexpr bool LOG_AVAILABILITY = false;

}  // namespace AvailabilityConfig

namespace RtdbHttpConfig {

// HTTP requests are blocking on the ESP32. These limits prevent one bad RTDB
// PUT from freezing the push-to-talk loop for minutes.
constexpr uint16_t HTTP_TIMEOUT_MS = 15000;
constexpr int32_t CONNECT_TIMEOUT_MS = 8000;
constexpr uint8_t CONTROL_REQUEST_MAX_ATTEMPTS = 3;
constexpr unsigned long CONTROL_RETRY_DELAY_MS = 350;

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

// Ten fixed blocks cover two seconds of 200 ms audio without placing full PCM
// arrays inside a FreeRTOS queue. One chunk is sent per persistent HTTP PATCH.
constexpr uint8_t TX_QUEUE_LENGTH = 10;
constexpr uint8_t UPLOAD_BATCH_MAX_CHUNKS = 1;
constexpr uint32_t BATCH_COLLECTION_WINDOW_MS = 300;
constexpr uint8_t UPLOAD_MAX_ATTEMPTS = 2;
constexpr unsigned long UPLOAD_RETRY_DELAY_MS = 250;
// Persistent audio PATCH response budget. Audio uses one TLS connection per
// PTT session instead of reconnecting for every 200 ms chunk.
constexpr uint32_t UPLOAD_RESPONSE_TIMEOUT_MS = 5000;
constexpr uint32_t UPLOAD_TASK_STACK_BYTES = 16384;
constexpr uint8_t UPLOAD_TASK_PRIORITY = 1;
constexpr uint8_t UPLOAD_TASK_CORE = 1;

}  // namespace RtdbUploadConfig


namespace RtdbRequestConfig {

constexpr uint8_t HIGH_QUEUE_LENGTH = 6;
constexpr uint8_t LOW_QUEUE_LENGTH = 2;
constexpr uint8_t CONTROL_SLOT_COUNT = 4;
// Short REST operations use bounded connect and response budgets. Callers wait
// longer than the configured retry sequence without owning request memory.
constexpr uint16_t SHORT_HTTP_TIMEOUT_MS = 3000;
constexpr int32_t SHORT_CONNECT_TIMEOUT_MS = 3000;
constexpr uint32_t TASK_STACK_BYTES = 20480;
constexpr uint8_t TASK_PRIORITY = 2;
constexpr uint8_t TASK_CORE = 0;
constexpr uint32_t TASK_IDLE_DELAY_MS = 10;
constexpr uint32_t SYNC_OPERATION_TIMEOUT_MS = 60000;

}  // namespace RtdbRequestConfig

namespace PlaybackConfig {

constexpr uint8_t BLOCK_COUNT = 5;
constexpr uint8_t QUEUE_LENGTH = 4;
constexpr uint32_t TASK_STACK_BYTES = 6144;
constexpr uint8_t TASK_PRIORITY = 1;
constexpr uint8_t TASK_CORE = 1;

}  // namespace PlaybackConfig

namespace CommunicationConfig {

constexpr uint32_t TASK_STACK_BYTES = 8192;
constexpr uint8_t TASK_PRIORITY = 2;
constexpr uint8_t TASK_CORE = 1;
constexpr uint32_t LOOP_DELAY_MS = 10;
constexpr uint32_t PLAYBACK_STOP_TIMEOUT_MS = 1000;
// Guarantee one microphone block for quick PTT presses while SESSION_START is
// still completing. Normal held transmissions are unaffected.
constexpr uint32_t INITIAL_CAPTURE_GRACE_MS = 750;
constexpr uint32_t UPLOAD_DRAIN_TIMEOUT_MS = 30000;
constexpr uint32_t REMOTE_TALKER_IDLE_TIMEOUT_MS = 2500;

}  // namespace CommunicationConfig

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
