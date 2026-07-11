# RTDB Audio Buffered Batch Patch

This version keeps the one-directional model:

- Talker ESP records while `Pins::MAIN_BUTTON` is held.
- Listener ESP continuously streams RTDB and plays received PCM chunks.

## Main fix in this patch

The previous async version still uploaded one chunk per HTTPS request. If a 200 ms audio chunk takes longer than 200 ms to upload, the upload queue eventually fills and chunks are dropped.

This patch changes the upload side to:

```text
recording loop -> FreeRTOS queue -> upload task -> RTDB PATCH batch
```

Instead of uploading:

```text
PUT /chunks/00000000
PUT /chunks/00000001
PUT /chunks/00000002
```

it uploads several children in one request:

```text
PATCH /chunks
{
  "00000000": { ... },
  "00000001": { ... },
  "00000002": { ... }
}
```

This reduces TLS/HTTP overhead and should produce about 25 chunks for a 5 second recording at 200 ms per chunk.

## Important config

In `app_config.h`:

```cpp
constexpr uint8_t TX_QUEUE_LENGTH = 24;
constexpr uint8_t UPLOAD_BATCH_MAX_CHUNKS = 6;
constexpr uint8_t UPLOAD_MAX_ATTEMPTS = 2;
```

For a 5 second test, expected chunk count is roughly:

```text
5000 ms / 200 ms = 25 chunks
```

A queue length of 24 plus the currently-uploading batch should be enough for that test unless the ESP runs out of heap or Wi-Fi stalls badly.

## Logs to check

Good recording should show many enqueue lines:

```text
[Talker][QUEUE_ENQUEUE_OK] ... seq=0
[Talker][QUEUE_ENQUEUE_OK] ... seq=1
...
[Talker][QUEUE_ENQUEUE_OK] ... seq=24
```

If chunks are dropped, you will see:

```text
[Talker][QUEUE_FULL_DROP]
```

Batch upload should show:

```text
[Talker][UPLOAD_TASK_DEQUEUE_BATCH] ... count=6 firstSeq=...
[Talker][BATCH_SEND_OK] ... count=6 firstSeq=... lastSeq=...
```

At startup, check free heap:

```text
[READY] RTDB buffered async talker initialized ... freeHeap=...
```

If queue creation fails or free heap is very low, reduce `TX_QUEUE_LENGTH` or `UPLOAD_BATCH_MAX_CHUNKS`.
