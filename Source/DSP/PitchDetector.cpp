#include "PitchDetector.h"
#include <cmath>
#include <algorithm>

PitchDetector::PitchDetector() {
  circularBuffer.resize(yinBufferSize, 0.0f);
}

PitchDetector::~PitchDetector() {}

void PitchDetector::prepare(double sampleRate, int samplesPerBlock) {
  juce::ignoreUnused(samplesPerBlock);
  currentSampleRate = sampleRate > 0 ? sampleRate : 44100.0;
  std::fill(circularBuffer.begin(), circularBuffer.end(), 0.0f);
  writeIndex = 0;
  lastValidPitch = 0.0f;
  confidence = 0.0f;
  holdCounter = 0;
}

float PitchDetector::process(const float *audioData, int numSamples) {
  for (int i = 0; i < numSamples; ++i) {
    circularBuffer[writeIndex] = audioData[i];
    writeIndex = (writeIndex + 1) % yinBufferSize;
  }
  
  // Calculate RMS to determine if voiced/active
  float rms = 0.0f;
  for (float sample : circularBuffer) {
    rms += sample * sample;
  }
  rms = std::sqrt(rms / yinBufferSize);
  
  float rawPitch = 0.0f;
  if (rms > 0.01f) {
    rawPitch = getPitchYin();
  }

  if (rawPitch > 0.0f) {
    // Got a valid detection
    // If we already have a valid pitch, only accept the new one if it's
    // reasonably close (within ~4 semitones) to avoid octave jumps
    if (lastValidPitch > 0.0f) {
      float ratio = rawPitch / lastValidPitch;
      if (ratio > 0.79f && ratio < 1.26f) {
        // Close enough — smooth toward the new pitch
        lastValidPitch = lastValidPitch * 0.7f + rawPitch * 0.3f;
      } else {
        // Big jump — could be a real note change or an octave error.
        // Accept it but don't smooth (allows genuine note transitions).
        lastValidPitch = rawPitch;
      }
    } else {
      lastValidPitch = rawPitch;
    }
    confidence = 1.0f;
    holdCounter = 0;
  } else {
    // No valid detection this frame
    if (holdCounter < holdFrames && lastValidPitch > 0.0f) {
      // Hold the last known pitch (avoids gap at word beginnings/endings)
      holdCounter++;
      confidence = 1.0f - ((float)holdCounter / (float)holdFrames);
    } else {
      // Held long enough — declare unvoiced
      lastValidPitch = 0.0f;
      confidence = 0.0f;
    }
  }

  return lastValidPitch;
}

float PitchDetector::getPitchYin() {
  int halfBufferSize = yinBufferSize / 2;
  std::vector<float> yinBuffer(halfBufferSize, 0.0f);
  
  // 1. Difference function
  for (int tau = 1; tau < halfBufferSize; tau++) {
    for (int i = 0; i < halfBufferSize; i++) {
      int idx1 = (writeIndex - yinBufferSize + i + yinBufferSize) % yinBufferSize;
      int idx2 = (writeIndex - yinBufferSize + i + tau + yinBufferSize) % yinBufferSize;
      float delta = circularBuffer[idx1] - circularBuffer[idx2];
      yinBuffer[tau] += delta * delta;
    }
  }

  // 2. Cumulative mean normalized difference function
  yinBuffer[0] = 1.0f;
  float runningSum = 0.0f;
  int tauEstimate = -1;
  
  for (int tau = 1; tau < halfBufferSize; tau++) {
    runningSum += yinBuffer[tau];
    if (runningSum == 0.0f) {
        yinBuffer[tau] = 1.0f;
    } else {
        yinBuffer[tau] *= tau / runningSum;
    }
    
    // 3. Absolute threshold
    if (tauEstimate == -1 && yinBuffer[tau] < yinTolerance) {
      // Find the local minimum
      while (tau + 1 < halfBufferSize && yinBuffer[tau + 1] < yinBuffer[tau]) {
        tau++;
      }
      tauEstimate = tau;
      break;
    }
  }

  // 4. Fallback if threshold not met
  if (tauEstimate == -1) {
    float minVal = 10000.0f;
    for (int tau = 1; tau < halfBufferSize; tau++) {
      if (yinBuffer[tau] < minVal) {
        minVal = yinBuffer[tau];
        tauEstimate = tau;
      }
    }
    if (minVal > 0.5f) { 
       return 0.0f; // Unvoiced
    }
  }

  // 5. Parabolic interpolation
  float betterTau = tauEstimate;
  if (tauEstimate > 0 && tauEstimate < halfBufferSize - 1) {
    float s0 = yinBuffer[tauEstimate - 1];
    float s1 = yinBuffer[tauEstimate];
    float s2 = yinBuffer[tauEstimate + 1];
    float denom = 2.0f * (2.0f * s1 - s2 - s0);
    if (std::abs(denom) > 1e-6f) {
      float adjustment = (s2 - s0) / denom;
      if (std::abs(adjustment) < 1.0f) {
        betterTau += adjustment;
      }
    }
  }

  if (betterTau <= 0.0f) return 0.0f;

  float pitchHz = currentSampleRate / betterTau;
  
  // Bounding to human voice
  if (pitchHz < 60.0f || pitchHz > 1200.0f) {
    return 0.0f;
  }
  
  return pitchHz;
}
