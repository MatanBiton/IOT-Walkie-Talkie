#include <Arduino.h>
#include <WiFi.h>
#include <esp_now.h>
#include <esp_wifi.h>
#include <driver/i2s.h>
#include <esp_err.h>

#if __has_include(<esp_arduino_version.h>)
  #include <esp_arduino_version.h>
#endif
#ifndef ESP_ARDUINO_VERSION_MAJOR
  #define ESP_ARDUINO_VERSION_MAJOR 2
#endif

// =====================================================
// ESP-NOW REAL-TIME TWO-WAY PUSH-TO-TALK WALKIE-TALKIE
//
// Flash this same sketch to BOTH ESP32 boards.
// On each board, set peerMac[] to the OTHER ESP32's WiFi STA MAC.
//
// Behavior:
//   Hold button  -> stream microphone audio to the peer in real time
//   Release      -> stop transmitting
//   Not pressed  -> receive and play peer audio
//
// Assumption: both boards do not press the transmit button at the same time.
// Transport: ESP-NOW only. No WiFi router, no UDP.
// =====================================================

// ---------------- ESP-NOW settings ----------------
// Both ESPs must use the same ESP-NOW channel.
#define ESPNOW_CHANNEL              1

// Replace this on EACH board with the OTHER board's MAC address.
// Example below is the receiver MAC from your previous sender sketch.
uint8_t peerMac[] = {0xF4, 0x65, 0x0B, 0xE9, 0x3B, 0x64};

// Set to 1 only if you want to avoid configuring peer MACs.
// Unicast peer MACs are preferred because send callbacks can report delivery status.
#define USE_BROADCAST_PEER          1
uint8_t broadcastMac[] = {0xff, 0xff, 0xff, 0xff, 0xff, 0xff};

// ---------------- Audio settings ----------------
// 8 kHz keeps ESP-NOW bandwidth low and matches your working sketches.
#define SAMPLE_RATE                 8000
#define AUDIO_SAMPLES_PER_PACKET    100       // 100 samples = 12.5 ms at 8 kHz
#define MIC_READ_SAMPLES            AUDIO_SAMPLES_PER_PACKET

// Your working mic code used raw >> 12 and gain 3 after DC removal.
#define MIC_SHIFT                   12
#define MIC_GAIN                    3
#define SPEAKER_GAIN                1

// Queue depth trades latency vs resilience to brief WiFi jitter.
// 12 packets ~= 150 ms max queue at 8 kHz. Lower = less latency; higher = fewer drops.
#define RX_QUEUE_LEN                12

// ---------------- Button + LED ----------------
#define BUTTON_PIN                  GPIO_NUM_14    // active LOW, INPUT_PULLUP
#define TX_LED_PIN                  GPIO_NUM_4
#define BUTTON_DEBOUNCE_MS          45

// ---------------- I2S microphone pins ----------------
// Same as your working sender_with_microphone.ino
#define I2S_MIC_PORT                I2S_NUM_0
#define I2S_MIC_CHANNEL             I2S_CHANNEL_FMT_ONLY_RIGHT
#define I2S_MIC_BCLK                GPIO_NUM_32
#define I2S_MIC_WS                  GPIO_NUM_25
#define I2S_MIC_SD                  GPIO_NUM_33

// ---------------- I2S speaker pins ----------------
// Same as your working receiver_mic_to_speaker.ino
#define I2S_SPK_PORT                I2S_NUM_1
#define I2S_SPK_BCLK                GPIO_NUM_27
#define I2S_SPK_WS                  GPIO_NUM_26
#define I2S_SPK_SD                  GPIO_NUM_22

// ---------------- Packet format ----------------
// Packet size: 2 + 1 + 1 + 4 + 2 + 200 = 210 bytes, below the classic ESP-NOW 250-byte payload limit.
static const uint16_t PACKET_MAGIC = 0xB17A;

// Use a different stream id when a new push-to-talk burst starts.
// This helps the receiver reset sequence tracking between bursts.
enum PacketType : uint8_t {
  PACKET_START = 1,
  PACKET_AUDIO = 2,
  PACKET_END   = 3
};

typedef struct __attribute__((packed)) {
  uint16_t magic;
  uint8_t type;
  uint8_t streamId;
  uint32_t seq;
  uint16_t sampleCount;
  int16_t samples[AUDIO_SAMPLES_PER_PACKET];
} AudioPacket;

static_assert(sizeof(AudioPacket) <= 250, "AudioPacket is too large for classic ESP-NOW payloads");

// ---------------- Globals ----------------
QueueHandle_t rxQueue = nullptr;

static int32_t micRaw[MIC_READ_SAMPLES];
static int16_t stereoBuffer[AUDIO_SAMPLES_PER_PACKET * 2];

volatile bool isTransmitting = false;
bool remoteStreamActive = false;

uint8_t streamId = 0;
uint32_t txSeq = 0;
int32_t dcEstimate = 0;

uint32_t txPacketsQueued = 0;
uint32_t txSendErrors = 0;
uint32_t txSendOkCallbacks = 0;
uint32_t txSendFailCallbacks = 0;

uint32_t rxPacketsQueued = 0;
uint32_t rxPacketsPlayed = 0;
uint32_t rxQueueDropped = 0;
uint32_t rxBadPackets = 0;
uint32_t rxMissedSeq = 0;
uint32_t rxLastSeq = 0;
uint8_t rxLastStreamId = 0;
bool rxHaveLastSeq = false;

bool lastRawPressed = false;
bool stablePressed = false;
uint32_t lastButtonChangeMs = 0;

static int16_t clampToInt16(int32_t value) {
  if (value > 32767) return 32767;
  if (value < -32768) return -32768;
  return (int16_t)value;
}

void printErr(const char *label, esp_err_t err) {
  if (err == ESP_OK) {
    Serial.printf("[OK]   %s\n", label);
  } else {
    Serial.printf("[FAIL] %s: %d / %s\n", label, err, esp_err_to_name(err));
  }
}

void printMac(const uint8_t *mac) {
  Serial.printf("%02X:%02X:%02X:%02X:%02X:%02X",
                mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
}

void setupMic() {
  Serial.println("[setup] Installing microphone I2S RX driver...");

  i2s_config_t micConfig = {
      .mode = (i2s_mode_t)(I2S_MODE_MASTER | I2S_MODE_RX),
      .sample_rate = SAMPLE_RATE,
      .bits_per_sample = I2S_BITS_PER_SAMPLE_32BIT,
      .channel_format = I2S_MIC_CHANNEL,
      .communication_format = I2S_COMM_FORMAT_I2S,
      .intr_alloc_flags = ESP_INTR_FLAG_LEVEL1,
      .dma_buf_count = 4,
      .dma_buf_len = 128,
      .use_apll = false,
      .tx_desc_auto_clear = false,
      .fixed_mclk = 0
  };

  i2s_pin_config_t micPins = {
      .bck_io_num = I2S_MIC_BCLK,
      .ws_io_num = I2S_MIC_WS,
      .data_out_num = I2S_PIN_NO_CHANGE,
      .data_in_num = I2S_MIC_SD
  };

  esp_err_t err = i2s_driver_install(I2S_MIC_PORT, &micConfig, 0, NULL);
  printErr("mic i2s_driver_install", err);

  err = i2s_set_pin(I2S_MIC_PORT, &micPins);
  printErr("mic i2s_set_pin", err);

  err = i2s_set_clk(I2S_MIC_PORT, SAMPLE_RATE, I2S_BITS_PER_SAMPLE_32BIT, I2S_CHANNEL_MONO);
  printErr("mic i2s_set_clk", err);

  i2s_zero_dma_buffer(I2S_MIC_PORT);
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

void onDataSent(const uint8_t *mac_addr, esp_now_send_status_t status) {
  if (status == ESP_NOW_SEND_SUCCESS) {
    txSendOkCallbacks++;
  } else {
    txSendFailCallbacks++;
  }
}

void queueReceivedPacket(const uint8_t *incomingData, int len) {
  if (len != sizeof(AudioPacket)) {
    rxBadPackets++;
    return;
  }

  AudioPacket packet;
  memcpy(&packet, incomingData, sizeof(packet));

  if (packet.magic != PACKET_MAGIC) {
    rxBadPackets++;
    return;
  }

  if (packet.sampleCount > AUDIO_SAMPLES_PER_PACKET) {
    rxBadPackets++;
    return;
  }

  // Local transmit wins in this half-duplex design.
  // This also avoids speaker feedback while your mic is open.
  if (isTransmitting) {
    return;
  }

  if (rxQueue == nullptr) {
    return;
  }

  BaseType_t ok = xQueueSend(rxQueue, &packet, 0);
  if (ok != pdTRUE) {
    // Keep latency bounded: drop the oldest packet, then queue the newest one.
    AudioPacket oldPacket;
    xQueueReceive(rxQueue, &oldPacket, 0);
    if (xQueueSend(rxQueue, &packet, 0) != pdTRUE) {
      rxQueueDropped++;
    } else {
      rxQueueDropped++;
      rxPacketsQueued++;
    }
  } else {
    rxPacketsQueued++;
  }
}

#if ESP_ARDUINO_VERSION_MAJOR >= 3
void onDataRecv(const esp_now_recv_info_t *info, const uint8_t *incomingData, int len) {
  queueReceivedPacket(incomingData, len);
}
#else
void onDataRecv(const uint8_t *mac, const uint8_t *incomingData, int len) {
  queueReceivedPacket(incomingData, len);
}
#endif

bool sendAudioPacket(const AudioPacket *packet) {
#if USE_BROADCAST_PEER
  const uint8_t *dest = broadcastMac;
#else
  const uint8_t *dest = peerMac;
#endif

  esp_err_t result = esp_now_send(dest, (const uint8_t *)packet, sizeof(AudioPacket));
  if (result == ESP_OK) {
    txPacketsQueued++;
    return true;
  }

  txSendErrors++;
  return false;
}

void sendControlPacket(PacketType type) {
  AudioPacket packet = {};
  packet.magic = PACKET_MAGIC;
  packet.type = type;
  packet.streamId = streamId;
  packet.seq = txSeq++;
  packet.sampleCount = 0;
  sendAudioPacket(&packet);
}

void resetRxPlayback() {
  remoteStreamActive = false;
  rxHaveLastSeq = false;
  if (rxQueue != nullptr) {
    xQueueReset(rxQueue);
  }
  i2s_zero_dma_buffer(I2S_SPK_PORT);
}

void startTransmit() {
  Serial.println();
  Serial.println("========== PTT START ==========");
  Serial.println("[TX] Streaming microphone over ESP-NOW...");

  isTransmitting = true;
  digitalWrite(TX_LED_PIN, HIGH);

  resetRxPlayback();
  i2s_zero_dma_buffer(I2S_MIC_PORT);

  streamId++;
  txSeq = 0;
  dcEstimate = 0;

  txPacketsQueued = 0;
  txSendErrors = 0;
  txSendOkCallbacks = 0;
  txSendFailCallbacks = 0;

  sendControlPacket(PACKET_START);
}

void stopTransmit() {
  sendControlPacket(PACKET_END);
  delay(5);

  isTransmitting = false;
  digitalWrite(TX_LED_PIN, LOW);

  Serial.println("[TX] Stop.");
  Serial.printf("[TX] queued=%u sendErrors=%u cbOK=%u cbFail=%u\n",
                txPacketsQueued,
                txSendErrors,
                txSendOkCallbacks,
                txSendFailCallbacks);
  Serial.println("===============================");
}

void transmitAudioStep() {
  size_t bytesRead = 0;

  esp_err_t err = i2s_read(I2S_MIC_PORT,
                           micRaw,
                           sizeof(micRaw),
                           &bytesRead,
                           pdMS_TO_TICKS(25));

  if (err != ESP_OK || bytesRead == 0) {
    return;
  }

  uint16_t samplesRead = bytesRead / sizeof(int32_t);
  if (samplesRead > AUDIO_SAMPLES_PER_PACKET) {
    samplesRead = AUDIO_SAMPLES_PER_PACKET;
  }

  AudioPacket packet = {};
  packet.magic = PACKET_MAGIC;
  packet.type = PACKET_AUDIO;
  packet.streamId = streamId;
  packet.seq = txSeq++;
  packet.sampleCount = samplesRead;

  for (uint16_t i = 0; i < samplesRead; i++) {
    // Convert 32-bit I2S mic sample to 16-bit PCM, remove DC offset online, then amplify.
    int32_t shifted = micRaw[i] >> MIC_SHIFT;
    int32_t centered = shifted - dcEstimate;
    dcEstimate += centered >> 8;  // slow moving average high-pass filter

    int32_t amplified = centered * MIC_GAIN;
    packet.samples[i] = clampToInt16(amplified);
  }

  sendAudioPacket(&packet);
}

bool buttonChanged(bool &pressedNow) {
  bool rawPressed = (digitalRead(BUTTON_PIN) == LOW);
  uint32_t now = millis();

  if (rawPressed != lastRawPressed) {
    lastRawPressed = rawPressed;
    lastButtonChangeMs = now;
  }

  if ((now - lastButtonChangeMs) >= BUTTON_DEBOUNCE_MS && rawPressed != stablePressed) {
    stablePressed = rawPressed;
    pressedNow = stablePressed;
    return true;
  }

  return false;
}

void handleRxSequence(const AudioPacket &packet) {
  if (!rxHaveLastSeq || packet.streamId != rxLastStreamId) {
    rxLastStreamId = packet.streamId;
    rxLastSeq = packet.seq;
    rxHaveLastSeq = true;
    return;
  }

  if (packet.seq != rxLastSeq + 1) {
    if (packet.seq > rxLastSeq + 1) {
      rxMissedSeq += packet.seq - (rxLastSeq + 1);
    }
  }

  rxLastSeq = packet.seq;
}

void playAudioPacket(const AudioPacket &packet) {
  uint16_t count = packet.sampleCount;
  if (count > AUDIO_SAMPLES_PER_PACKET) {
    count = AUDIO_SAMPLES_PER_PACKET;
  }

  for (uint16_t i = 0; i < count; i++) {
    int32_t amplified = (int32_t)packet.samples[i] * SPEAKER_GAIN;
    int16_t sample = clampToInt16(amplified);

    // Mono -> stereo for MAX98357A / I2S amplifier style output.
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

  if (err == ESP_OK && bytesWritten == bytesToWrite) {
    rxPacketsPlayed++;
  }
}

void receiveAndPlayStep() {
  if (rxQueue == nullptr) {
    return;
  }

  AudioPacket packet;
  if (xQueueReceive(rxQueue, &packet, pdMS_TO_TICKS(2)) != pdTRUE) {
    return;
  }

  if (packet.type == PACKET_START) {
    remoteStreamActive = true;
    rxHaveLastSeq = false;
    rxLastStreamId = packet.streamId;
    i2s_zero_dma_buffer(I2S_SPK_PORT);
    handleRxSequence(packet);
    Serial.println();
    Serial.println("[RX] Remote PTT start");
    return;
  }

  if (packet.type == PACKET_END) {
    handleRxSequence(packet);
    remoteStreamActive = false;
    i2s_zero_dma_buffer(I2S_SPK_PORT);
    Serial.println("[RX] Remote PTT stop");
    return;
  }

  if (packet.type != PACKET_AUDIO) {
    return;
  }

  // If START was dropped, still play audio instead of staying silent.
  if (!remoteStreamActive) {
    remoteStreamActive = true;
    rxHaveLastSeq = false;
    rxLastStreamId = packet.streamId;
  }

  handleRxSequence(packet);
  playAudioPacket(packet);
}

void setupEspNow() {
  WiFi.mode(WIFI_STA);
  WiFi.disconnect(false, true);
  WiFi.setSleep(false);

  Serial.print("[INFO] My WiFi STA MAC: ");
  Serial.println(WiFi.macAddress());

  esp_err_t err = esp_wifi_set_channel(ESPNOW_CHANNEL, WIFI_SECOND_CHAN_NONE);
  printErr("esp_wifi_set_channel", err);

  if (esp_now_init() != ESP_OK) {
    Serial.println("[ERROR] ESP-NOW init failed");
    return;
  }

  esp_now_register_send_cb(onDataSent);
  esp_now_register_recv_cb(onDataRecv);

  esp_now_peer_info_t peerInfo = {};
#if USE_BROADCAST_PEER
  memcpy(peerInfo.peer_addr, broadcastMac, 6);
#else
  memcpy(peerInfo.peer_addr, peerMac, 6);
#endif
  peerInfo.channel = ESPNOW_CHANNEL;
  peerInfo.encrypt = false;

  esp_err_t addPeerResult = esp_now_add_peer(&peerInfo);
  printErr("esp_now_add_peer", addPeerResult);

#if USE_BROADCAST_PEER
  Serial.print("[INFO] ESP-NOW destination: broadcast ");
  printMac(broadcastMac);
  Serial.println();
#else
  Serial.print("[INFO] ESP-NOW peer MAC: ");
  printMac(peerMac);
  Serial.println();
#endif
}

void setup() {
  Serial.begin(115200);
  delay(1000);

  pinMode(BUTTON_PIN, INPUT_PULLUP);
  pinMode(TX_LED_PIN, OUTPUT);
  digitalWrite(TX_LED_PIN, LOW);

  Serial.println();
  Serial.println("=== ESP-NOW REAL-TIME TWO-WAY WALKIE-TALKIE ===");
  Serial.printf("[INFO] Sample rate: %d Hz\n", SAMPLE_RATE);
  Serial.printf("[INFO] Samples per packet: %d\n", AUDIO_SAMPLES_PER_PACKET);
  Serial.printf("[INFO] AudioPacket size: %u bytes\n", (unsigned int)sizeof(AudioPacket));
  Serial.printf("[INFO] Button GPIO: %d, TX LED GPIO: %d\n", (int)BUTTON_PIN, (int)TX_LED_PIN);

  rxQueue = xQueueCreate(RX_QUEUE_LEN, sizeof(AudioPacket));
  if (rxQueue == nullptr) {
    Serial.println("[ERROR] Failed to create RX queue");
    while (true) delay(1000);
  }

  setupMic();
  setupSpeaker();
  setupEspNow();

  Serial.println("[READY] Hold button to talk. Release to listen.");
}

void loop() {
  bool pressedNow;
  if (buttonChanged(pressedNow)) {
    if (pressedNow && !isTransmitting) {
      startTransmit();
    } else if (!pressedNow && isTransmitting) {
      stopTransmit();
    }
  }

  if (isTransmitting) {
    transmitAudioStep();
  } else {
    receiveAndPlayStep();
  }

  static uint32_t lastStatsMs = 0;
  uint32_t now = millis();
  if (now - lastStatsMs >= 3000) {
    lastStatsMs = now;
    Serial.printf("[STATS] txQueued=%u txErr=%u txCbOK=%u txCbFail=%u | rxQueued=%u rxPlayed=%u rxDropped=%u rxBad=%u rxMissed=%u q=%u\n",
                  txPacketsQueued,
                  txSendErrors,
                  txSendOkCallbacks,
                  txSendFailCallbacks,
                  rxPacketsQueued,
                  rxPacketsPlayed,
                  rxQueueDropped,
                  rxBadPackets,
                  rxMissedSeq,
                  (unsigned int)uxQueueMessagesWaiting(rxQueue));
  }
}
