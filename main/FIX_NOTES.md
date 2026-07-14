# V4 repeated-transmission stability fix

## Observed failure

After one or more transmissions, the talker reached a state similar to:

```text
SESSION_END ... queued=10 dropped=1 durationMs=18243
RTDB_HEAP ... free=50016 largest=2164 minimum=304
```

The listener received `active=true` and `active=false` metadata but no chunk
events.

## Root causes

1. Every 200 ms audio chunk created a new `WiFiClientSecure`/`HTTPClient`
   connection and built a roughly 4.5 KB dynamic Arduino `String`. Ten queued
   chunks therefore caused repeated TLS allocation/free cycles. Total free heap
   remained nonzero, but the largest contiguous block collapsed to about 2 KB,
   so audio-sized allocations and TLS operations stopped succeeding.
2. Completed chunk maps remained below `/talkers/<device>/live/chunks`. Every SSE
   reconnect could therefore download an increasingly large historical root
   snapshot, further fragmenting the listener/talker heap.
3. Availability was disabled at the application level, but the RTDB service still
   allocated its low-priority queue, mutex, and a 3 KB response buffer.

## V4 changes

- Audio uses one persistent HTTP/1.1 TLS connection for the entire PTT session.
- Chunk JSON is built in a fixed static buffer; no 4.5 KB heap `String` is created.
- The persistent audio connection is closed before the small `SESSION_END`
  control request, so two TLS contexts are never alive together.
- `SESSION_START` atomically clears stale chunks before authorizing uploads.
- `SESSION_END` marks the session inactive and removes completed chunks before
  the talker resumes its SSE stream.
- SSE line/data buffers are reserved before TLS connection, when a large
  contiguous block is still available.
- When `AvailabilityConfig::ENABLED == false`, no availability queue, mutex,
  task, NTP flow, RTDB read, or 3 KB response reserve is created.

## Expected talker logs

The first chunk in a session should open one persistent connection:

```text
[AUDIO_TX] persistent_connection open host=... channel=1 session=...
[RTDB_REQUEST] success type=AUDIO_BATCH http=204 ... persistent=true
```

Later chunks in the same session should succeed without another
`persistent_connection open` line. At session end:

```text
[AUDIO_TX] persistent_connection closed reason=session_end ...
[RTDB_REQUEST] success type=SESSION_END http=204 ...
```

The `minimum` heap value is historical and does not reset until reboot. The
important regression signal is that `largestBlock` should no longer shrink on
every individual audio packet or prevent subsequent sessions.

## RTDB console behavior

Chunk nodes exist only while a session is being transmitted/drained. They are
removed by the successful `SESSION_END` PATCH. Seeing only `meta.active=false`
after releasing PTT is therefore expected in V4.
