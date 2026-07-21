#pragma once

#include <Arduino.h>

namespace AppConfig {

constexpr uint8_t DEFAULT_CHANNEL = 1;
constexpr const char* ROOM_ID = "room1";
constexpr const char* DEVICE_ID = "esp32-01";

// Logical project user id. Compile/upload the same code to each ESP after
// changing only this value and DEVICE_ID, for example: "01" ... "05".
constexpr const char* USER_ID = "01";

}  // namespace AppConfig

namespace StatisticsConfig {

// Persistent statistics are written to the ESP32 NVS partition. The live
// network-diagnostics page (SSID, RSSI, P2P TX reject, and P2P RX loss) remains
// current-boot only.
constexpr bool PERSISTENCE_ENABLED = true;
constexpr char NVS_NAMESPACE[] = "run_stats";
constexpr uint8_t SCHEMA_VERSION = 1;

// Dirty statistics are saved at most once per this interval. Leaving or
// switching away from a joined channel requests an immediate save on the main
// loop, without performing flash I/O inside the communication/audio tasks.
constexpr uint32_t SAVE_INTERVAL_MS = 60UL * 1000UL;
constexpr uint32_t SAVE_FAILURE_RETRY_MS = 5000;
constexpr bool LOG_PERSISTENCE = true;

static_assert(sizeof(NVS_NAMESPACE) - 1 <= 15,
              "NVS namespace names are limited to 15 characters");
static_assert(SAVE_INTERVAL_MS >= 1000,
              "Statistics save interval should be at least one second");

}  // namespace StatisticsConfig

namespace WifiConfig {

// Fill locally before uploading. Do not commit real Wi-Fi credentials.
constexpr const char* SSID = "GalaxyA31948B";
constexpr const char* PASSWORD = "enxu9794";
constexpr unsigned int WIFI_CHANNEL = 1;

constexpr unsigned long CONNECT_TIMEOUT_MS = 20000;
constexpr unsigned long RECONNECT_INTERVAL_MS = 5000;

}  // namespace WifiConfig

namespace RuntimeSettingsConfig {

// Runtime-only defaults for the second OLED settings page. Disabling Wi-Fi
// stops infrastructure connection/reconnection while leaving the STA radio
// active for ESP-NOW. Automatic P2P downgrade controls locally initiated
// fallback after failed VoIP uploads; explicit Wi-Fi disable and peer switch
// coordination can still select P2P.
constexpr bool WIFI_ENABLED_DEFAULT = true;
constexpr bool AUTOMATIC_P2P_DOWNGRADE_DEFAULT = true;

}  // namespace RuntimeSettingsConfig

namespace FirebaseConfig {

// Keep the URL without a trailing slash.
// This patch assumes permissive RTDB rules for the demo, so no Firebase Auth,
// API key, email, or password are used.
constexpr const char* DATABASE_URL =
    "https://walki-talkie-37ed3-default-rtdb.europe-west1.firebasedatabase.app";

}  // namespace FirebaseConfig



namespace FirebaseClientConfig {

// The project deliberately compiles only RTDB support and uses NoAuth.
// FIREBASE_ASYNC_QUEUE_LIMIT is set to one in rtdb_request_service.cpp so the
// library never retains multiple 4.4 KB audio payloads.
constexpr uint32_t INITIALIZE_TIMEOUT_MS = 5000;
constexpr uint32_t STREAM_STOP_TIMEOUT_MS = 4500;
constexpr uint32_t STREAM_START_TIMEOUT_MS = 2500;
constexpr uint32_t SYNC_SEND_TIMEOUT_SECONDS = 5;
constexpr uint32_t SYNC_READ_TIMEOUT_SECONDS = 5;
constexpr uint32_t SESSION_TIMEOUT_SECONDS = 180;
constexpr unsigned long TLS_HANDSHAKE_TIMEOUT_SECONDS = 4;

}  // namespace FirebaseClientConfig

namespace AvailabilityConfig {

constexpr bool ENABLED = true;
constexpr uint8_t USER_COUNT = 5;
constexpr uint8_t SCHEMA_VERSION = 1;

// Compact event-driven presence tree. Firebase creates this path on the first
// successful write; no periodic full-tree polling is performed.
constexpr const char* RTDB_USERS_PATH = "/presence/users";
constexpr size_t LEASE_PAYLOAD_BUFFER_BYTES = 256;

// V:ON means a server-timestamped lease is no older than this threshold.
// Abrupt power loss can therefore remain visible for at most this interval.
constexpr uint32_t VOIP_LEASE_FRESH_MS = 5UL * 60UL * 1000UL;
constexpr uint32_t LEASE_MAINTENANCE_INTERVAL_MS = 4UL * 60UL * 1000UL;
constexpr uint32_t LEASE_REQUEST_VALID_MS = 15000;
constexpr uint32_t USERS_READ_REQUEST_VALID_MS = 15000;
// Failed background lease writes are retried only after this quiet interval.
// Manual View Users refreshes do not wait for this backoff.
constexpr uint32_t LEASE_FAILURE_RETRY_MS = 30000;

// Optional stronger presence. When enabled, an idle VoIP listener briefly
// pauses SSE at the maintenance interval, writes one lease, then reconnects.
// Disabled by default because the bounded pause is a real listening gap.
constexpr bool ENABLE_SSE_MAINTENANCE_LEASE = false;
constexpr uint32_t SSE_MAINTENANCE_OPERATION_TIMEOUT_MS = 12000;

// On-demand ESP-NOW discovery. No NTP synchronization and no periodic beacon
// are required. A matching reply proves round-trip reachability during a scan.
constexpr uint16_t P2P_SCAN_WINDOW_MS = 700;
constexpr uint8_t P2P_DISCOVER_REPEATS = 2;
constexpr uint16_t P2P_DISCOVER_GAP_MS = 45;
constexpr uint16_t P2P_REPLY_USER_OFFSET_MS = 35;
constexpr uint16_t P2P_REPLY_RANDOM_JITTER_MS = 20;
constexpr uint16_t P2P_REPLY_DEFER_STEP_MS = 20;
constexpr uint16_t P2P_RESULT_STALE_MS = 30000;

// The main loop already owns NTP maintenance; availability only consumes the
// synchronized clock when classifying server timestamps.
constexpr const char* TIME_ZONE_NAME = "Asia/Jerusalem";
constexpr const char* TIME_ZONE_POSIX = "IST-2IDT,M3.4.4/26,M10.5.0";
constexpr const char* NTP_SERVER_1 = "pool.ntp.org";
constexpr const char* NTP_SERVER_2 = "time.nist.gov";
constexpr unsigned long TIME_SYNC_CHECK_MS = 30000;
constexpr uint32_t CLOCK_VALID_EPOCH_SECONDS = 1700000000UL;

constexpr unsigned long GUI_REFRESH_MS = 250;
constexpr bool LOG_AVAILABILITY = true;

static_assert(USER_COUNT >= 1 && USER_COUNT <= 9,
              "Availability USER_COUNT must fit the fixed UI/protocol");
static_assert(P2P_SCAN_WINDOW_MS > P2P_DISCOVER_GAP_MS,
              "P2P scan window must exceed discovery repeat gap");

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
// startListening() is a blocking HTTP call. A long header timeout prevents the
// SSE task from acknowledging PTT. pollListening() itself uses available() and
// does not need a 65-second blocking read timeout.
constexpr uint16_t STREAM_HTTP_TIMEOUT_MS = 4000;
// Bound the blocking SSE TCP/TLS setup so a PTT pause request cannot be
// starved for the previous eight-second connect budget.
constexpr int32_t STREAM_CONNECT_TIMEOUT_MS = 1800;
constexpr unsigned long STREAM_TLS_HANDSHAKE_TIMEOUT_SECONDS = 2;
constexpr unsigned long STREAM_RECONNECT_INTERVAL_MS = 3000;

}  // namespace RtdbHttpConfig


namespace RtdbUploadConfig {

// Seven fixed blocks cover 1.4 seconds of 200 ms audio without placing full PCM
// arrays inside a FreeRTOS queue. When the network stalls and all blocks are
// occupied, the recorder replaces the oldest queued (not in-flight) chunk with
// the newest speech. This bounds RAM while keeping live audio current.
constexpr uint8_t TX_QUEUE_LENGTH = 7;
constexpr uint8_t UPLOAD_BATCH_MAX_CHUNKS = 1;
constexpr uint32_t BATCH_COLLECTION_WINDOW_MS = 300;
// While PTT is held, do not spend another full timeout retrying stale audio.
// Move to a newer queued chunk instead. Once recording stops, retry each
// remaining chunk once because latency is no longer more important than loss.
constexpr uint8_t LIVE_UPLOAD_MAX_ATTEMPTS = 1;
// Drain retries are coordinated at queue level so a failed block can remain
// owned by the queue across backoff or a Wi-Fi reconnect. Avoid immediately
// retrying the same broken socket twice inside one blocking request.
constexpr uint8_t DRAIN_UPLOAD_MAX_ATTEMPTS = 1;
constexpr unsigned long UPLOAD_RETRY_DELAY_MS = 200;
constexpr uint32_t TRANSIENT_RETRY_BASE_MS = 500;
constexpr uint32_t TRANSIENT_RETRY_MAX_MS = 2000;
// A response stall previously blocked the uploader for more than five seconds,
// while the fixed PCM pool only holds two seconds. Bound the response wait so
// the uploader can close the bad socket and continue with newer audio.
constexpr uint32_t UPLOAD_RESPONSE_TIMEOUT_MS = 3000;
// Bound both TCP setup and the TLS handshake. setTimeout() alone only bounds
// stream reads and previously allowed a failed connect to hold the request
// task indefinitely with one audio block marked in flight.
constexpr int32_t UPLOAD_CONNECT_TIMEOUT_MS = 4000;
constexpr unsigned long UPLOAD_TLS_HANDSHAKE_TIMEOUT_SECONDS = 4;
// During draining, bound the total attempts for one chunk. Firebase/TLS errors
// never force a Wi-Fi disconnect while the station remains connected.
constexpr uint8_t DRAIN_TOTAL_MAX_ATTEMPTS_PER_CHUNK = 3;
constexpr uint32_t UPLOAD_TASK_STACK_BYTES = 16384;
constexpr uint8_t UPLOAD_TASK_PRIORITY = 1;
constexpr uint8_t UPLOAD_TASK_CORE = 1;

}  // namespace RtdbUploadConfig


namespace RtdbRequestConfig {

constexpr uint8_t HIGH_QUEUE_LENGTH = 6;
constexpr uint8_t CONTROL_SLOT_COUNT = 4;
// Short REST operations use bounded connect and response budgets. Callers wait
// longer than the configured retry sequence without owning request memory.
constexpr uint16_t SHORT_HTTP_TIMEOUT_MS = 3000;
constexpr int32_t SHORT_CONNECT_TIMEOUT_MS = 3000;
constexpr uint32_t TASK_STACK_BYTES = 16384;
constexpr uint8_t TASK_PRIORITY = 2;
constexpr uint8_t TASK_CORE = 0;
constexpr uint32_t TASK_IDLE_DELAY_MS = 10;
constexpr uint32_t SYNC_OPERATION_TIMEOUT_MS = 60000;

}  // namespace RtdbRequestConfig


// Number of ESP-NOW audio packets aggregated into one speaker playback block.
//
// Each packet contains 100 PCM16 samples (12.5 ms at 8 kHz). The default of
// 8 therefore produces an 800-sample / 100-ms playback block. The hard upper
// bound is 16 packets because the shared playback block has room for 1,600
// samples, equal to one 200-ms VoIP chunk. You may also override this at build
// time, for example: -DP2P_PLAYBACK_PACKETS_PER_BLOCK=12.
#ifndef P2P_PLAYBACK_PACKETS_PER_BLOCK
#define P2P_PLAYBACK_PACKETS_PER_BLOCK 16
#endif

#define P2P_PLAYBACK_PACKETS_PER_BLOCK_UPPER_BOUND 16

#if P2P_PLAYBACK_PACKETS_PER_BLOCK < 1 || \
    P2P_PLAYBACK_PACKETS_PER_BLOCK > \
        P2P_PLAYBACK_PACKETS_PER_BLOCK_UPPER_BOUND
#error "P2P_PLAYBACK_PACKETS_PER_BLOCK must be between 1 and 16"
#endif

namespace P2pAudioConfig {

// Presence traffic is suppressed while P2P audio was recently active.
constexpr uint32_t AVAILABILITY_GUARD_MS = 300;

// Maximum failed RTDB audio upload attempts allowed during one joined-channel
// communication session before the transport is changed to ESP-NOW. The count
// is cumulative across PTT presses and resets only when the channel is left or
// a new channel is joined.
//
// Special case: when automatic P2P downgrade is enabled, 0 selects P2P
// immediately after joining. Turning the runtime automatic-downgrade setting
// off keeps VoIP active even with a zero limit. With value 10, P2P is selected
// after the 11th failed attempt while the runtime setting is enabled.
constexpr uint32_t VOIP_FAILED_UPLOAD_ATTEMPT_LIMIT = 10;

// Classic ESP-NOW payloads are limited to 250 bytes. One packet carries 100
// mono PCM16 samples (12.5 ms at 8 kHz) plus compact coordination metadata.
constexpr size_t SAMPLES_PER_PACKET = 100;

// constexpr view used by the receiver after the preprocessor validation above.
constexpr uint8_t PLAYBACK_PACKETS_PER_BLOCK =
    P2P_PLAYBACK_PACKETS_PER_BLOCK;
constexpr size_t PLAYBACK_SAMPLES_PER_BLOCK =
    SAMPLES_PER_PACKET * PLAYBACK_PACKETS_PER_BLOCK;

static_assert(
    PLAYBACK_PACKETS_PER_BLOCK >= 1 &&
        PLAYBACK_PACKETS_PER_BLOCK <=
            P2P_PLAYBACK_PACKETS_PER_BLOCK_UPPER_BOUND,
    "Invalid P2P playback aggregation count");
static_assert(
    PLAYBACK_SAMPLES_PER_BLOCK <= 1600,
    "P2P aggregate exceeds the shared 1600-sample playback block");

constexpr uint8_t RX_QUEUE_LENGTH = 12;

// The explicit channel-wide switch message is repeated because acknowledgments
// are intentionally deferred to a later protocol revision. START/AUDIO packets
// also imply P2P, so the peer can recover if all switch messages are lost.
constexpr uint8_t SWITCH_ANNOUNCE_REPEATS = 3;
constexpr uint16_t SWITCH_ANNOUNCE_GAP_MS = 20;

}  // namespace P2pAudioConfig

namespace ActivityLedConfig {

// LED semantics:
//   off          - idle/listening, with no unfinished local session
//   solid        - microphone recording while PTT is held
//   blinking     - recording has stopped, but queued uploads/session cleanup
//                  are still in progress
// The LED uses its own task so it keeps blinking while a blocking HTTPS cleanup
// request is running in the communication task.
constexpr uint32_t BUSY_BLINK_INTERVAL_MS = 250;
constexpr uint32_t TASK_STACK_BYTES = 2048;
constexpr uint8_t TASK_PRIORITY = 1;
constexpr uint8_t TASK_CORE = 1;

}  // namespace ActivityLedConfig


namespace PlaybackConfig {

constexpr uint8_t BLOCK_COUNT = 4;
constexpr uint8_t QUEUE_LENGTH = 3;
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
// The SSE task may already be inside a bounded TLS handshake when PTT is
// pressed. Wait slightly longer than that handshake budget for a clean pause.
constexpr uint32_t SSE_PAUSE_TIMEOUT_MS = 5000;
// After finishing a local session, delay the listener reconnect briefly. This
// gives a rapid second PTT press priority over opening another SSE TLS session.
constexpr uint32_t SSE_RESUME_RECONNECT_GRACE_MS = 350;
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

namespace AudioSettingsConfig {

// Runtime audio settings shown in the OLED settings window. Change this single
// constant to alter the amount added/subtracted by each GUI button press.
constexpr uint16_t ADJUST_STEP_PERCENT = 5;

constexpr uint16_t SPEAKER_VOLUME_DEFAULT_PERCENT = 100;
constexpr uint16_t SPEAKER_VOLUME_MIN_PERCENT = 0;
constexpr uint16_t SPEAKER_VOLUME_MAX_PERCENT = 100;

constexpr uint16_t MICROPHONE_GAIN_DEFAULT_PERCENT = 100;
constexpr uint16_t MICROPHONE_GAIN_MIN_PERCENT = 50;
constexpr uint16_t MICROPHONE_GAIN_MAX_PERCENT = 400;

// 0 disables the gate. At 100%, an amplified chunk whose peak amplitude is
// below NOISE_GATE_MAX_THRESHOLD is replaced with silence.
constexpr uint16_t MICROPHONE_NOISE_GATE_DEFAULT_PERCENT = 0;
constexpr uint16_t MICROPHONE_NOISE_GATE_MIN_PERCENT = 0;
constexpr uint16_t MICROPHONE_NOISE_GATE_MAX_PERCENT = 100;
constexpr int32_t NOISE_GATE_MAX_THRESHOLD = 4000;

}  // namespace AudioSettingsConfig

namespace RtdbBufferConfig {

// The talker writes chunks as /chunks/00000000, /chunks/00000001, ...
// A new press creates a new session and clears the previous session buffer.
constexpr uint32_t MAX_CHUNKS_PER_SESSION = 1200;  // 4 minutes at 200 ms/chunk.

// Keep this false for normal use, so the listener does not replay stale chunks
// already present in RTDB when it first connects. It still plays all new chunks
// that arrive after the stream opens.
constexpr bool PLAY_EXISTING_CHUNKS_ON_CONNECT = false;

}  // namespace RtdbBufferConfig
