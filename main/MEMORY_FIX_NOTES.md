# V5 continuous memory cleanup and stalled-upload recovery

## What the supplied logs show

- `minimumFree=900` is the lifetime heap low-water mark. It cannot increase until
  reboot, so it is not the amount of heap still available at the failure. At the
  first failed upload the current heap was about 97 KB and the largest free block
  was about 32 KB.
- The idle/current heap nevertheless falls by roughly 0.4-0.5 KB per complete PTT
  cycle, and the largest block also trends downward. This is repeated dynamic
  allocation/retention and fragmentation around the SSE and TLS lifecycles.
- On the final failed session, one audio block remains marked in flight while the
  request task is stuck in connection/response handling. `DrainingUploads` then
  prints `drain_timeout` forever because clearing the ready queue cannot release
  the block already removed by the request task.
- The listener receives and plays the successful sessions. Its two
  `queue_full policy=drop_oldest` messages are playback back-pressure, not the
  cause of the talker becoming unable to transmit.

## Changes

1. SSE stop now fully releases the reserved `String` backing buffers and
   reconstructs both long-lived HTTP/TLS objects every time listening is paused,
   stopped, or a connection attempt fails.
2. The persistent audio TLS client is reconstructed after every close and every
   failed connection attempt, rather than carrying internal allocations into the
   next attempt/session.
3. Control requests collect the Firebase `ETag` header only when it is actually
   requested. Every control-request exit explicitly ends HTTP and stops TLS.
4. Audio blocks are zeroed whenever returned to the free pool, including normal
   completion, explicit release, and queue discard.
5. TCP connection and TLS handshake time are bounded to five seconds.
6. A drain timeout now occurs once, requests cancellation of the in-flight upload,
   and lets the request task return that block to the free pool. Wi-Fi loss also
   requests the same cancellation.
7. Added `[RTDB_HEAP] ... phase=after_cleanup` so current heap recovery can be
   compared after every control request.

## Validation signals

After reboot, run at least 20 PTT sessions, including a 10-20 second press.

- Judge recovery using current `free` and `largest`, not `minimum`.
- `free` after `STREAM stopped` and `SESSION_END after_cleanup` should settle near
  a stable range instead of dropping by roughly the same amount every session.
- A failed upload may print one `drain_timeout ... abortRequested=true`, followed
  by `batch_result` and normal session cleanup. It must not print that line
  continuously with `queueDepth=1`.
- The listener should continue receiving later sessions after a failed attempt.
