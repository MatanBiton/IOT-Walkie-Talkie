#pragma once

namespace WifiConnection {

// Observers are called from the Wi-Fi manager task whenever station
// connectivity returns. Keep callbacks short and non-blocking; they should
// normally wake their owning task or set a reconnect flag.
using ConnectedObserver = void (*)(void* context);

bool begin();
bool ensureConnected();
bool isConnected();

// Uses a fixed-size observer table and performs no dynamic allocation.
// Registering while already connected schedules one prompt notification.
bool registerConnectedObserver(ConnectedObserver observer, void* context = nullptr);
bool unregisterConnectedObserver(ConnectedObserver observer, void* context = nullptr);

}  // namespace WifiConnection
