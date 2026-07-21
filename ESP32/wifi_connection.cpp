#include "wifi_connection.h"

#include <Arduino.h>
#include <WiFi.h>
#include <esp_heap_caps.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

#include "app_config.h"

namespace {

constexpr uint8_t MAX_CONNECTED_OBSERVERS = 4;
constexpr uint32_t WIFI_MANAGER_STACK_BYTES = 4096;
constexpr UBaseType_t WIFI_MANAGER_PRIORITY = 1;
constexpr BaseType_t WIFI_MANAGER_CORE = 0;
constexpr unsigned long WIFI_MANAGER_POLL_MS = 250;

struct ObserverSlot {
  WifiConnection::ConnectedObserver observer = nullptr;
  void* context = nullptr;
  bool initialNotificationPending = false;
};

TaskHandle_t wifiManagerTaskHandle = nullptr;
ObserverSlot observerSlots[MAX_CONNECTED_OBSERVERS];
portMUX_TYPE observerMux = portMUX_INITIALIZER_UNLOCKED;
portMUX_TYPE wifiEventMux = portMUX_INITIALIZER_UNLOCKED;
portMUX_TYPE reconnectRequestMux = portMUX_INITIALIZER_UNLOCKED;
portMUX_TYPE enabledMux = portMUX_INITIALIZER_UNLOCKED;
bool infrastructureWifiEnabled = RuntimeSettingsConfig::WIFI_ENABLED_DEFAULT;
unsigned long lastReconnectAttemptMs = 0;
bool haveReconnectAttempt = false;
bool connectivityReturnEventPending = false;
bool forcedReconnectPending = false;
char forcedReconnectReason[64] = {0};

bool readInfrastructureWifiEnabled() {
  portENTER_CRITICAL(&enabledMux);
  const bool snapshot = infrastructureWifiEnabled;
  portEXIT_CRITICAL(&enabledMux);
  return snapshot;
}

void logWifiEvent(WiFiEvent_t event, const WiFiEventInfo_t& info) {
  const wl_status_t status = WiFi.status();
  const bool hasIp = status == WL_CONNECTED || event == ARDUINO_EVENT_WIFI_STA_GOT_IP;
  const IPAddress ip = WiFi.localIP();
  const int32_t channel = WiFi.channel();
  const int32_t rssi = hasIp ? WiFi.RSSI() : 0;
  const uint32_t freeHeap = ESP.getFreeHeap();
  const size_t largestBlock = heap_caps_get_largest_free_block(MALLOC_CAP_8BIT);
  const size_t minimumFree = heap_caps_get_minimum_free_size(MALLOC_CAP_8BIT);

  uint8_t disconnectReason = 0;
  if (event == ARDUINO_EVENT_WIFI_STA_DISCONNECTED) {
    disconnectReason = info.wifi_sta_disconnected.reason;
  }

  if (hasIp) {
    Serial.printf(
        "[WIFI_EVENT] type=%s status=%d ip=%u.%u.%u.%u channel=%ld rssi=%ld "
        "freeHeap=%lu largestBlock=%u minFreeHeap=%u reason=%u\n",
        WiFi.eventName(event),
        static_cast<int>(status),
        static_cast<unsigned int>(ip[0]),
        static_cast<unsigned int>(ip[1]),
        static_cast<unsigned int>(ip[2]),
        static_cast<unsigned int>(ip[3]),
        static_cast<long>(channel),
        static_cast<long>(rssi),
        static_cast<unsigned long>(freeHeap),
        static_cast<unsigned int>(largestBlock),
        static_cast<unsigned int>(minimumFree),
        static_cast<unsigned int>(disconnectReason));
    return;
  }

  Serial.printf(
      "[WIFI_EVENT] type=%s status=%d ip=- channel=%ld rssi=%ld "
      "freeHeap=%lu largestBlock=%u minFreeHeap=%u reason=%u\n",
      WiFi.eventName(event),
      static_cast<int>(status),
      static_cast<long>(channel),
      static_cast<long>(rssi),
      static_cast<unsigned long>(freeHeap),
      static_cast<unsigned int>(largestBlock),
      static_cast<unsigned int>(minimumFree),
      static_cast<unsigned int>(disconnectReason));
}

void onWifiEvent(WiFiEvent_t event, WiFiEventInfo_t info) {
  logWifiEvent(event, info);

  if (event == ARDUINO_EVENT_WIFI_STA_GOT_IP) {
    portENTER_CRITICAL(&wifiEventMux);
    connectivityReturnEventPending = true;
    portEXIT_CRITICAL(&wifiEventMux);
  }

  // The callback runs on Arduino's Wi-Fi event task. It only wakes the manager;
  // reconnection and observer callbacks remain owned by the manager task.
  if (wifiManagerTaskHandle != nullptr) {
    xTaskNotifyGive(wifiManagerTaskHandle);
  }
}

bool takeConnectivityReturnEvent() {
  portENTER_CRITICAL(&wifiEventMux);
  const bool pending = connectivityReturnEventPending;
  connectivityReturnEventPending = false;
  portEXIT_CRITICAL(&wifiEventMux);
  return pending;
}

bool takeForcedReconnectRequest(char* outReason, size_t outReasonSize) {
  bool pending = false;
  portENTER_CRITICAL(&reconnectRequestMux);
  pending = forcedReconnectPending;
  if (pending) {
    forcedReconnectPending = false;
    if (outReason != nullptr && outReasonSize > 0) {
      snprintf(outReason, outReasonSize, "%s", forcedReconnectReason);
    }
    forcedReconnectReason[0] = '\0';
  }
  portEXIT_CRITICAL(&reconnectRequestMux);
  return pending;
}

void notifyConnectedObservers(bool notifyAll) {
  WifiConnection::ConnectedObserver callbacks[MAX_CONNECTED_OBSERVERS] = {};
  void* contexts[MAX_CONNECTED_OBSERVERS] = {};
  uint8_t callbackCount = 0;

  portENTER_CRITICAL(&observerMux);
  for (uint8_t i = 0; i < MAX_CONNECTED_OBSERVERS; ++i) {
    ObserverSlot& slot = observerSlots[i];
    if (slot.observer == nullptr || (!notifyAll && !slot.initialNotificationPending)) {
      continue;
    }

    callbacks[callbackCount] = slot.observer;
    contexts[callbackCount] = slot.context;
    slot.initialNotificationPending = false;
    ++callbackCount;
  }
  portEXIT_CRITICAL(&observerMux);

  if (callbackCount > 0) {
    Serial.printf(
        "[WIFI_MANAGER] connectivity_return observers=%u\n",
        static_cast<unsigned int>(callbackCount));
  }

  for (uint8_t i = 0; i < callbackCount; ++i) {
    callbacks[i](contexts[i]);
  }
}

void requestConnectionAttempt() {
  if (!readInfrastructureWifiEnabled()) {
    return;
  }

  const unsigned long nowMs = millis();
  if (haveReconnectAttempt &&
      (nowMs - lastReconnectAttemptMs) < WifiConfig::RECONNECT_INTERVAL_MS) {
    return;
  }

  lastReconnectAttemptMs = nowMs;
  haveReconnectAttempt = true;

  Serial.printf(
      "[WIFI_MANAGER] reconnect_start method=begin status=%d cooldownMs=%lu\n",
      static_cast<int>(WiFi.status()),
      static_cast<unsigned long>(WifiConfig::RECONNECT_INTERVAL_MS));

  // WiFi.begin() only initiates the connection. The manager waits for a later
  // GOT_IP event and never treats this return value as proof of connectivity.
  const wl_status_t beginStatus = WiFi.begin(WifiConfig::SSID, WifiConfig::PASSWORD, WifiConfig::WIFI_CHANNEL);
  Serial.printf(
      "[WIFI_MANAGER] reconnect_requested method=begin status=%d awaitingEvent=true\n",
      static_cast<int>(beginStatus));
}

void wifiManagerTask(void*) {
  bool previouslyConnected = false;
  bool previouslyEnabled = readInfrastructureWifiEnabled();

  for (;;) {
    const bool enabled = readInfrastructureWifiEnabled();
    if (!enabled) {
      if (previouslyEnabled) {
        Serial.println(
            "[WIFI_MANAGER] disabled infrastructure=false espNowRadio=true");
      }

      if (previouslyEnabled || WiFi.status() == WL_CONNECTED ||
          previouslyConnected) {
        // Keep credentials and the WIFI_STA interface. ESP-NOW depends on the
        // radio/interface even though AP connectivity is intentionally off.
        WiFi.disconnect(false, false);
      }

      takeConnectivityReturnEvent();
      char ignoredReason[64] = {0};
      takeForcedReconnectRequest(ignoredReason, sizeof(ignoredReason));
      haveReconnectAttempt = false;
      previouslyConnected = false;
      previouslyEnabled = false;
      ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(WIFI_MANAGER_POLL_MS));
      continue;
    }

    if (!previouslyEnabled) {
      Serial.println("[WIFI_MANAGER] enabled reconnecting=true");
      haveReconnectAttempt = false;
      previouslyConnected = false;
    }
    previouslyEnabled = true;

    char reconnectReason[64] = {0};
    if (takeForcedReconnectRequest(reconnectReason, sizeof(reconnectReason))) {
      Serial.printf(
          "[WIFI_MANAGER] forced_reconnect reason=%s status=%d\n",
          reconnectReason[0] == '\0' ? "unspecified" : reconnectReason,
          static_cast<int>(WiFi.status()));
      // Preserve credentials and NVS. The manager owns the subsequent begin().
      WiFi.disconnect(false, false);
      haveReconnectAttempt = false;
      previouslyConnected = false;
      vTaskDelay(pdMS_TO_TICKS(50));
    }

    const bool connected = WifiConnection::isConnected();
    const bool connectivityReturned = takeConnectivityReturnEvent();
    if (connected) {
      if (!previouslyConnected || connectivityReturned) {
        const IPAddress ip = WiFi.localIP();
        Serial.printf(
            "[WIFI_MANAGER] connected ip=%u.%u.%u.%u channel=%ld rssi=%ld\n",
            static_cast<unsigned int>(ip[0]),
            static_cast<unsigned int>(ip[1]),
            static_cast<unsigned int>(ip[2]),
            static_cast<unsigned int>(ip[3]),
            static_cast<long>(WiFi.channel()),
            static_cast<long>(WiFi.RSSI()));
        notifyConnectedObservers(true);
      } else {
        notifyConnectedObservers(false);
      }
    } else {
      if (previouslyConnected) {
        Serial.printf(
            "[WIFI_MANAGER] disconnected status=%d reconnectCooldownMs=%lu\n",
            static_cast<int>(WiFi.status()),
            static_cast<unsigned long>(WifiConfig::RECONNECT_INTERVAL_MS));
      }
      requestConnectionAttempt();
    }

    previouslyConnected = connected;
    ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(WIFI_MANAGER_POLL_MS));
  }
}

}  // namespace

namespace WifiConnection {

bool isEnabled() {
  return readInfrastructureWifiEnabled();
}

bool isConnected() {
  return isEnabled() && WiFi.status() == WL_CONNECTED;
}

void setEnabled(bool enabled) {
  bool previous;
  portENTER_CRITICAL(&enabledMux);
  previous = infrastructureWifiEnabled;
  infrastructureWifiEnabled = enabled;
  portEXIT_CRITICAL(&enabledMux);

  if (previous == enabled) {
    return;
  }

  Serial.printf(
      "[SETTINGS] wifi=%s espNowRadio=enabled\n",
      enabled ? "enabled" : "disabled");

  if (wifiManagerTaskHandle != nullptr) {
    xTaskNotifyGive(wifiManagerTaskHandle);
  }
}

bool begin() {
  if (wifiManagerTaskHandle != nullptr) {
    return true;
  }

  WiFi.onEvent(onWifiEvent);
  WiFi.persistent(false);
  WiFi.setAutoReconnect(false);
  WiFi.mode(WIFI_STA);
  WiFi.setSleep(false);

  const BaseType_t created = xTaskCreatePinnedToCore(
      wifiManagerTask,
      "wifi_manager",
      WIFI_MANAGER_STACK_BYTES,
      nullptr,
      WIFI_MANAGER_PRIORITY,
      &wifiManagerTaskHandle,
      WIFI_MANAGER_CORE);

  if (created != pdPASS) {
    wifiManagerTaskHandle = nullptr;
    Serial.println("[ERROR] Failed creating Wi-Fi manager task");
    return false;
  }

  Serial.printf(
      "[READY] Wi-Fi manager initialized async=true reconnectIntervalMs=%lu "
      "stackBytes=%lu core=%ld\n",
      static_cast<unsigned long>(WifiConfig::RECONNECT_INTERVAL_MS),
      static_cast<unsigned long>(WIFI_MANAGER_STACK_BYTES),
      static_cast<long>(WIFI_MANAGER_CORE));
  return true;
}

bool ensureConnected() {
  // Kept for source compatibility. It is deliberately non-blocking and never
  // manipulates or wakes the radio; only the manager task owns reconnection.
  return isConnected();
}

void requestReconnect(const char* reason) {
  if (!isEnabled()) {
    Serial.printf(
        "[WIFI_MANAGER] reconnect_ignored reason=%s wifi=disabled\n",
        (reason == nullptr || reason[0] == '\0') ? "unspecified" : reason);
    return;
  }

  portENTER_CRITICAL(&reconnectRequestMux);
  if (!forcedReconnectPending) {
    forcedReconnectPending = true;
    snprintf(
        forcedReconnectReason,
        sizeof(forcedReconnectReason),
        "%s",
        (reason == nullptr || reason[0] == '\0') ? "unspecified" : reason);
  }
  portEXIT_CRITICAL(&reconnectRequestMux);

  if (wifiManagerTaskHandle != nullptr) {
    xTaskNotifyGive(wifiManagerTaskHandle);
  }
}

bool registerConnectedObserver(ConnectedObserver observer, void* context) {
  if (observer == nullptr) {
    return false;
  }

  bool registered = false;
  const bool connectedNow = isConnected();
  portENTER_CRITICAL(&observerMux);
  for (uint8_t i = 0; i < MAX_CONNECTED_OBSERVERS; ++i) {
    ObserverSlot& slot = observerSlots[i];
    if (slot.observer == observer && slot.context == context) {
      slot.initialNotificationPending = slot.initialNotificationPending || connectedNow;
      registered = true;
      break;
    }
    if (slot.observer == nullptr) {
      slot.observer = observer;
      slot.context = context;
      slot.initialNotificationPending = connectedNow;
      registered = true;
      break;
    }
  }
  portEXIT_CRITICAL(&observerMux);

  if (registered && wifiManagerTaskHandle != nullptr) {
    xTaskNotifyGive(wifiManagerTaskHandle);
  }
  return registered;
}

bool unregisterConnectedObserver(ConnectedObserver observer, void* context) {
  if (observer == nullptr) {
    return false;
  }

  bool removed = false;
  portENTER_CRITICAL(&observerMux);
  for (uint8_t i = 0; i < MAX_CONNECTED_OBSERVERS; ++i) {
    ObserverSlot& slot = observerSlots[i];
    if (slot.observer != observer || slot.context != context) {
      continue;
    }

    slot = ObserverSlot();
    removed = true;
    break;
  }
  portEXIT_CRITICAL(&observerMux);
  return removed;
}

}  // namespace WifiConnection
