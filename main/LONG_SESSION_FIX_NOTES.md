# Long-session transport fix

## What the new logs showed

The heap cleanup fix is working: after completed control requests, current free
heap returns to roughly 97-100 KB and the largest free block remains large. The
low `minimumFree` value is only the lifetime low-water mark.

The failed long session was instead caused by transport back-pressure:

1. The first audio PATCH waited 5.8 seconds and ended with `READ_TIMEOUT`.
2. The fixed ten-block PCM pool holds only two seconds of audio.
3. While that one request was blocked, the pool filled and newer microphone
   chunks were discarded.
4. Both attempts for the first chunk failed, so the old code treated the whole
   session as failed and discarded everything still queued.
5. The listener therefore received `active=true`, but no chunk event.

## Changes

- Queue-full policy is now `drop_oldest`: the oldest queued block is reused for
  the newest microphone audio. The in-flight block is never reclaimed.
- A transient socket/HTTP failure no longer stops recording or turns off the
  LED while PTT remains held.
- During live capture, stale chunks receive one upload attempt. The uploader
  then moves to newer queued audio rather than spending another full timeout on
  the same old chunk.
- During post-PTT draining, each remaining chunk still receives two attempts.
- The audio response timeout is reduced from 5 seconds to 3 seconds; TCP/TLS
  setup is bounded to 4 seconds.
- If repeated failures continue during draining while Wi-Fi still reports
  connected, the request service asks the Wi-Fi manager for one controlled
  reconnect. This prevents later PTT sessions from remaining stuck behind a
  stale network path.
- Transport failures are separated from fatal local failures. Only invalid
  session state, invalid payload construction, or a non-retryable HTTP status
  aborts recording immediately.

## Expected recovery logs

During a temporary stall, logs may include:

```text
[AUDIO_TX] queue_full policy=drop_oldest ...
[AUDIO_TX] batch_result outcome=transient_failure ...
[AUDIO_TX] transient_failure_continue ... recording=true
```

The LED should remain on while PTT is held. If connectivity recovers, later
sequence numbers should report `outcome=success`, and the listener should begin
playing from the recovered live point. Sequence gaps are expected after a stall.

If the transport remains broken after PTT release, logs may include:

```text
[AUDIO_TX] transport_recovery action=wifi_reconnect ...
[WIFI_MANAGER] forced_reconnect ...
```

The device should reconnect, clean up the session, and permit the next PTT
attempt instead of remaining stuck.
