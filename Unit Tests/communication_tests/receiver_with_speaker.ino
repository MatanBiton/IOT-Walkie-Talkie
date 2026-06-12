#include <WiFi.h>
#include <esp_now.h>
#include <driver/i2s.h>
#include <esp_err.h>

#define SAMPLE_RATE 16000
#define AUDIO_SAMPLES_PER_PACKET 100

#define I2S_SPK_PORT I2S_NUM_1
#define I2S_SPK_BCLK GPIO_NUM_27
#define I2S_SPK_WS   GPIO_NUM_26
#define I2S_SPK_SD   GPIO_NUM_22

typedef struct __attribute__((packed)) {
  uint32_t seq;
  uint16_t sampleCount;
  int16_t samples[AUDIO_SAMPLES_PER_PACKET];
} AudioPacket;

QueueHandle_t audioQueue;

uint32_t packetsReceived = 0;
uint32_t packetsPlayed = 0;
uint32_t packetsDroppedQueueFull = 0;
uint32_t badSizePackets = 0;
uint32_t lastSeq = 0;
uint32_t missedPackets = 0;
bool haveLastSeq = false;

static int16_t stereoBuffer[AUDIO_SAMPLES_PER_PACKET * 2];

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

// Arduino-ESP32 2.x callback.
// If this does not compile, tell me the exact error and we’ll switch to the 3.x signature.
void onDataRecv(const uint8_t *mac, const uint8_t *incomingData, int len) {
  if (len != sizeof(AudioPacket)) {
    badSizePackets++;
    return;
  }

  AudioPacket packet;
  memcpy(&packet, incomingData, sizeof(packet));

  packetsReceived++;

  if (haveLastSeq) {
    if (packet.seq != lastSeq + 1) {
      missedPackets += packet.seq - (lastSeq + 1);
    }
  }

  lastSeq = packet.seq;
  haveLastSeq = true;

  BaseType_t ok = xQueueSend(audioQueue, &packet, 0);
  if (ok != pdTRUE) {
    packetsDroppedQueueFull++;
  }
}

void playPacket(const AudioPacket *packet) {
  uint16_t count = packet->sampleCount;

  if (count > AUDIO_SAMPLES_PER_PACKET) {
    count = AUDIO_SAMPLES_PER_PACKET;
  }

  for (int i = 0; i < count; i++) {
    int16_t sample = packet->samples[i];

    // duplicate mono sample to left and right
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
  Serial.println("=== ESP-NOW FAKE AUDIO RECEIVER + SPEAKER ===");

  WiFi.mode(WIFI_STA);

  Serial.print("[INFO] My MAC: ");
  Serial.println(WiFi.macAddress());

  Serial.print("[INFO] AudioPacket size: ");
  Serial.print(sizeof(AudioPacket));
  Serial.println(" bytes");

  setupSpeaker();

  audioQueue = xQueueCreate(20, sizeof(AudioPacket));
  if (audioQueue == NULL) {
    Serial.println("[ERROR] Failed to create audio queue");
    return;
  }

  if (esp_now_init() != ESP_OK) {
    Serial.println("[ERROR] ESP-NOW init failed");
    return;
  }

  esp_now_register_recv_cb(onDataRecv);

  Serial.println("[INFO] Ready. Waiting for fake audio packets...");
}

void loop() {
  static unsigned long lastStatsMillis = 0;

  AudioPacket packet;

  if (xQueueReceive(audioQueue, &packet, pdMS_TO_TICKS(10)) == pdTRUE) {
    playPacket(&packet);
  }

  unsigned long now = millis();
  if (now - lastStatsMillis >= 1000) {
    lastStatsMillis = now;

    Serial.print("[STATS] received=");
    Serial.print(packetsReceived);
    Serial.print(" played=");
    Serial.print(packetsPlayed);
    Serial.print(" queueDropped=");
    Serial.print(packetsDroppedQueueFull);
    Serial.print(" badSize=");
    Serial.print(badSizePackets);
    Serial.print(" missedSeq=");
    Serial.println(missedPackets);
  }
}