#include <WiFi.h>
#include <esp_now.h>

#define BUTTON_PIN 14

// CHANGE THIS to the receiver ESP's MAC address.
uint8_t receiverMac[] = {0xF4, 0x65, 0x0B, 0xE9, 0x3B, 0x64};

typedef struct __attribute__((packed)) {
  uint32_t counter;
  uint8_t payload[8];
} TestPacket;

uint32_t counter = 0;

bool lastButtonState = HIGH;
unsigned long lastDebounceTime = 0;
const unsigned long debounceDelayMs = 50;

void onDataSent(const uint8_t *mac_addr, esp_now_send_status_t status) {
  Serial.print("[SEND CALLBACK] Status: ");
  Serial.println(status == ESP_NOW_SEND_SUCCESS ? "SUCCESS" : "FAILED");
}

void sendTestPacket() {
  TestPacket packet;

  packet.counter = counter++;

  packet.payload[0] = 0x11;
  packet.payload[1] = 0x22;
  packet.payload[2] = 0x33;
  packet.payload[3] = 0x44;
  packet.payload[4] = 0x55;
  packet.payload[5] = 0x66;
  packet.payload[6] = 0x77;
  packet.payload[7] = 0x88;

  Serial.println();
  Serial.println("[BUTTON] Press detected");

  Serial.print("[SEND] Counter: ");
  Serial.println(packet.counter);

  Serial.print("[SEND] Payload: ");
  for (int i = 0; i < 8; i++) {
    Serial.print("0x");
    if (packet.payload[i] < 0x10) Serial.print("0");
    Serial.print(packet.payload[i], HEX);
    Serial.print(" ");
  }
  Serial.println();

  esp_err_t result = esp_now_send(receiverMac, (uint8_t *)&packet, sizeof(packet));

  if (result == ESP_OK) {
    Serial.println("[SEND] Send request queued");
  } else {
    Serial.print("[SEND] esp_now_send error: ");
    Serial.println(result);
  }
}

void setup() {
  Serial.begin(115200);
  delay(1000);

  pinMode(BUTTON_PIN, INPUT_PULLUP);

  WiFi.mode(WIFI_STA);

  Serial.println();
  Serial.println("=== ESP-NOW BUTTON SENDER ===");
  Serial.print("[INFO] My MAC: ");
  Serial.println(WiFi.macAddress());

  if (esp_now_init() != ESP_OK) {
    Serial.println("[ERROR] ESP-NOW init failed");
    return;
  }

  esp_now_register_send_cb(onDataSent);

  esp_now_peer_info_t peerInfo = {};
  memcpy(peerInfo.peer_addr, receiverMac, 6);
  peerInfo.channel = 0;
  peerInfo.encrypt = false;

  if (esp_now_add_peer(&peerInfo) != ESP_OK) {
    Serial.println("[ERROR] Failed to add receiver peer");
    return;
  }

  Serial.println("[INFO] Receiver peer added");
  Serial.println("[INFO] Press the button to send bytes");
}

void loop() {
  bool currentButtonState = digitalRead(BUTTON_PIN);

  // Detect button press: HIGH -> LOW
  if (lastButtonState == HIGH && currentButtonState == LOW) {
    unsigned long now = millis();

    if (now - lastDebounceTime > debounceDelayMs) {
      lastDebounceTime = now;
      sendTestPacket();
    }
  }

  lastButtonState = currentButtonState;
}