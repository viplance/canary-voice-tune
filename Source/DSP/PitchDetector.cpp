#include "PitchDetector.h"
#include <cmath>

PitchDetector::PitchDetector() {}
PitchDetector::~PitchDetector() {}

void PitchDetector::prepare(double sampleRate, int samplesPerBlock) {
  juce::ignoreUnused(samplesPerBlock);
  currentSampleRate = sampleRate;
  lastSample = 0.0f;
  samplesSinceZeroCrossing = 0;
}

float PitchDetector::process(const float *audioData, int numSamples) {
  float detectedHz = 0.0f;
  for (int i = 0; i < numSamples; ++i) {
    float currentSample = audioData[i];
    samplesSinceZeroCrossing++;

    // Basic zero-crossing detection (positive going)
    if (lastSample <= 0.0f && currentSample > 0.0f) {
      if (samplesSinceZeroCrossing > 1) {
        // Convert period to Hz
        float period = (float)samplesSinceZeroCrossing;
        float currentFreq = (float)currentSampleRate / period;

        // Voice range approx 80Hz - 1000Hz
        if (currentFreq > 80.0f && currentFreq < 1000.0f) {
          detectedHz = currentFreq;
        }
      }
      samplesSinceZeroCrossing = 0;
    }
    lastSample = currentSample;
  }

  // In a real algorithm, we would use YIN or McLeod to avoid octave errors
  // and provide much better accuracy.
  return detectedHz;
}
