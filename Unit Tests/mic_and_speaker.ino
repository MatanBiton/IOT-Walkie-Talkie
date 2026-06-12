#include <Arduino.h>
#include <driver/i2s.h>
#include <esp_err.h>
#include <math.h>

// =============================================================
// Behavior
//   1. Wait for button press.
//   2. Record 2 seconds from the I2S microphone.
//   3. Wait 5 seconds.
//   4. Play the recording through the I2S speaker amp.
//
// Debugging
//   Open Serial Monitor at 115200 baud.
//   The sketch prints:
//     - I2S setup result for mic and speaker
//     - button press/release detection
//     - bytes read from mic
//     - raw/converted sample ranges
//     - peak amplitude after DC removal
//     - playback write progress
// =============================================================

// ---------------- Common audio settings ----------------
#define SAMPLE_RATE          16000
#define RECORD_SECONDS       3
#define NUM_SAMPLES          (SAMPLE_RATE * RECORD_SECONDS)
#define MIC_READ_CHUNK       256
#define SPEAKER_WRITE_CHUNK  128

// Increase MIC_SHIFT if playback clips/distorts. Decrease it if playback is too quiet.
#define MIC_SHIFT            12
#define PLAYBACK_GAIN        3

// Set to 1 if you want the board to play a short sine tone on boot.
// This is useful for verifying the speaker wiring independently of the microphone.
#define PLAY_SPEAKER_TEST_TONE_ON_BOOT 0

// ---------------- Button ----------------
// Button is wired between BUTTON_PIN and GND. Internal pull-up is enabled.
#define BUTTON_PIN           GPIO_NUM_14

// ---------------- I2S microphone pins ----------------
// Kept from your mic_unit_test.ino.
#define I2S_MIC_PORT         I2S_NUM_0
#define I2S_MIC_CHANNEL      I2S_CHANNEL_FMT_ONLY_RIGHT
#define I2S_MIC_BCLK         GPIO_NUM_32   // microphone SCK / BCLK
#define I2S_MIC_WS           GPIO_NUM_25   // microphone WS / LRCLK
#define I2S_MIC_SD           GPIO_NUM_33   // microphone DOUT / SD

// ---------------- I2S speaker pins ----------------
// Your speaker_test.ino used GPIO25 for DIN, but GPIO25 is already used by the mic WS.
// Therefore speaker DIN is moved to GPIO22.
#define I2S_SPK_PORT         I2S_NUM_1
#define I2S_SPK_BCLK         GPIO_NUM_27   // MAX98357A BCLK
#define I2S_SPK_WS           GPIO_NUM_26   // MAX98357A LRC / WS
#define I2S_SPK_SD           GPIO_NUM_22   // MAX98357A DIN

static int16_t recording[NUM_SAMPLES];
static int32_t micRaw[MIC_READ_CHUNK];
static int16_t speakerBuffer[SPEAKER_WRITE_CHUNK * 2]; // stereo: L,R per sample

static int16_t clampToInt16(int32_t value) {
  if (value > 32767) return 32767;
  if (value < -32768) return -32768;
  return (int16_t)value;
}

static void printErr(const char *label, esp_err_t err) {
  if (err == ESP_OK) {
    Serial.printf("[OK]   %s\n", label);
  } else {
    Serial.printf("[FAIL] %s: %d / %s\n", label, err, esp_err_to_name(err));
  }
}

static void printConfiguration() {
  Serial.println();
  Serial.println("========== Configuration ==========");
  Serial.printf("Sample rate:        %d Hz\n", SAMPLE_RATE);
  Serial.printf("Record duration:    %d seconds\n", RECORD_SECONDS);
  Serial.printf("Samples stored:     %d\n", NUM_SAMPLES);
  Serial.printf("Recording buffer:   %u bytes\n", (unsigned int)sizeof(recording));
  Serial.printf("Free heap:          %u bytes\n", (unsigned int)ESP.getFreeHeap());
  Serial.printf("MIC  port:          I2S_NUM_%d\n", (int)I2S_MIC_PORT);
  Serial.printf("MIC  BCLK/SCK:      GPIO%d\n", (int)I2S_MIC_BCLK);
  Serial.printf("MIC  WS/LRCLK:      GPIO%d\n", (int)I2S_MIC_WS);
  Serial.printf("MIC  SD/DOUT:       GPIO%d\n", (int)I2S_MIC_SD);
  Serial.printf("SPK  port:          I2S_NUM_%d\n", (int)I2S_SPK_PORT);
  Serial.printf("SPK  BCLK:          GPIO%d\n", (int)I2S_SPK_BCLK);
  Serial.printf("SPK  WS/LRC:        GPIO%d\n", (int)I2S_SPK_WS);
  Serial.printf("SPK  DIN:           GPIO%d\n", (int)I2S_SPK_SD);
  Serial.printf("Button:             GPIO%d -> GND when pressed\n", (int)BUTTON_PIN);
  Serial.printf("MIC_SHIFT:          %d\n", MIC_SHIFT);
  Serial.printf("PLAYBACK_GAIN:      %d\n", PLAYBACK_GAIN);
  Serial.println("===================================");
  Serial.println();
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

void waitForButtonRelease() {
  Serial.println("[button] Press detected. Waiting for release...");
  while (digitalRead(BUTTON_PIN) == LOW) {
    delay(10);
  }
  delay(50); // debounce after release
  Serial.println("[button] Released.");
}

bool buttonPressed() {
  if (digitalRead(BUTTON_PIN) == LOW) {
    delay(30); // debounce
    return digitalRead(BUTTON_PIN) == LOW;
  }
  return false;
}

void playSpeakerTestTone(uint16_t frequencyHz, uint16_t durationMs) {
  Serial.printf("[speaker-test] Playing %u Hz tone for %u ms...\n", frequencyHz, durationMs);

  const int samplesToPlay = (SAMPLE_RATE * durationMs) / 1000;
  const int16_t amplitude = 12000;
  float phase = 0.0f;
  float phaseIncrement = 2.0f * PI * frequencyHz / SAMPLE_RATE;
  int samplesWritten = 0;

  while (samplesWritten < samplesToPlay) {
    size_t samplesThisChunk = min((size_t)SPEAKER_WRITE_CHUNK, (size_t)(samplesToPlay - samplesWritten));

    for (size_t i = 0; i < samplesThisChunk; i++) {
      int16_t sample = (int16_t)(amplitude * sinf(phase));
      phase += phaseIncrement;
      if (phase >= 2.0f * PI) phase -= 2.0f * PI;

      speakerBuffer[2 * i] = sample;
      speakerBuffer[2 * i + 1] = sample;
    }

    size_t bytesWritten = 0;
    esp_err_t err = i2s_write(I2S_SPK_PORT,
                              speakerBuffer,
                              samplesThisChunk * 2 * sizeof(int16_t),
                              &bytesWritten,
                              portMAX_DELAY);

    if (err != ESP_OK) {
      printErr("speaker-test i2s_write", err);
      return;
    }

    samplesWritten += bytesWritten / (2 * sizeof(int16_t));
  }

  Serial.println("[speaker-test] Done.");
}

void recordFromMic() {
  Serial.println();
  Serial.println("========== Recording ==========");
  Serial.printf("[record] Starting %d second recording...\n", RECORD_SECONDS);

  size_t sampleIndex = 0;
  size_t totalBytesRead = 0;
  size_t readCalls = 0;
  size_t zeroByteReads = 0;
  int64_t sum = 0;

  int32_t rawMin = INT32_MAX;
  int32_t rawMax = INT32_MIN;
  int16_t convMin = INT16_MAX;
  int16_t convMax = INT16_MIN;
  int32_t firstRaw[8] = {0};
  size_t firstRawCount = 0;

  // Clear old data.
  memset(recording, 0, sizeof(recording));
  i2s_zero_dma_buffer(I2S_MIC_PORT);

  uint32_t startMs = millis();
  uint32_t lastProgressMs = startMs;

  while (sampleIndex < NUM_SAMPLES) {
    size_t bytesRead = 0;
    esp_err_t err = i2s_read(I2S_MIC_PORT,
                             micRaw,
                             sizeof(micRaw),
                             &bytesRead,
                             pdMS_TO_TICKS(1000));

    readCalls++;

    if (err != ESP_OK) {
      printErr("record i2s_read", err);
      continue;
    }

    if (bytesRead == 0) {
      zeroByteReads++;
      Serial.println("[record] WARNING: i2s_read returned 0 bytes.");
      continue;
    }

    totalBytesRead += bytesRead;
    size_t samplesRead = bytesRead / sizeof(int32_t);

    for (size_t i = 0; i < samplesRead && sampleIndex < NUM_SAMPLES; i++) {
      int32_t raw = micRaw[i];
      if (firstRawCount < 8) firstRaw[firstRawCount++] = raw;

      if (raw < rawMin) rawMin = raw;
      if (raw > rawMax) rawMax = raw;

      // Most I2S MEMS mics provide a signed 24-bit value inside a 32-bit word.
      // Shift down to fit a 16-bit speaker sample.
      int16_t sample = clampToInt16(raw >> MIC_SHIFT);
      if (sample < convMin) convMin = sample;
      if (sample > convMax) convMax = sample;

      recording[sampleIndex++] = sample;
      sum += sample;
    }

    uint32_t now = millis();
    if (now - lastProgressMs >= 500 || sampleIndex >= NUM_SAMPLES) {
      Serial.printf("[record] Progress: %u / %u samples, totalBytesRead=%u\n",
                    (unsigned int)sampleIndex,
                    (unsigned int)NUM_SAMPLES,
                    (unsigned int)totalBytesRead);
      lastProgressMs = now;
    }
  }

  // Remove DC offset. This usually makes playback cleaner for MEMS microphones.
  int16_t mean = (int16_t)(sum / NUM_SAMPLES);
  int16_t finalMin = INT16_MAX;
  int16_t finalMax = INT16_MIN;
  int16_t finalPeak = 0;
  size_t nonZeroSamples = 0;

  for (size_t i = 0; i < NUM_SAMPLES; i++) {
    recording[i] = clampToInt16(((int32_t)recording[i] - mean) * PLAYBACK_GAIN);

    if (recording[i] != 0) nonZeroSamples++;
    if (recording[i] < finalMin) finalMin = recording[i];
    if (recording[i] > finalMax) finalMax = recording[i];

    int32_t absSample = abs((int32_t)recording[i]);
    if (absSample > finalPeak) finalPeak = (int16_t)absSample;
  }

  uint32_t elapsedMs = millis() - startMs;

  Serial.println("[record] Done.");
  Serial.printf("[record] Elapsed:          %u ms\n", (unsigned int)elapsedMs);
  Serial.printf("[record] Read calls:       %u\n", (unsigned int)readCalls);
  Serial.printf("[record] Zero-byte reads:  %u\n", (unsigned int)zeroByteReads);
  Serial.printf("[record] Total bytes read: %u\n", (unsigned int)totalBytesRead);
  Serial.printf("[record] Raw min/max:      %ld / %ld\n", (long)rawMin, (long)rawMax);
  Serial.printf("[record] Shifted min/max:  %d / %d\n", convMin, convMax);
  Serial.printf("[record] Mean removed:     %d\n", mean);
  Serial.printf("[record] Final min/max:    %d / %d\n", finalMin, finalMax);
  Serial.printf("[record] Final peak:       %d\n", finalPeak);
  Serial.printf("[record] Non-zero samples: %u / %u\n", (unsigned int)nonZeroSamples, (unsigned int)NUM_SAMPLES);

  Serial.print("[record] First raw samples: ");
  for (size_t i = 0; i < firstRawCount; i++) {
    Serial.printf("%ld", (long)firstRaw[i]);
    if (i + 1 < firstRawCount) Serial.print(", ");
  }
  Serial.println();

  if (finalPeak < 20) {
    Serial.println("[diagnostic] WARNING: final peak is extremely small. The mic may be silent, on the wrong L/R channel, or wired incorrectly.");
    Serial.println("[diagnostic] Try swapping mic L/R between 3.3V and GND, and changing I2S_MIC_CHANNEL between ONLY_RIGHT and ONLY_LEFT.");
  } else if (finalPeak < 300) {
    Serial.println("[diagnostic] Recording exists but is quiet. Try lowering MIC_SHIFT or increasing PLAYBACK_GAIN.");
  } else if (finalPeak > 30000) {
    Serial.println("[diagnostic] Recording may be clipping. Try increasing MIC_SHIFT or lowering PLAYBACK_GAIN.");
  } else {
    Serial.println("[diagnostic] Mic data looks non-zero and probably usable.");
  }

  Serial.println("===============================");
  Serial.println();
}

void playRecording() {
  Serial.println();
  Serial.println("========== Playback ==========");
  Serial.println("[play] Starting playback...");

  i2s_zero_dma_buffer(I2S_SPK_PORT);

  size_t sampleIndex = 0;
  size_t writeCalls = 0;
  size_t totalBytesWritten = 0;
  uint32_t startMs = millis();
  uint32_t lastProgressMs = startMs;

  while (sampleIndex < NUM_SAMPLES) {
    size_t samplesThisChunk = min((size_t)SPEAKER_WRITE_CHUNK, (size_t)(NUM_SAMPLES - sampleIndex));

    for (size_t i = 0; i < samplesThisChunk; i++) {
      int16_t sample = recording[sampleIndex++];
      speakerBuffer[2 * i] = sample;       // left
      speakerBuffer[2 * i + 1] = sample;   // right
    }

    size_t bytesToWrite = samplesThisChunk * 2 * sizeof(int16_t);
    size_t bytesWritten = 0;
    esp_err_t err = i2s_write(I2S_SPK_PORT,
                              speakerBuffer,
                              bytesToWrite,
                              &bytesWritten,
                              pdMS_TO_TICKS(1000));

    writeCalls++;

    if (err != ESP_OK) {
      printErr("play i2s_write", err);
      continue;
    }

    totalBytesWritten += bytesWritten;

    if (bytesWritten != bytesToWrite) {
      Serial.printf("[play] WARNING: partial write. requested=%u, written=%u\n",
                    (unsigned int)bytesToWrite,
                    (unsigned int)bytesWritten);
    }

    uint32_t now = millis();
    if (now - lastProgressMs >= 500 || sampleIndex >= NUM_SAMPLES) {
      Serial.printf("[play] Progress: %u / %u samples, totalBytesWritten=%u\n",
                    (unsigned int)sampleIndex,
                    (unsigned int)NUM_SAMPLES,
                    (unsigned int)totalBytesWritten);
      lastProgressMs = now;
    }
  }

  uint32_t elapsedMs = millis() - startMs;
  Serial.println("[play] Done.");
  Serial.printf("[play] Elapsed:             %u ms\n", (unsigned int)elapsedMs);
  Serial.printf("[play] Write calls:         %u\n", (unsigned int)writeCalls);
  Serial.printf("[play] Total bytes written: %u\n", (unsigned int)totalBytesWritten);
  Serial.println("[play] Press the button to record again.");
  Serial.println("==============================");
  Serial.println();
}

void setup() {
  Serial.begin(115200);
  delay(1000);

  Serial.println();
  Serial.println("Booting record-then-play debug sketch...");

  pinMode(BUTTON_PIN, INPUT_PULLUP);
  Serial.printf("[setup] Button initial state: %s\n", digitalRead(BUTTON_PIN) == LOW ? "LOW/PRESSED" : "HIGH/NOT PRESSED");

  printConfiguration();
  setupMic();
  setupSpeaker();

#if PLAY_SPEAKER_TEST_TONE_ON_BOOT
  playSpeakerTestTone(440, 700);
#endif

  Serial.println();
  Serial.println("Ready. Press the button to record 2 seconds, wait 5 seconds, then play it back.");
  Serial.println("Serial Monitor baud rate must be 115200.");
}

void loop() {
  if (!buttonPressed()) {
    delay(10);
    return;
  }

  waitForButtonRelease();
  recordFromMic();

  Serial.println("[wait] Waiting 2 seconds before playback...");
  for (int secondsLeft = 2; secondsLeft > 0; secondsLeft--) {
    Serial.printf("[wait] %d...\n", secondsLeft);
    delay(1000);
  }

  playRecording();
}
