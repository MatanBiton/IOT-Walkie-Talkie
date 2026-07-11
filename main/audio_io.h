#pragma once

#include <Arduino.h>

namespace AudioIO {

bool beginMicrophone();
bool beginSpeaker();

bool readMicChunk(int16_t* outSamples, size_t maxSamples, size_t& samplesRead);
bool playPcm16(const int16_t* samples, size_t sampleCount);

}  // namespace AudioIO
