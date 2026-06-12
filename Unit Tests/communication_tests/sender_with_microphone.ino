#include <Arduino.h>
#include <WiFi.h>
#include <esp_now.h>
#include <driver/i2s.h>
#include <esp_err.h>

// =====================================================
// ESP-NOW RECORD THEN SEND - SENDER
//
// 1st button press  -> start recording
// 2nd button press  -> stop recording
// Auto-stop         -> after 5 seconds
// Then sends recorded audio to receiver over ESP-NOW
//
// Recording LED:
// GPIO4 -> resistor -> long LED leg
// short LED leg -> GND
// =====================================================

// ---------------- ESP-NOW receiver MAC ----------------
uint8_t receiverMac[] = {0xF4, 0x65, 0x0B, 0xE9, 0x3B, 0x64};

// ---------------- Audio settings ----------------
// Same as your working mic experiment
#define SAMPLE_RATE                 8000
#define MAX_RECORD_SECONDS          5
#define MAX_SAMPLES                 (SAMPLE_RATE * MAX_RECORD_SECONDS)

#define AUDIO_SAMPLES_PER_PACKET    100
#define PACKET_INTERVAL_US          12500  // 100 samples / 16000 Hz = 6.25 ms

#define MIC_SHIFT                   12
#define RECORD_GAIN                 3

// ---------------- Button + LED ----------------
#define BUTTON_PIN                  GPIO_NUM_14
#define RECORDING_LED_PIN           GPIO_NUM_4

// ---------------- I2S microphone pins ----------------
// Same as your working mic experiment
#define I2S_MIC_PORT                I2S_NUM_0
#define I2S_MIC_CHANNEL             I2S_CHANNEL_FMT_ONLY_RIGHT
#define I2S_MIC_BCLK                GPIO_NUM_32
#define I2S_MIC_WS                  GPIO_NUM_25
#define I2S_MIC_SD                  GPIO_NUM_33

#define MIC_READ_CHUNK              256

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

enum SenderState {
  IDLE,
  RECORDING,
  PROCESSING,
  SENDING
};

SenderState state = IDLE;

// 5 seconds at 16 kHz, 16-bit = 160,000 bytes.
// Allocate dynamically instead of as a huge static global.
int16_t *recording = nullptr;

static int32_t micRaw[MIC_READ_CHUNK];

size_t recordedSamples = 0;
uint32_t recordStartMs = 0;
uint32_t seqCounter = 0;

bool lastButtonState = HIGH;
unsigned long lastDebounceMs = 0;
const unsigned long debounceDelayMs = 80;

uint32_t packetsQueued = 0;
uint32_t sendErrors = 0;
uint32_t sendSuccessCallbacks = 0;
uint32_t sendFailCallbacks = 0;

// Diagnostics
int32_t rawMin = INT32_MAX;
int32_t rawMax = INT32_MIN;
int16_t shiftedMin = INT16_MAX;
int16_t shiftedMax = INT16_MIN;

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

void setRecordingLed(bool on) {
  digitalWrite(RECORDING_LED_PIN, on ? HIGH : LOW);
  Serial.println(on ? "[LED] Recording LED ON" : "[LED] Recording LED OFF");
}

void onDataSent(const uint8_t *mac_addr, esp_now_send_status_t status) {
  if (status == ESP_NOW_SEND_SUCCESS) {
    sendSuccessCallbacks++;
  } else {
    sendFailCallbacks++;
  }
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
      .dma_buf_len = 1024,
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

void setupEspNow() {
  WiFi.mode(WIFI_STA);

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
}

bool buttonPressedEvent() {
  bool currentButtonState = digitalRead(BUTTON_PIN);
  bool pressed = false;

  if (lastButtonState == HIGH && currentButtonState == LOW) {
    unsigned long now = millis();

    if (now - lastDebounceMs > debounceDelayMs) {
      lastDebounceMs = now;
      pressed = true;
    }
  }

  lastButtonState = currentButtonState;
  return pressed;
}

void resetDiagnostics() {
  rawMin = INT32_MAX;
  rawMax = INT32_MIN;
  shiftedMin = INT16_MAX;
  shiftedMax = INT16_MIN;
}

void startRecording() {
  Serial.println();
  Serial.println("========== START RECORDING ==========");
  Serial.println("[STATE] Recording started!");
  Serial.println("[record] Speak now.");

  if (recording == nullptr) {
    Serial.println("[ERROR] recording buffer is NULL");
    setRecordingLed(false);
    state = IDLE;
    return;
  }

  memset(recording, 0, MAX_SAMPLES * sizeof(int16_t));
  i2s_zero_dma_buffer(I2S_MIC_PORT);

  recordedSamples = 0;
  recordStartMs = millis();
  resetDiagnostics();

  setRecordingLed(true);

  state = RECORDING;

  Serial.println("[record] Press button again to stop, or wait 5 seconds.");
}

void recordStep() {
  if (recordedSamples >= MAX_SAMPLES) {
    Serial.println("[record] Reached max 5 seconds.");
    Serial.println("[STATE] Recording finished by max sample limit.");
    state = PROCESSING;
    return;
  }

  size_t bytesRead = 0;

  esp_err_t err = i2s_read(I2S_MIC_PORT,
                           micRaw,
                           sizeof(micRaw),
                           &bytesRead,
                           pdMS_TO_TICKS(20));

  if (err != ESP_OK) {
    printErr("record i2s_read", err);
    return;
  }

  if (bytesRead == 0) {
    return;
  }

  size_t samplesRead = bytesRead / sizeof(int32_t);

  for (size_t i = 0; i < samplesRead && recordedSamples < MAX_SAMPLES; i++) {
    int32_t raw = micRaw[i];

    if (raw < rawMin) rawMin = raw;
    if (raw > rawMax) rawMax = raw;

    int16_t shifted = clampToInt16(raw >> MIC_SHIFT);

    if (shifted < shiftedMin) shiftedMin = shifted;
    if (shifted > shiftedMax) shiftedMax = shifted;

    recording[recordedSamples++] = shifted;
  }

  static uint32_t lastProgressMs = 0;
  uint32_t now = millis();

  if (now - lastProgressMs >= 500) {
    lastProgressMs = now;

    Serial.printf("[record] Samples: %u / %u\n",
                  (unsigned int)recordedSamples,
                  (unsigned int)MAX_SAMPLES);
  }

  if (now - recordStartMs >= MAX_RECORD_SECONDS * 1000) {
    Serial.println("[record] Auto-stop after 5 seconds.");
    Serial.println("[STATE] Recording finished by timeout.");
    state = PROCESSING;
  }
}

void processRecording() {
  Serial.println();
  Serial.println("========== FINISH RECORDING ==========");
  Serial.println("[STATE] Recording finished!");
  setRecordingLed(false);

  Serial.printf("[record] Recorded samples: %u\n", (unsigned int)recordedSamples);
  Serial.printf("[record] Duration approx: %.2f seconds\n", (float)recordedSamples / SAMPLE_RATE);

  if (recordedSamples == 0) {
    Serial.println("[record] No samples recorded. Returning to IDLE.");
    state = IDLE;
    return;
  }

  Serial.println("[diagnostic] Raw / shifted mic ranges before processing:");
  Serial.printf("[diagnostic] rawMin=%ld rawMax=%ld\n", rawMin, rawMax);
  Serial.printf("[diagnostic] shiftedMin=%d shiftedMax=%d\n", shiftedMin, shiftedMax);

  Serial.print("[diagnostic] First 10 shifted samples: ");
  for (int i = 0; i < 10 && i < recordedSamples; i++) {
    Serial.print(recording[i]);
    Serial.print(" ");
  }
  Serial.println();

  // Remove DC offset.
  int64_t sum = 0;
  for (size_t i = 0; i < recordedSamples; i++) {
    sum += recording[i];
  }

  int16_t mean = (int16_t)(sum / recordedSamples);

  int16_t minSample = INT16_MAX;
  int16_t maxSample = INT16_MIN;
  int16_t peak = 0;

  for (size_t i = 0; i < recordedSamples; i++) {
    int32_t fixed = ((int32_t)recording[i] - mean) * RECORD_GAIN;
    recording[i] = clampToInt16(fixed);

    if (recording[i] < minSample) minSample = recording[i];
    if (recording[i] > maxSample) maxSample = recording[i];

    int32_t absSample = abs((int32_t)recording[i]);
    if (absSample > peak) peak = absSample;
  }

  Serial.println("[diagnostic] After DC removal + gain:");
  Serial.printf("[record] Mean removed: %d\n", mean);
  Serial.printf("[record] Final min/max: %d / %d\n", minSample, maxSample);
  Serial.printf("[record] Final peak: %d\n", peak);

  Serial.print("[diagnostic] First 10 final samples: ");
  for (int i = 0; i < 10 && i < recordedSamples; i++) {
    Serial.print(recording[i]);
    Serial.print(" ");
  }
  Serial.println();

  if (peak < 50) {
    Serial.println("[diagnostic] Very quiet recording.");
    Serial.println("[diagnostic] Your old working code used ONLY_RIGHT.");
    Serial.println("[diagnostic] Keep L/R wiring exactly as before.");
    Serial.println("[diagnostic] If it is still silent, try ONLY_LEFT.");
  }

  Serial.println("[STATE] Processing finished. Moving to sending.");
  state = SENDING;
}

bool sendPacket(const AudioPacket *packet) {
  esp_err_t result = esp_now_send(receiverMac, (uint8_t *)packet, sizeof(AudioPacket));

  if (result == ESP_OK) {
    packetsQueued++;
    return true;
  } else {
    sendErrors++;
    Serial.print("[send] esp_now_send error: ");
    Serial.println(result);
    return false;
  }
}

void sendRecording() {
  Serial.println();
  Serial.println("========== SENDING RECORDING ==========");
  Serial.println("[STATE] Sending started.");

  seqCounter = 0;
  packetsQueued = 0;
  sendErrors = 0;
  sendSuccessCallbacks = 0;
  sendFailCallbacks = 0;

  AudioPacket packet = {};

  // START packet
  packet.type = PACKET_START;
  packet.seq = seqCounter++;
  packet.sampleCount = 0;
  sendPacket(&packet);
  delay(20);

  size_t sampleIndex = 0;
  unsigned long nextPacketUs = micros();

  while (sampleIndex < recordedSamples) {
    packet = {};
    packet.type = PACKET_DATA;
    packet.seq = seqCounter++;

    size_t remaining = recordedSamples - sampleIndex;
    size_t count = min((size_t)AUDIO_SAMPLES_PER_PACKET, remaining);

    packet.sampleCount = count;

    for (size_t i = 0; i < count; i++) {
      packet.samples[i] = recording[sampleIndex++];
    }

    sendPacket(&packet);

    // Send at approximately audio speed.
    nextPacketUs += PACKET_INTERVAL_US;
    while ((int32_t)(micros() - nextPacketUs) < 0) {
      delayMicroseconds(50);
    }

    if (packet.seq % 50 == 0) {
      Serial.printf("[send] Sent packet seq=%u, samples=%u / %u\n",
                    packet.seq,
                    (unsigned int)sampleIndex,
                    (unsigned int)recordedSamples);
    }
  }

  // END packet
  packet = {};
  packet.type = PACKET_END;
  packet.seq = seqCounter++;
  packet.sampleCount = 0;
  sendPacket(&packet);

  delay(300);

  Serial.println("[send] Done.");
  Serial.println("[STATE] Sending finished.");
  Serial.printf("[send] packetsQueued=%u sendErrors=%u cbOK=%u cbFail=%u\n",
                packetsQueued,
                sendErrors,
                sendSuccessCallbacks,
                sendFailCallbacks);

  Serial.println("[state] Returning to IDLE. Press button to record again.");
  state = IDLE;
}

void setup() {
  Serial.begin(115200);
  delay(1000);

  pinMode(BUTTON_PIN, INPUT_PULLUP);

  pinMode(RECORDING_LED_PIN, OUTPUT);
  digitalWrite(RECORDING_LED_PIN, LOW);

  Serial.println();
  Serial.println("=== ESP-NOW RECORD THEN SEND - SENDER ===");

  Serial.printf("[INFO] Button pin: GPIO%d\n", BUTTON_PIN);
  Serial.printf("[INFO] Recording LED pin: GPIO%d\n", RECORDING_LED_PIN);

  Serial.printf("[INFO] Max recording: %d seconds\n", MAX_RECORD_SECONDS);
  Serial.printf("[INFO] Max samples: %u\n", (unsigned int)MAX_SAMPLES);
  Serial.printf("[INFO] Recording buffer needed: %u bytes\n",
                (unsigned int)(MAX_SAMPLES * sizeof(int16_t)));
  Serial.printf("[INFO] AudioPacket size: %u bytes\n", (unsigned int)sizeof(AudioPacket));
  Serial.printf("[INFO] Free heap before malloc: %u bytes\n", (unsigned int)ESP.getFreeHeap());

  recording = (int16_t *)malloc(MAX_SAMPLES * sizeof(int16_t));

  if (recording == nullptr) {
    Serial.println("[ERROR] Failed to allocate recording buffer.");
    Serial.println("[ERROR] 5 seconds may be too large for this ESP memory state.");
    Serial.println("[ERROR] Try reducing MAX_RECORD_SECONDS or using a PSRAM board.");
    while (true) {
      digitalWrite(RECORDING_LED_PIN, HIGH);
      delay(200);
      digitalWrite(RECORDING_LED_PIN, LOW);
      delay(200);
    }
  }

  Serial.printf("[INFO] Recording buffer allocated at: %p\n", recording);
  Serial.printf("[INFO] Free heap after malloc: %u bytes\n", (unsigned int)ESP.getFreeHeap());

  setupMic();
  setupEspNow();

  Serial.println("[READY] Press button to START recording.");
}

void loop() {
  if (buttonPressedEvent()) {
    if (state == IDLE) {
      startRecording();
    } else if (state == RECORDING) {
      Serial.println("[button] Stop recording requested.");
      state = PROCESSING;
    } else if (state == PROCESSING || state == SENDING) {
      Serial.println("[button] Ignored: currently processing/sending.");
    }
  }

  if (state == RECORDING) {
    recordStep();
  } else if (state == PROCESSING) {
    processRecording();
  } else if (state == SENDING) {
    sendRecording();
  }

  delay(1);
}