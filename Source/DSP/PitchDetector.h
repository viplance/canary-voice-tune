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

  // 2048 samples ≈ 46 ms at 44.1 kHz. This lets YIN search for periods up
  // to 23 ms, i.e. fundamentals down to ~43 Hz, with enough overlap at long
  // periods that the difference function is well-conditioned (the main
  // remedy for YIN's classic octave-up errors on low male voices).
  static const int yinBufferSize = 2048;
  std::vector<float> circularBuffer;
  int writeIndex = 0;

  // 0.10 is the canonical YIN absolute threshold from the de Cheveigné &
  // Kawahara paper; 0.20 (the previous value here) is permissive and lets
  // weak harmonic minima win, which produces octave jumps.
  float yinTolerance = 0.12f;
  float getPitchYin();

  float lastValidPitch = 0.0f;
  float confidence = 0.0f;
  int holdCounter = 0;
  static const int holdFrames = 8;

  // Short median filter on recent raw pitches, to suppress single-frame
  // octave jumps (one aberrant detection in three is rejected).
  static const int kMedianHistory = 5;
  float medianHistory[kMedianHistory] = {0,0,0,0,0};
  int   medianIndex = 0;
  int   medianFilled = 0;
  float medianOf(float a, float b, float c, float d, float e) const;
};
