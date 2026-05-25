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

  // Per-block unsmoothed estimate (post median filter, pre `lastValidPitch`
  // exponential averaging). 0 if the current block was unvoiced. Useful when
  // a downstream stage needs to cancel the live pitch wobble — the smoothed
  // pitch returned by process() lags it by 3-4 blocks and produces stale
  // cancellation ratios.
  float getInstantPitch() const { return instantPitch; }

  // True when the current block looks like a consonant (fricative,
  // plosive transient, sibilant) or other non-tonal sound rather than a
  // sustained vowel. Downstream stages use this to wipe their lock state
  // so the next vowel starts fresh and the consonant isn't "tuned".
  // Detection combines three cheap features:
  //   1. Zero-crossing rate (high for fricatives/sibilants).
  //   2. High-band-to-low-band energy ratio (high for unvoiced consonants).
  //   3. YIN tau-minimum depth (shallow when no clear periodicity).
  bool isConsonant() const { return consonantFlag; }
  bool isBreath() const { return breathFlag; }


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
  float instantPitch = 0.0f;
  float confidence = 0.0f;
  int holdCounter = 0;
  static const int holdFrames = 8;

  // Per-block consonant classifier (computed inside process()).
  bool  consonantFlag = false;
  // Simple 1st-order high-pass state for the high-band energy split.
  // Cutoff is set in prepare() to ~2.5 kHz.
  float hpfState = 0.0f;
  float hpfAlpha = 0.0f;
  // Lowest YIN tau-value seen in the current block (after the cumulative
  // mean normalisation). Used as a "voiced-ness" proxy: deep minima are
  // characteristic of a vowel, shallow ones of an unvoiced consonant.
  float lastYinMinValue = 1.0f;

  // Short median filter on recent raw pitches, to suppress single-frame
  // octave jumps (one aberrant detection in three is rejected).
  static const int kMedianHistory = 5;
  float medianHistory[kMedianHistory] = {0,0,0,0,0};
  int   medianIndex = 0;
  int   medianFilled = 0;
  float medianOf(float a, float b, float c, float d, float e) const;

  bool  breathFlag = false;
  int   breathBlockCounter = 0;

  juce::dsp::IIR::Filter<float> lowFilter;
  juce::dsp::IIR::Filter<float> midHpFilter;
  juce::dsp::IIR::Filter<float> midLpFilter;
  juce::dsp::IIR::Filter<float> highFilter;

  std::vector<float> lowScratch;
  std::vector<float> midScratch;
  std::vector<float> highScratch;
};


