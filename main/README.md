# View Users + synchronized background availability patch (fixed ESP-NOW channel policy)

This patch adds a low-priority background availability service plus a real `View users` screen.
It is applied from the original project state, not on top of an older patch.

## What changed

- Added `availability_service.h/.cpp`.
- `main.ino` starts the availability service for both Talker and Listener roles.
- `gui.cpp/.h` now render `View users` instead of the old placeholder.
- `app_config.h` now has per-device user identity plus availability timing configuration.
- P2P ESP-NOW discovery now uses synchronized epoch-time windows instead of `millis()` since boot, so ESPs that start at different times do not permanently miss each other.
- ESP-NOW presence uses the current Wi-Fi/AP channel, not a separate physical channel. This avoids `Peer channel is not equal to the home channel` errors while the ESP is connected to Wi-Fi/RTDB.

## RTDB VOIP availability

Each ESP periodically writes its own heartbeat to:

```text
/statistics/users/user_<id>
```

For example, user `01` writes to:

```text
/statistics/users/user_01
```

The heartbeat includes:

- `userId`
- `deviceId`
- `roomId`
- `lastSeenServerMs` using Firebase server timestamp
- `deviceEpochMs` from the ESP clock after NTP sync
- `uptimeMs`
- `periodMs`
- `timezone`
- `tz`
- `localTime`

Every ESP also reads `/statistics/users` and marks a VOIP user unavailable when the last server timestamp is older than:

```text
AvailabilityConfig::PERIOD_TIME_MS * 1.1
```

Default period is 5000 ms, so the VOIP offline threshold is 5500 ms.

## Synchronized P2P availability

Each ESP broadcasts a compact ESP-NOW presence packet with its user number during a shared statistics window.
The shared window is based on wall-clock epoch time:

```cpp
inWindow = (epochMs % AvailabilityConfig::PERIOD_TIME_MS) <
           AvailabilityConfig::P2P_STATS_SYNC_WINDOW_MS;
```

Because this uses NTP-synchronized epoch time instead of `millis()` since boot, user `01` and user `02` enter the stats window at the same global time even if they booted at different moments.

Important ESP32 Wi-Fi/ESP-NOW constraint: while the ESP is connected to a Wi-Fi router, the radio is homed to the router/AP channel. ESP-NOW sends must use that same physical channel. Configuring a different peer channel causes logs like:

```text
ESPNOW: Peer channel is not equal to the home channel, send fail!
```

Therefore this patch does **not** switch to a separate physical stats channel. It keeps ESP-NOW on the current Wi-Fi channel and separates traffic using:

- a dedicated packet magic/version
- `P2P_STATS_LOGICAL_CHANNEL` inside the packet
- synchronized stats windows
- repeated short beacons
- per-user transmit offsets

Relevant defaults:

```cpp
AvailabilityConfig::P2P_ESPNOW_CURRENT_WIFI_CHANNEL = 0; // current AP/home channel
AvailabilityConfig::P2P_STATS_LOGICAL_CHANNEL = 250;
AvailabilityConfig::P2P_STATS_SYNC_WINDOW_MS = 300;
AvailabilityConfig::P2P_STATS_BROADCAST_REPEATS = 3;
AvailabilityConfig::P2P_STATS_BROADCAST_GAP_MS = 35;
AvailabilityConfig::P2P_STATS_USER_TX_OFFSET_STEP_MS = 35;
```

The per-user offset spreads transmissions inside the same window. For example, user `01` transmits around 35 ms into the window, user `02` around 70 ms, and so on.

P2P availability is intentionally less strict than VOIP availability. It is marked unavailable only after several missed synchronized windows:

```cpp
AvailabilityConfig::P2P_OFFLINE_AFTER_MISSED_PERIODS = 3;
```

This prevents the GUI from flickering when one ESP-NOW packet is lost.

All ESPs that should discover each other over ESP-NOW must be on the same Wi-Fi/AP channel. The easiest way is to connect them to the same router/hotspot.

## Device setup

Before uploading to each ESP, change these in `app_config.h`:

```cpp
constexpr const char* DEVICE_ID = "esp32-Talker-01";
constexpr const char* USER_ID = "01";
```

Use IDs `01` through `05` for the five assumed users.

## View users screen

The screen displays five users:

```text
U01 V:ON P:--
U02 V:-- P:ON
...
```

- `V` = VOIP/RTDB availability
- `P` = P2P/ESP-NOW availability
- Left button returns to the main menu
- Right button forces a redraw

The GUI is updated from a small static snapshot every `AvailabilityConfig::GUI_REFRESH_MS` milliseconds. No dynamic list allocation or queues are used for availability state.
