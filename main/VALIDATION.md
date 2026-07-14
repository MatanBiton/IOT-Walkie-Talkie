# V5 two-device validation

## Before testing

1. Delete the old RTDB subtree once:

   ```text
   /rooms/room1/channels/ch01/talkers
   ```

2. Flash both ESPs from this same project.
3. Give each board a different `DEVICE_ID` and `USER_ID` in `app_config.h`.
4. Keep `AvailabilityConfig::ENABLED` set to `false`.

## A. Boot and idle listening

Both devices should print a stream path ending in:

```text
/rooms/room1/channels/ch01/talkers
```

There should be no Availability heartbeat, user-read, NTP, or ESP-NOW presence
logs. The RTDB service startup line should report `lowQueue=0`.

## B. Normal transmission

Hold PTT on device 1 for about three seconds.

Expected talker sequence:

```text
[STATE] old=Listening new=StartingSession
[RTDB_REQUEST] success type=SESSION_START ...
[AUDIO_TX] session_start_async ... uploadPrepared=true
[AUDIO_TX] persistent_connection open ...
[RTDB_REQUEST] success type=AUDIO_BATCH http=204 ... persistent=true
[AUDIO_TX] batch_result outcome=success success=true ...
```

Only the first audio packet should normally print `persistent_connection open`.
Later packets in that PTT session should reuse the same connection.

Expected listener sequence:

```text
[RTDB][STREAM][META] ... active=true ...
[RTDB][STREAM][CHUNK_DECODE_OK] ... seq=0 samples=1600
[AUDIO_RX] playback success=true ...
```

Release PTT. The talker should drain all queued blocks, close the persistent
connection, complete `SESSION_END`, and resume SSE listening. The listener should
receive inactive metadata after the final chunk.

## C. RTDB cleanup behavior

While PTT is held, chunk keys may appear below:

```text
/talkers/<device>/live/chunks
```

After a successful session end, `chunks` should be absent and `meta.active`
should be `false`. This cleanup is intentional and prevents historical SSE
snapshots from growing without bound.

## D. Repeated-session regression

Alternate at least 20 transmissions between the boards. Include several quick
presses and several 3-5 second presses.

Required observations:

- Every session reports `uploadPrepared=true`.
- Audio packets report `batch_result outcome=success success=true`.
- The listener receives `CHUNK_DECODE_OK` between active and inactive metadata.
- A session normally opens only one persistent audio connection.
- No progressive `largestBlock` collapse occurs per audio packet.
- No recurring `TOO_LESS_RAM`, `ENCODING`, `READ_TIMEOUT`, or
  `CONNECTION_REFUSED` pattern appears.
- `EVENT_BAD reason=missing_path` should not recur after the old subtree has been
  deleted and completed sessions are being cleaned.

The logged `minimumFree` value is a lifetime low-water mark and does not increase
again until reboot. Use current `free` and `largestBlock`, plus successful later
sessions, to evaluate recovery.

## E. Short PTT press

Press and release PTT in under one second.

The talker should still queue at least one 200 ms block during the initial capture
grace. The listener should see:

```text
META active=true
CHUNK_DECODE_OK
META active=false
```

An active/inactive pair with no chunk is a regression.

## F. Wi-Fi interruption

Disable Wi-Fi during transmission. Recording should stop, pending audio should be
discarded, and the state should enter `Reconnecting`. After Wi-Fi returns, the
ESP should clean up its session when possible and return to listening.

## G. Long-session and stalled-upload regression

Hold PTT for 15-20 seconds. The LED must remain on until the button is released,
even if an individual upload times out.

A temporary network stall may produce:

```text
[AUDIO_TX] queue_full policy=drop_oldest ...
[AUDIO_TX] batch_result outcome=transient_failure ...
[AUDIO_TX] transient_failure_continue ... recording=true
```

These lines are not a session abort. Later chunks should still be attempted.
When transport recovers, expect `outcome=success` on a later sequence number and
listener `CHUNK_DECODE_OK` events with possible sequence gaps.

If failures persist after PTT release, one controlled Wi-Fi recovery is expected:

```text
[AUDIO_TX] transport_recovery action=wifi_reconnect ...
[WIFI_MANAGER] forced_reconnect ...
```

After reconnection, the device must return to `Listening` and accept another PTT
press. It must not remain in an infinite drain loop.
