#pragma once

#include <Arduino.h>

namespace AudioIO {

bool beginMicrophone();
bool beginSpeaker();

bool readMicChunk(int16_t* outSamples, size_t maxSamples, size_t& samplesRead);

// Consecutive real-time P2P aggregates pass drainAfterWrite=false. The speaker
// DMA ring is drained once, before half-duplex operation returns to the mic.
bool playPcm16(
    const int16_t* samples,
    size_t sampleCount,
    bool drainAfterWrite = true);
bool drainSpeaker();

// Used by GUI screens that must be silent. Disabling playback aborts the
// current speaker write at the next small I2S write segment and clears queued
// DMA audio.
void setSpeakerPlaybackEnabled(bool enabled);
bool speakerPlaybackEnabled();

// Runtime settings. Values are clamped to the ranges in AudioSettingsConfig.
void setSpeakerVolumePercent(uint16_t percent);
uint16_t speakerVolumePercent();

void setMicrophoneGainPercent(uint16_t percent);
uint16_t microphoneGainPercent();

void setMicrophoneNoiseGatePercent(uint16_t percent);
uint16_t microphoneNoiseGatePercent();

}  // namespace AudioIO
