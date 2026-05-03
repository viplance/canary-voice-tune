#pragma once

#include <JuceHeader.h>
#include <vector>
#include <deque>

class PitchDetector {
public:
  PitchDetector();
  ~PitchDetector();

  void prepare(double sampleRate, int samplesPerBlock);

  // Returns detected pitch in Hz, or 0 if unvoiced/undetected
  float process(const float *audioData, int numSamples);

  // Confidence of the last detection (0.0 = none, 1.0 = very sure)
  float getConfidence() const { return confidence; }

private:
  double currentSampleRate = 44100.0;
  
  static const int yinBufferSize = 1024;
  std::vector<float> circularBuffer;
  int writeIndex = 0;
  
  float yinTolerance = 0.20f;
  float getPitchYin();

  float lastValidPitch = 0.0f;
  float confidence = 0.0f;
  int holdCounter = 0;
  static const int holdFrames = 8; // Hold last pitch for ~8 blocks after losing detection
};
