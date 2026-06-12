#include <Arduino.h>
#include <WiFi.h>
#include <esp_now.h>
#include <driver/i2s.h>
#include <esp_err.h>

// =====================================================
// ESP-NOW RECORD THEN PLAY - RECEIVER
//
// Receives audio packets from sender and plays them
// through I2S speaker amp.
// =====================================================

// ---------------- Audio settings ----------------
#define SAMPLE_RATE                 8000
#define AUDIO_SAMPLES_PER_PACKET    100

// ---------------- Speaker pins ----------------
// Same as your working speaker setup
#define I2S_SPK_PORT                I2S_NUM_1
#define I2S_SPK_BCLK                GPIO_NUM_27
#define I2S_SPK_WS                  GPIO_NUM_26
#define I2S_SPK_SD                  GPIO_NUM_22

enum PacketType : uint8_t {
  PACKET_START = 1,
  PACKET_DATA  = 2,
  PACKET_END   = 3
};

typedef struct __attribute__((packed)) {
  uint8_t type;
  uint32_t seq;
  uint16_t sampleCount;
  int16_t samples[AUDIO_SAMPLES_PER_PACKET];
} AudioPacket;

QueueHandle_t audioQueue;

static int16_t stereoBuffer[AUDIO_SAMPLES_PER_PACKET * 2];

uint32_t packetsReceived = 0;
uint32_t packetsPlayed = 0;
uint32_t queueDropped = 0;
uint32_t badSize = 0;
uint32_t missedSeq = 0;

uint32_t lastSeq = 0;
bool haveLastSeq = false;

void printErr(const char *label, esp_err_t err) {
  if (err == ESP_OK) {
    Serial.printf("[OK]   %s\n", label);
  } else {
    Serial.printf("[FAIL] %s: %d / %s\n", label, err, esp_err_to_name(err));
  }
}

void setupSpeaker() {
  Serial.println("[setup] Installing speaker I2S TX driver...");

  i2s_config_t speakerConfig = {
      .mode = (i2s_mode_t)(I2S_MODE_MASTER | I2S_MODE_TX),
      .sample_rate = SAMPLE_RATE,
      .bits_per_sample = I2S_BITS_PER_SAMPLE_16BIT,
      .channel_format = I2S_CHANNEL_FMT_RIGHT_LEFT,
      .communication_format = (i2s_comm_format_t)(I2S_COMM_FORMAT_I2S | I2S_COMM_FORMAT_I2S_MSB),
      .intr_alloc_flags = ESP_INTR_FLAG_LEVEL1,
      .dma_buf_count = 8,
      .dma_buf_len = 64,
      .use_apll = false,
      .tx_desc_auto_clear = true,
      .fixed_mclk = 0
  };

  i2s_pin_config_t speakerPins = {
      .bck_io_num = I2S_SPK_BCLK,
      .ws_io_num = I2S_SPK_WS,
      .data_out_num = I2S_SPK_SD,
      .data_in_num = I2S_PIN_NO_CHANGE
  };

  esp_err_t err = i2s_driver_install(I2S_SPK_PORT, &speakerConfig, 0, NULL);
  printErr("speaker i2s_driver_install", err);

  err = i2s_set_pin(I2S_SPK_PORT, &speakerPins);
  printErr("speaker i2s_set_pin", err);

  err = i2s_set_clk(I2S_SPK_PORT, SAMPLE_RATE, I2S_BITS_PER_SAMPLE_16BIT, I2S_CHANNEL_STEREO);
  printErr("speaker i2s_set_clk", err);

  i2s_zero_dma_buffer(I2S_SPK_PORT);
}

void resetReceiveState() {
  haveLastSeq = false;
  missedSeq = 0;
  xQueueReset(audioQueue);
  i2s_zero_dma_buffer(I2S_SPK_PORT);
}

void handleSequence(uint32_t seq) {
  if (haveLastSeq) {
    if (seq != lastSeq + 1) {
      if (seq > lastSeq + 1) {
        missedSeq += seq - (lastSeq + 1);
      }
    }
  }

  lastSeq = seq;
  haveLastSeq = true;
}

void handleReceivedPacket(const uint8_t *incomingData, int len) {
  if (len != sizeof(AudioPacket)) {
    badSize++;
    return;
  }

  AudioPacket packet;
  memcpy(&packet, incomingData, sizeof(packet));

  packetsReceived++;

  if (packet.type == PACKET_START) {
    Serial.println();
    Serial.println("========== RECEIVING RECORDING ==========");
    Serial.printf("[recv] START seq=%u\n", packet.seq);

    resetReceiveState();
    handleSequence(packet.seq);
    return;
  }

  if (packet.type == PACKET_END) {
    Serial.printf("[recv] END seq=%u\n", packet.seq);
    handleSequence(packet.seq);

    Serial.printf("[recv] packetsReceived=%u played=%u queueDropped=%u badSize=%u missedSeq=%u\n",
                  packetsReceived,
                  packetsPlayed,
                  queueDropped,
                  badSize,
                  missedSeq);

    Serial.println("=========================================");
    return;
  }

  if (packet.type != PACKET_DATA) {
    return;
  }

  handleSequence(packet.seq);

  BaseType_t ok = xQueueSend(audioQueue, &packet, 0);
  if (ok != pdTRUE) {
    queueDropped++;
  }
}

// Arduino-ESP32 2.x callback
void onDataRecv(const uint8_t *mac, const uint8_t *incomingData, int len) {
  handleReceivedPacket(incomingData, len);
}

/*
If this callback does not compile on your Arduino-ESP32 version,
comment out the callback above and use this one instead:

void onDataRecv(const esp_now_recv_info_t *info, const uint8_t *incomingData, int len) {
  handleReceivedPacket(incomingData, len);
}
*/

void playPacket(const AudioPacket *packet) {
  uint16_t count = packet->sampleCount;

  if (count > AUDIO_SAMPLES_PER_PACKET) {
    count = AUDIO_SAMPLES_PER_PACKET;
  }

  for (int i = 0; i < count; i++) {
    int16_t sample = packet->samples[i];

    // Mono -> stereo
    stereoBuffer[2 * i] = sample;
    stereoBuffer[2 * i + 1] = sample;
  }

  size_t bytesToWrite = count * 2 * sizeof(int16_t);
  size_t bytesWritten = 0;

  esp_err_t err = i2s_write(I2S_SPK_PORT,
                            stereoBuffer,
                            bytesToWrite,
                            &bytesWritten,
                            portMAX_DELAY);

  if (err == ESP_OK) {
    packetsPlayed++;
  }
}

void setup() {
  Serial.begin(115200);
  delay(1000);

  Serial.println();
  Serial.println("=== ESP-NOW RECORD THEN PLAY - RECEIVER ===");

  WiFi.mode(WIFI_STA);

  Serial.print("[INFO] My MAC: ");
  Serial.println(WiFi.macAddress());

  Serial.printf("[INFO] AudioPacket size: %u bytes\n", (unsigned int)sizeof(AudioPacket));

  setupSpeaker();

  audioQueue = xQueueCreate(30, sizeof(AudioPacket));
  if (audioQueue == NULL) {
    Serial.println("[ERROR] Failed to create audio queue");
    return;
  }

  if (esp_now_init() != ESP_OK) {
    Serial.println("[ERROR] ESP-NOW init failed");
    return;
  }

  esp_now_register_recv_cb(onDataRecv);

  Serial.println("[READY] Waiting for recording packets...");
}

void loop() {
  static unsigned long lastStatsMs = 0;

  AudioPacket packet;

  if (xQueueReceive(audioQueue, &packet, pdMS_TO_TICKS(10)) == pdTRUE) {
    playPacket(&packet);
  }

  unsigned long now = millis();

  if (now - lastStatsMs >= 1000) {
    lastStatsMs = now;

    Serial.printf("[STATS] received=%u played=%u queueDropped=%u badSize=%u missedSeq=%u queueWaiting=%u\n",
                  packetsReceived,
                  packetsPlayed,
                  queueDropped,
                  badSize,
                  missedSeq,
                  (unsigned int)uxQueueMessagesWaiting(audioQueue));
  }
}