#include "audio_io.h"

#include <driver/i2s.h>
#if __has_include(<esp_arduino_version.h>)
#include <esp_arduino_version.h>
#endif

#include "app_config.h"
#include "consts.h"

namespace {

constexpr i2s_port_t MIC_I2S_PORT = I2S_NUM_0;
constexpr i2s_port_t SPEAKER_I2S_PORT = I2S_NUM_1;

bool micStarted = false;
bool speakerStarted = false;

constexpr i2s_comm_format_t I2S_COMM_FORMAT_COMPAT = I2S_COMM_FORMAT_STAND_I2S;

}  // namespace

namespace AudioIO {

bool beginMicrophone() {
  if (micStarted) {
    return true;
  }

  i2s_config_t config = {};
  config.mode = static_cast<i2s_mode_t>(I2S_MODE_MASTER | I2S_MODE_RX);
  config.sample_rate = AudioConfig::SAMPLE_RATE;
  config.bits_per_sample = I2S_BITS_PER_SAMPLE_16BIT;
  config.channel_format = I2S_CHANNEL_FMT_ONLY_LEFT;
  config.communication_format = I2S_COMM_FORMAT_COMPAT;
  config.intr_alloc_flags = ESP_INTR_FLAG_LEVEL1;
  config.dma_buf_count = AudioConfig::I2S_DMA_BUFFER_COUNT;
  config.dma_buf_len = AudioConfig::I2S_DMA_BUFFER_LEN;
  config.use_apll = false;
  config.tx_desc_auto_clear = false;
  config.fixed_mclk = 0;

  i2s_pin_config_t pins = {};
  pins.bck_io_num = Pins::MIC_BCLK;
  pins.ws_io_num = Pins::MIC_WS;
  pins.data_out_num = I2S_PIN_NO_CHANGE;
  pins.data_in_num = Pins::MIC_SD;

  esp_err_t err = i2s_driver_install(MIC_I2S_PORT, &config, 0, nullptr);
  if (err != ESP_OK) {
    Serial.printf("[Audio] Microphone i2s_driver_install failed: %d\n", err);
    return false;
  }

  err = i2s_set_pin(MIC_I2S_PORT, &pins);
  if (err != ESP_OK) {
    Serial.printf("[Audio] Microphone i2s_set_pin failed: %d\n", err);
    i2s_driver_uninstall(MIC_I2S_PORT);
    return false;
  }

  i2s_zero_dma_buffer(MIC_I2S_PORT);
  micStarted = true;
  Serial.println("[Audio] Microphone initialized");
  return true;
}

bool beginSpeaker() {
  if (speakerStarted) {
    return true;
  }

  i2s_config_t config = {};
  config.mode = static_cast<i2s_mode_t>(I2S_MODE_MASTER | I2S_MODE_TX);
  config.sample_rate = AudioConfig::SAMPLE_RATE;
  config.bits_per_sample = I2S_BITS_PER_SAMPLE_16BIT;
  config.channel_format = I2S_CHANNEL_FMT_RIGHT_LEFT;
  config.communication_format = I2S_COMM_FORMAT_COMPAT;
  config.intr_alloc_flags = ESP_INTR_FLAG_LEVEL1;
  config.dma_buf_count = AudioConfig::I2S_DMA_BUFFER_COUNT;
  config.dma_buf_len = AudioConfig::I2S_DMA_BUFFER_LEN;
  config.use_apll = false;
  config.tx_desc_auto_clear = true;
  config.fixed_mclk = 0;

  i2s_pin_config_t pins = {};
  pins.bck_io_num = Pins::SPEAKER_BCLK;
  pins.ws_io_num = Pins::SPEAKER_WS;
  pins.data_out_num = Pins::SPEAKER_DIN;
  pins.data_in_num = I2S_PIN_NO_CHANGE;

  esp_err_t err = i2s_driver_install(SPEAKER_I2S_PORT, &config, 0, nullptr);
  if (err != ESP_OK) {
    Serial.printf("[Audio] Speaker i2s_driver_install failed: %d\n", err);
    return false;
  }

  err = i2s_set_pin(SPEAKER_I2S_PORT, &pins);
  if (err != ESP_OK) {
    Serial.printf("[Audio] Speaker i2s_set_pin failed: %d\n", err);
    i2s_driver_uninstall(SPEAKER_I2S_PORT);
    return false;
  }

  i2s_zero_dma_buffer(SPEAKER_I2S_PORT);
  speakerStarted = true;
  Serial.println("[Audio] Speaker initialized");
  return true;
}

bool readMicChunk(int16_t* outSamples, size_t maxSamples, size_t& samplesRead) {
  samplesRead = 0;
  if (!micStarted || outSamples == nullptr || maxSamples == 0) {
    return false;
  }

  const size_t bytesToRead = maxSamples * sizeof(int16_t);
  size_t bytesRead = 0;
  const esp_err_t err = i2s_read(
      MIC_I2S_PORT,
      outSamples,
      bytesToRead,
      &bytesRead,
      portMAX_DELAY);

  if (err != ESP_OK) {
    Serial.printf("[Audio] i2s_read failed: %d\n", err);
    return false;
  }

  samplesRead = bytesRead / sizeof(int16_t);
  return samplesRead > 0;
}

bool playPcm16(const int16_t* samples, size_t sampleCount) {
  if (!speakerStarted || samples == nullptr || sampleCount == 0) {
    return false;
  }

  // The speaker I2S output is configured as stereo. Duplicate each mono PCM
  // sample into L/R frames so mono audio plays on typical I2S amplifiers.
  static int16_t stereoBuffer[AudioConfig::SPEAKER_WRITE_FRAMES * 2];

  size_t offset = 0;
  while (offset < sampleCount) {
    const size_t frames = min(AudioConfig::SPEAKER_WRITE_FRAMES, sampleCount - offset);
    for (size_t i = 0; i < frames; ++i) {
      const int16_t s = samples[offset + i];
      stereoBuffer[2 * i] = s;
      stereoBuffer[2 * i + 1] = s;
    }

    size_t bytesWritten = 0;
    const esp_err_t err = i2s_write(
        SPEAKER_I2S_PORT,
        stereoBuffer,
        frames * 2 * sizeof(int16_t),
        &bytesWritten,
        portMAX_DELAY);

    if (err != ESP_OK) {
      Serial.printf("[Audio] i2s_write failed: %d\n", err);
      return false;
    }

    offset += frames;
  }

  // ESP-IDF bundled with Arduino-ESP32 2.x has no i2s_wait_tx_done(). Keep the
  // playback task marked active for the maximum remaining DMA-ring duration so
  // microphone capture cannot begin while the final speaker frames are playing.
  constexpr uint32_t SPEAKER_DMA_DRAIN_MS =
      (AudioConfig::I2S_DMA_BUFFER_COUNT * AudioConfig::I2S_DMA_BUFFER_LEN * 1000UL) /
          AudioConfig::SAMPLE_RATE +
      5;
  vTaskDelay(pdMS_TO_TICKS(SPEAKER_DMA_DRAIN_MS));
  return true;
}

}  // namespace AudioIO
