#include "wifi_connection.h"

#include <Arduino.h>
#include <WiFi.h>

#include "app_config.h"

namespace {
unsigned long lastReconnectAttemptMs = 0;
}

namespace WifiConnection {

bool isConnected() {
  return WiFi.status() == WL_CONNECTED;
}

bool begin() {
  WiFi.mode(WIFI_STA);
  WiFi.setSleep(false);
  WiFi.begin(WifiConfig::SSID, WifiConfig::PASSWORD);

  Serial.print("[WiFi] Connecting");
  const unsigned long startMs = millis();
  while (!isConnected() && (millis() - startMs) < WifiConfig::CONNECT_TIMEOUT_MS) {
    Serial.print('.');
    delay(250);
  }
  Serial.println();

  if (!isConnected()) {
    Serial.println("[WiFi] Initial connection failed");
    return false;
  }

  Serial.print("[WiFi] Connected, IP=");
  Serial.println(WiFi.localIP());
  return true;
}

bool ensureConnected() {
  if (isConnected()) {
    return true;
  }

  const unsigned long now = millis();
  if ((now - lastReconnectAttemptMs) < WifiConfig::RECONNECT_INTERVAL_MS) {
    return false;
  }
  lastReconnectAttemptMs = now;

  Serial.println("[WiFi] Reconnecting...");
  WiFi.disconnect(false);
  WiFi.begin(WifiConfig::SSID, WifiConfig::PASSWORD);
  return isConnected();
}

}  // namespace WifiConnection
