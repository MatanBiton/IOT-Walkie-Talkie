#include "audio_io.h"

#include <driver/i2s.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <string.h>
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

portMUX_TYPE speakerStateMux = portMUX_INITIALIZER_UNLOCKED;
bool speakerDrainPending = false;
uint32_t speakerLastWriteCompletedMs = 0;
bool speakerPlaybackEnabledValue = true;

portMUX_TYPE audioSettingsMux = portMUX_INITIALIZER_UNLOCKED;
uint16_t speakerVolumePercentValue =
    AudioSettingsConfig::SPEAKER_VOLUME_DEFAULT_PERCENT;
uint16_t microphoneGainPercentValue =
    AudioSettingsConfig::MICROPHONE_GAIN_DEFAULT_PERCENT;
uint16_t microphoneNoiseGatePercentValue =
    AudioSettingsConfig::MICROPHONE_NOISE_GATE_DEFAULT_PERCENT;

constexpr i2s_comm_format_t I2S_COMM_FORMAT_COMPAT = I2S_COMM_FORMAT_STAND_I2S;

uint16_t clampPercent(uint16_t value, uint16_t minimum, uint16_t maximum) {
  if (value < minimum) {
    return minimum;
  }
  if (value > maximum) {
    return maximum;
  }
  return value;
}

int16_t clampPcm16(int32_t value) {
  if (value > 32767) {
    return 32767;
  }
  if (value < -32768) {
    return -32768;
  }
  return static_cast<int16_t>(value);
}

void snapshotAudioSettings(
    uint16_t& speakerVolume,
    uint16_t& microphoneGain,
    uint16_t& noiseGate) {
  portENTER_CRITICAL(&audioSettingsMux);
  speakerVolume = speakerVolumePercentValue;
  microphoneGain = microphoneGainPercentValue;
  noiseGate = microphoneNoiseGatePercentValue;
  portEXIT_CRITICAL(&audioSettingsMux);
}

bool playbackEnabledSnapshot() {
  portENTER_CRITICAL(&speakerStateMux);
  const bool enabled = speakerPlaybackEnabledValue;
  portEXIT_CRITICAL(&speakerStateMux);
  return enabled;
}

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
  if (samplesRead == 0) {
    return false;
  }

  uint16_t unusedSpeakerVolume = 0;
  uint16_t microphoneGain = 0;
  uint16_t noiseGatePercent = 0;
  snapshotAudioSettings(
      unusedSpeakerVolume,
      microphoneGain,
      noiseGatePercent);

  int32_t peakAmplitude = 0;
  for (size_t i = 0; i < samplesRead; ++i) {
    const int32_t amplified =
        (static_cast<int32_t>(outSamples[i]) * microphoneGain) / 100;
    const int16_t scaled = clampPcm16(amplified);
    outSamples[i] = scaled;

    const int32_t magnitude =
        scaled == -32768 ? 32768 : (scaled < 0 ? -scaled : scaled);
    if (magnitude > peakAmplitude) {
      peakAmplitude = magnitude;
    }
  }

  if (noiseGatePercent > 0) {
    const int32_t threshold =
        (AudioSettingsConfig::NOISE_GATE_MAX_THRESHOLD * noiseGatePercent) /
        100;
    if (peakAmplitude < threshold) {
      memset(outSamples, 0, samplesRead * sizeof(int16_t));
    }
  }

  return true;
}

bool playPcm16(
    const int16_t* samples,
    size_t sampleCount,
    bool drainAfterWrite) {
  if (!speakerStarted || samples == nullptr || sampleCount == 0 ||
      !playbackEnabledSnapshot()) {
    return false;
  }

  // The speaker I2S output is configured as stereo. Duplicate each mono PCM
  // sample into L/R frames so mono audio plays on typical I2S amplifiers.
  static int16_t stereoBuffer[AudioConfig::SPEAKER_WRITE_FRAMES * 2];

  uint16_t speakerVolume = 0;
  uint16_t unusedMicrophoneGain = 0;
  uint16_t unusedNoiseGate = 0;
  snapshotAudioSettings(
      speakerVolume,
      unusedMicrophoneGain,
      unusedNoiseGate);

  size_t offset = 0;
  while (offset < sampleCount) {
    if (!playbackEnabledSnapshot()) {
      i2s_zero_dma_buffer(SPEAKER_I2S_PORT);
      portENTER_CRITICAL(&speakerStateMux);
      speakerDrainPending = false;
      portEXIT_CRITICAL(&speakerStateMux);
      return false;
    }

    const size_t frames = min(AudioConfig::SPEAKER_WRITE_FRAMES, sampleCount - offset);
    for (size_t i = 0; i < frames; ++i) {
      const int32_t scaled =
          (static_cast<int32_t>(samples[offset + i]) * speakerVolume) / 100;
      const int16_t outputSample = clampPcm16(scaled);
      stereoBuffer[2 * i] = outputSample;
      stereoBuffer[2 * i + 1] = outputSample;
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

  portENTER_CRITICAL(&speakerStateMux);
  speakerDrainPending = true;
  speakerLastWriteCompletedMs = millis();
  portEXIT_CRITICAL(&speakerStateMux);

  return !drainAfterWrite || drainSpeaker();
}

bool drainSpeaker() {
  if (!speakerStarted) {
    return false;
  }

  bool needsDrain = false;
  uint32_t lastWriteMs = 0;
  portENTER_CRITICAL(&speakerStateMux);
  needsDrain = speakerDrainPending;
  lastWriteMs = speakerLastWriteCompletedMs;
  portEXIT_CRITICAL(&speakerStateMux);
  if (!needsDrain) {
    return true;
  }

  // Arduino-ESP32 2.x does not expose i2s_wait_tx_done(). Conservatively wait
  // only for the remaining maximum DMA-ring duration. For a completed P2P
  // stream that has already drained naturally, this becomes a no-op.
  constexpr uint32_t SPEAKER_DMA_DRAIN_MS =
      (AudioConfig::I2S_DMA_BUFFER_COUNT * AudioConfig::I2S_DMA_BUFFER_LEN * 1000UL) /
          AudioConfig::SAMPLE_RATE +
      5;
  const uint32_t elapsedMs = millis() - lastWriteMs;
  if (elapsedMs < SPEAKER_DMA_DRAIN_MS) {
    vTaskDelay(pdMS_TO_TICKS(SPEAKER_DMA_DRAIN_MS - elapsedMs));
  }

  portENTER_CRITICAL(&speakerStateMux);
  // Callers use this only after playback writes have stopped. If that invariant
  // changes later, do not clear a newer pending write accidentally.
  if (speakerLastWriteCompletedMs == lastWriteMs) {
    speakerDrainPending = false;
  }
  portEXIT_CRITICAL(&speakerStateMux);
  return true;
}

void setSpeakerPlaybackEnabled(bool enabled) {
  bool changed = false;
  portENTER_CRITICAL(&speakerStateMux);
  changed = speakerPlaybackEnabledValue != enabled;
  speakerPlaybackEnabledValue = enabled;
  if (!enabled) {
    speakerDrainPending = false;
  }
  portEXIT_CRITICAL(&speakerStateMux);

  if (changed && !enabled && speakerStarted) {
    i2s_zero_dma_buffer(SPEAKER_I2S_PORT);
  }
}

bool speakerPlaybackEnabled() {
  return playbackEnabledSnapshot();
}

void setSpeakerVolumePercent(uint16_t percent) {
  percent = clampPercent(
      percent,
      AudioSettingsConfig::SPEAKER_VOLUME_MIN_PERCENT,
      AudioSettingsConfig::SPEAKER_VOLUME_MAX_PERCENT);
  portENTER_CRITICAL(&audioSettingsMux);
  speakerVolumePercentValue = percent;
  portEXIT_CRITICAL(&audioSettingsMux);
}

uint16_t speakerVolumePercent() {
  portENTER_CRITICAL(&audioSettingsMux);
  const uint16_t value = speakerVolumePercentValue;
  portEXIT_CRITICAL(&audioSettingsMux);
  return value;
}

void setMicrophoneGainPercent(uint16_t percent) {
  percent = clampPercent(
      percent,
      AudioSettingsConfig::MICROPHONE_GAIN_MIN_PERCENT,
      AudioSettingsConfig::MICROPHONE_GAIN_MAX_PERCENT);
  portENTER_CRITICAL(&audioSettingsMux);
  microphoneGainPercentValue = percent;
  portEXIT_CRITICAL(&audioSettingsMux);
}

uint16_t microphoneGainPercent() {
  portENTER_CRITICAL(&audioSettingsMux);
  const uint16_t value = microphoneGainPercentValue;
  portEXIT_CRITICAL(&audioSettingsMux);
  return value;
}

void setMicrophoneNoiseGatePercent(uint16_t percent) {
  percent = clampPercent(
      percent,
      AudioSettingsConfig::MICROPHONE_NOISE_GATE_MIN_PERCENT,
      AudioSettingsConfig::MICROPHONE_NOISE_GATE_MAX_PERCENT);
  portENTER_CRITICAL(&audioSettingsMux);
  microphoneNoiseGatePercentValue = percent;
  portEXIT_CRITICAL(&audioSettingsMux);
}

uint16_t microphoneNoiseGatePercent() {
  portENTER_CRITICAL(&audioSettingsMux);
  const uint16_t value = microphoneNoiseGatePercentValue;
  portEXIT_CRITICAL(&audioSettingsMux);
  return value;
}

}  // namespace AudioIO
