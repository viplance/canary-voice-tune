#pragma once

#include <JuceHeader.h>

class PitchDetector {
public:
  PitchDetector();
  ~PitchDetector();

  void prepare(double sampleRate, int samplesPerBlock);

  // Returns detected pitch in Hz, or 0 if unvoiced/undetected
  float process(const float *myAudioData, int numSamples);

private:
  double currentSampleRate = 44100.0;

  // YIN algorithm buffer and variables could go here
  // For this demonstration, we'll use a simple zero-crossing
  // measurement which isn't "extreme transparency" but works for the skeleton.
  float lastSample = 0.0f;
  int samplesSinceZeroCrossing = 0;
};
