#include <WiFi.h>
#include <esp_now.h>

#define BUTTON_PIN 14

#define SAMPLE_RATE 16000
#define AUDIO_SAMPLES_PER_PACKET 100
#define PACKET_INTERVAL_US 6250  // 100 samples / 16000 Hz = 6.25 ms

uint8_t receiverMac[] = {0xF4, 0x65, 0x0B, 0xE9, 0x3B, 0x64};

typedef struct __attribute__((packed)) {
  uint32_t seq;
  uint16_t sampleCount;
  int16_t samples[AUDIO_SAMPLES_PER_PACKET];
} AudioPacket;

uint32_t seqCounter = 0;
uint32_t globalSampleIndex = 0;

uint32_t packetsSent = 0;
uint32_t sendErrors = 0;
uint32_t callbacksSuccess = 0;
uint32_t callbacksFail = 0;

unsigned long lastPacketMicros = 0;
unsigned long lastStatsMillis = 0;

void onDataSent(const uint8_t *mac_addr, esp_now_send_status_t status) {
  if (status == ESP_NOW_SEND_SUCCESS) {
    callbacksSuccess++;
  } else {
    callbacksFail++;
  }
}

// Non-sine test sound:
// Mostly silence, with a short soft bipolar "click/thump" twice per second.
// This avoids the scary continuous alarm-like sine tone.
int16_t generateSoftClickSample(uint32_t sampleIndex) {
  const uint32_t periodSamples = SAMPLE_RATE / 2;  // click every 0.5 sec
  const uint32_t clickLength = 320;                // 20 ms at 16 kHz
  const int16_t amplitude = 100000;                  // keep modest volume

  uint32_t pos = sampleIndex % periodSamples;

  if (pos >= clickLength) {
    return 0;
  }

  // Bipolar triangular pulse:
  // first half positive, second half negative.
  uint32_t half = clickLength / 2;

  if (pos < half) {
    // positive triangle: 0 -> amplitude -> 0
    if (pos < half / 2) {
      return (int16_t)((amplitude * pos) / (half / 2));
    } else {
      return (int16_t)((amplitude * (half - pos)) / (half / 2));
    }
  } else {
    uint32_t p = pos - half;

    // negative triangle: 0 -> -amplitude -> 0
    if (p < half / 2) {
      return (int16_t)(-((amplitude * p) / (half / 2)));
    } else {
      return (int16_t)(-((amplitude * (half - p)) / (half / 2)));
    }
  }
}

void fillAudioPacket(AudioPacket *packet) {
  packet->seq = seqCounter++;
  packet->sampleCount = AUDIO_SAMPLES_PER_PACKET;

  for (int i = 0; i < AUDIO_SAMPLES_PER_PACKET; i++) {
    packet->samples[i] = generateSoftClickSample(globalSampleIndex++);
  }
}

void sendAudioPacket() {
  AudioPacket packet;
  fillAudioPacket(&packet);

  esp_err_t result = esp_now_send(receiverMac, (uint8_t *)&packet, sizeof(packet));

  if (result == ESP_OK) {
    packetsSent++;
  } else {
    sendErrors++;
  }
}

void setup() {
  Serial.begin(115200);
  delay(1000);

  pinMode(BUTTON_PIN, INPUT_PULLUP);

  WiFi.mode(WIFI_STA);

  Serial.println();
  Serial.println("=== ESP-NOW FAKE AUDIO STREAM SENDER ===");
  Serial.print("[INFO] My MAC: ");
  Serial.println(WiFi.macAddress());

  Serial.print("[INFO] AudioPacket size: ");
  Serial.print(sizeof(AudioPacket));
  Serial.println(" bytes");

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
  Serial.println("[INFO] Hold button to stream fake non-sine audio");
}

void loop() {
  bool buttonHeld = digitalRead(BUTTON_PIN) == LOW;

  if (buttonHeld) {
    unsigned long nowUs = micros();

    if (nowUs - lastPacketMicros >= PACKET_INTERVAL_US) {
      lastPacketMicros += PACKET_INTERVAL_US;
      sendAudioPacket();
    }
  } else {
    // Reset timing so it does not burst after button is pressed again.
    lastPacketMicros = micros();
  }

  unsigned long nowMs = millis();
  if (nowMs - lastStatsMillis >= 1000) {
    lastStatsMillis = nowMs;

    Serial.print("[STATS] button=");
    Serial.print(buttonHeld ? "HELD" : "released");
    Serial.print(" sent=");
    Serial.print(packetsSent);
    Serial.print(" sendErrors=");
    Serial.print(sendErrors);
    Serial.print(" cbOK=");
    Serial.print(callbacksSuccess);
    Serial.print(" cbFail=");
    Serial.println(callbacksFail);
  }
}