#include <Arduino.h>
#include <WiFi.h>

// Replace with the same credentials used by the main project.
constexpr const char* WIFI_SSID = "Adi";
constexpr const char* WIFI_PASSWORD = "0502092099";

constexpr unsigned long WIFI_TIMEOUT_MS = 20000;

const char* wifiStatusName(wl_status_t status) {
  switch (status) {
    case WL_IDLE_STATUS:
      return "IDLE";
    case WL_NO_SSID_AVAIL:
      return "SSID_NOT_FOUND";
    case WL_SCAN_COMPLETED:
      return "SCAN_COMPLETED";
    case WL_CONNECTED:
      return "CONNECTED";
    case WL_CONNECT_FAILED:
      return "CONNECT_FAILED";
    case WL_CONNECTION_LOST:
      return "CONNECTION_LOST";
    case WL_DISCONNECTED:
      return "DISCONNECTED";
    default:
      return "UNKNOWN";
  }
}

void printNetworkInfo() {
  Serial.println();
  Serial.println("========== WIFI CONNECTED ==========");
  Serial.print("SSID: ");
  Serial.println(WiFi.SSID());

  Serial.print("Local IP: ");
  Serial.println(WiFi.localIP());

  Serial.print("Gateway: ");
  Serial.println(WiFi.gatewayIP());

  Serial.print("Subnet mask: ");
  Serial.println(WiFi.subnetMask());

  Serial.print("DNS server: ");
  Serial.println(WiFi.dnsIP());

  Serial.print("Signal strength: ");
  Serial.print(WiFi.RSSI());
  Serial.println(" dBm");

  Serial.print("ESP32 MAC address: ");
  Serial.println(WiFi.macAddress());
  Serial.println("====================================");
}

void testDns() {
  constexpr const char* HOSTNAME =
      "walki-talkie-37ed3-default-rtdb.europe-west1.firebasedatabase.app";

  Serial.println();
  Serial.print("[DNS] Resolving ");
  Serial.println(HOSTNAME);

  IPAddress resolvedIp;

  if (WiFi.hostByName(HOSTNAME, resolvedIp)) {
    Serial.print("[DNS] Success: ");
    Serial.println(resolvedIp);
  } else {
    Serial.println("[DNS] FAILED");
    Serial.println(
        "The ESP32 is connected to Wi-Fi, but DNS or internet access may not work.");
  }
}

void setup() {
  Serial.begin(115200);
  delay(1000);

  Serial.println();
  Serial.println("ESP32 Wi-Fi connection test");
  Serial.print("Connecting to SSID: ");
  Serial.println(WIFI_SSID);

  WiFi.mode(WIFI_STA);
  WiFi.setSleep(false);
  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);

  const unsigned long startTime = millis();
  wl_status_t previousStatus = WL_IDLE_STATUS;

  while (WiFi.status() != WL_CONNECTED &&
         millis() - startTime < WIFI_TIMEOUT_MS) {
    const wl_status_t currentStatus = WiFi.status();

    if (currentStatus != previousStatus) {
      Serial.print("[WiFi] Status: ");
      Serial.print(wifiStatusName(currentStatus));
      Serial.print(" (");
      Serial.print(static_cast<int>(currentStatus));
      Serial.println(")");

      previousStatus = currentStatus;
    }

    Serial.print(".");
    delay(500);
  }

  Serial.println();

  if (WiFi.status() != WL_CONNECTED) {
    Serial.println("========== WIFI CONNECTION FAILED ==========");
    Serial.print("Final status: ");
    Serial.print(wifiStatusName(WiFi.status()));
    Serial.print(" (");
    Serial.print(static_cast<int>(WiFi.status()));
    Serial.println(")");

    Serial.println("Check:");
    Serial.println("- SSID spelling");
    Serial.println("- Wi-Fi password");
    Serial.println("- That the network is 2.4 GHz");
    Serial.println("- That the hotspot/router allows new devices");
    Serial.println("============================================");
    return;
  }

  printNetworkInfo();
  testDns();
}

void loop() {
  static wl_status_t previousStatus = WL_CONNECTED;
  const wl_status_t currentStatus = WiFi.status();

  if (currentStatus != previousStatus) {
    Serial.println();
    Serial.print("[WiFi] Status changed to: ");
    Serial.println(wifiStatusName(currentStatus));
    previousStatus = currentStatus;
  }

  if (currentStatus == WL_CONNECTED) {
    Serial.print("[WiFi] Connected | IP=");
    Serial.print(WiFi.localIP());
    Serial.print(" | RSSI=");
    Serial.print(WiFi.RSSI());
    Serial.println(" dBm");
  } else {
    Serial.print("[WiFi] Not connected | status=");
    Serial.println(wifiStatusName(currentStatus));
  }

  delay(5000);
}
