#include <WiFi.h>
#include <esp_now.h>

typedef struct __attribute__((packed)) {
  uint32_t counter;
  uint8_t payload[8];
} TestPacket;

TestPacket receivedPacket;

// For Arduino-ESP32 2.x
void onDataRecv(const uint8_t *mac, const uint8_t *incomingData, int len) {
  Serial.println();
  Serial.println("[RECV] Packet received");

  Serial.print("[RECV] From MAC: ");
  for (int i = 0; i < 6; i++) {
    if (mac[i] < 0x10) Serial.print("0");
    Serial.print(mac[i], HEX);
    if (i < 5) Serial.print(":");
  }
  Serial.println();

  Serial.print("[RECV] Length: ");
  Serial.println(len);

  if (len != sizeof(TestPacket)) {
    Serial.println("[RECV] Unexpected packet size");
    return;
  }

  memcpy(&receivedPacket, incomingData, sizeof(receivedPacket));

  Serial.print("[RECV] Counter: ");
  Serial.println(receivedPacket.counter);

  Serial.print("[RECV] Payload: ");
  for (int i = 0; i < 8; i++) {
    Serial.print("0x");
    if (receivedPacket.payload[i] < 0x10) Serial.print("0");
    Serial.print(receivedPacket.payload[i], HEX);
    Serial.print(" ");
  }
  Serial.println();
}

void setup() {
  Serial.begin(115200);
  delay(1000);

  WiFi.mode(WIFI_STA);

  Serial.println();
  Serial.println("=== ESP-NOW RECEIVER ===");
  Serial.print("[INFO] My MAC: ");
  Serial.println(WiFi.macAddress());

  if (esp_now_init() != ESP_OK) {
    Serial.println("[ERROR] ESP-NOW init failed");
    return;
  }

  esp_now_register_recv_cb(onDataRecv);

  Serial.println("[INFO] Receiver ready. Waiting for packets...");
}

void loop() {
  delay(2000);
  
}