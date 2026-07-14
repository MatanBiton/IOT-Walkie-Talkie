# ESP32 RTDB walkie-talkie

The same firmware runs on every ESP32. Before flashing each board, assign a
unique identity in `app_config.h`:

```cpp
constexpr const char* DEVICE_ID = "esp32-02";
constexpr const char* USER_ID = "02";
```

The audio path is half duplex. Pressing PTT pauses the local SSE listener,
creates this ESP's RTDB session, captures and uploads microphone chunks, drains
the bounded upload queue, closes the session, and resumes listening.

## RTDB layout

Each device owns a separate subtree:

```text
/rooms/<room>/channels/ch01/talkers/esp32-01/live/meta
/rooms/<room>/channels/ch01/talkers/esp32-01/live/chunks/<sequence>
/rooms/<room>/channels/ch01/talkers/esp32-02/live/meta
/rooms/<room>/channels/ch01/talkers/esp32-02/live/chunks/<sequence>
```

Transmitters never write the same RTDB node, so no shared channel-lock mechanism is used.
The listener opens one SSE stream on:

```text
/rooms/<room>/channels/ch01/talkers
```

If multiple remote ESPs talk simultaneously, a listener plays the first session
from which it receives a chunk and ignores the others until that session ends or
times out.

Audio is 8 kHz mono signed 16-bit PCM, split into 200 ms chunks and Base64
encoded.

## Request and memory model

- One RTDB request task serializes session control and audio writes.
- `SESSION_START` is processed before queued audio and authorizes only the
  matching channel/session.
- Microphone PCM is stored in ten fixed blocks; the FreeRTOS queue contains only
  block indices. If the network stalls, the oldest queued block is replaced by
  the newest microphone audio so RAM stays bounded and live speech stays current.
- Audio JSON is assembled in one fixed static buffer.
- One persistent HTTPS connection is reused for all chunks in a PTT session.
- Transient upload failures do not terminate a held PTT session; the uploader
  closes the bad socket and continues with newer queued audio.
- The persistent audio connection is closed before `SESSION_END`.
- Completed chunk nodes are deleted at session end so SSE reconnect snapshots
  stay small.
- The receiver decodes Base64 directly from the existing SSE event buffer.

Chunk nodes are intentionally temporary. They may be visible in the Firebase
console while PTT is held or uploads are draining, but a successful session end
removes them and leaves only inactive metadata.

## Availability status

Availability remains disabled:

```cpp
namespace AvailabilityConfig {
constexpr bool ENABLED = false;
}
```

With this setting, the availability task, NTP setup, ESP-NOW presence flow,
low-priority RTDB queue/mutex, heartbeat writes, user reads, and response buffer
reservation are not started.

Do not enable Availability until its earlier crash has been diagnosed separately.

## First V5 flash

Delete the existing test subtree once before the first V4 test so stale chunk
history from older firmware does not appear in the initial SSE snapshot:

```text
/rooms/room1/channels/ch01/talkers
```

Then flash both boards with different `DEVICE_ID` and `USER_ID` values.

See `VALIDATION.md` for the bench procedure and expected logs. See
`LONG_SESSION_FIX_NOTES.md` for the transport-stall diagnosis and recovery policy.
