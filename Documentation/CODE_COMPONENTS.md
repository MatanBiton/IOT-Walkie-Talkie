# Firmware Component Guide
## System flow

`main.ino` coordinates the GUI, PTT state machine, audio capture/playback, Wi-Fi, Firebase RTDB VoIP, and ESP-NOW P2P fallback.

- **VoIP path:** `AudioIO` → `RtdbRequestService` → Firebase RTDB → `RtdbAudioStream` → `AudioIO`.
- **P2P path:** `AudioIO` → `P2pAudio` → `EspNowTransport` → `P2pAudio` → `AudioIO`.
- **Supporting services:** `WifiConnection`, `AvailabilityService`, `RuntimeStatistics`, `Communication`, and `Gui`.

## Entry point and configuration

| File | Role |
|---|---|
| `main.ino` | Main application coordinator. Initializes all modules and FreeRTOS tasks; handles channel changes, PTT sessions, VoIP/P2P selection and fallback, SSE pause/resume, playback queues, LED behavior, GUI events, availability refreshes, NTP synchronization, and deferred statistics persistence. |
| `app_config.h` | Central compile-time configuration: device/user identity, Wi-Fi and Firebase settings, runtime defaults, queue sizes, timeouts, retry policies, RTDB buffering, availability behavior, audio format/settings, P2P thresholds, task parameters, and statistics persistence. Keep local credentials out of version control. |
| `consts.h` | Hardware constants: ESP32 GPIO mapping, OLED dimensions/address, and common button/debounce logic. |

## Audio and communication state

| File | Role |
|---|---|
| `audio_io.h` | Public interface for microphone capture, speaker playback/draining, playback enablement, speaker volume, microphone gain, and noise-gate settings. |
| `audio_io.cpp` | Implements the two I2S devices. Captures mono PCM, applies gain and noise gating, converts mono samples for speaker output, applies volume scaling, manages DMA draining, and supports immediate playback cancellation on communication-blocking screens. |
| `communication_state.h` | Defines the active transport (`VoIP`/`P2P`), communication states, transition API, playback permission, and automatic-P2P-downgrade setting. |
| `communication_state.cpp` | Thread-safe storage and logging for transport changes, state-machine transitions, and the runtime automatic-fallback toggle. |

## Network transports

| File | Role |
|---|---|
| `wifi_connection.h` | Public asynchronous Wi-Fi manager API: connect/status checks, runtime enable/disable, controlled reconnect requests, and connectivity-restored observers. |
| `wifi_connection.cpp` | Runs the Wi-Fi manager task, processes ESP32 Wi-Fi events, performs rate-limited reconnects, logs connection diagnostics, and notifies dependent tasks after connectivity returns. Disabling infrastructure Wi-Fi keeps the STA radio available for ESP-NOW. |
| `esp_now_transport.h` | Shared low-level ESP-NOW interface used by both audio and availability traffic. Defines receive-handler registration and serialized broadcast sending. |
| `esp_now_transport.cpp` | Initializes ESP-NOW and its broadcast peer, dispatches received packets to registered modules, serializes sends, and prioritizes audio by rejecting/deferring availability traffic while P2P audio is active. |
| `p2p_audio.h` | Defines the P2P audio protocol and public API for transport-switch announcements, stream start/audio/end packets, received-packet retrieval, and remote switch requests. |
| `p2p_audio.cpp` | Implements the compact ESP-NOW wire protocol, stream IDs and sequence numbers, packet validation, repeated switch announcements, fixed receive queues, and conversion from wire packets to PCM blocks consumed by the application. |

## Firebase RTDB VoIP

| File | Role |
|---|---|
| `rtdb_request_service.h` | Public RTDB worker API for session start/end, stream start/stop, fixed audio-block acquisition/submission, upload status/counters, abort/reconnect handling, and low-priority presence operations. |
| `rtdb_request_service.cpp` | Sole owner of the Firebase client and TLS connection. Serializes all RTDB work in one task, uploads PCM chunks, writes session metadata, controls the SSE stream, handles retries/timeouts, uses fixed memory pools and queues, drops stale audio under pressure, and schedules presence work only when audio/control traffic is idle. |
| `rtdb_audio_stream.h` | Public listener/parser interface and metadata structure for decoded remote PCM chunks. |
| `rtdb_audio_stream.cpp` | Receives Firebase SSE `put`/`patch` events from the RTDB worker, tracks one active remote talker/session, rejects stale or unrelated chunks, decodes Base64 PCM into caller buffers, and releases stream memory when listening stops. |

## Availability, GUI, and statistics

| File | Role |
|---|---|
| `availability_service.h` | Public API and status types for user availability, refresh state, event-driven polling, optional maintenance leases, and copying cached user status to the GUI. |
| `availability_service.cpp` | Combines server-timestamped RTDB presence leases with on-demand ESP-NOW discovery/replies. Classifies VoIP/P2P evidence as available, stale, or unknown without a dedicated task or periodic full-database polling. |
| `gui.h` | Declares all OLED screens, channel/user/status types, navigation results, communication-blocking rules, and the `AppGui` interface. |
| `gui.cpp` | Implements button handling and SSD1306 rendering for the main menu, channel selection/join screen, user availability, four statistics pages, audio settings, Wi-Fi/P2P settings, and channel-leave/navigation events. |
| `runtime_statistics.h` | Defines the statistics snapshot and recording API for sessions, talk/heard time, transport usage, losses, fallbacks, channel usage, and heard users. |
| `runtime_statistics.cpp` | Maintains thread-safe counters, merges RTDB network counters, calculates P2P diagnostics, and loads/saves selected long-term statistics in ESP32 NVS using deferred, schema-versioned persistence. |

## Ownership summary

- **Hardware access:** `audio_io.*`, `consts.h`, `gui.cpp`.
- **Application orchestration:** `main.ino`, `communication_state.*`.
- **Infrastructure networking:** `wifi_connection.*`, `rtdb_request_service.*`, `rtdb_audio_stream.*`.
- **Local fallback networking:** `esp_now_transport.*`, `p2p_audio.*`.
- **User-facing support:** `gui.*`, `availability_service.*`, `runtime_statistics.*`.
