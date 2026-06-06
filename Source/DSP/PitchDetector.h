#pragma once

#include <JuceHeader.h>
#include <array>
#include <vector>

class PitchDetector {
public:
  PitchDetector();
  ~PitchDetector();

  void prepare(double sampleRate, int samplesPerBlock);
  float process(const float* audioData, int numSamples);

  float getInstantPitch()  const { return instantPitch; }
  float getConfidence()    const { return confidence; }
  bool  isConsonant()      const { return consonantFlag; }
  bool  isBreath()         const { return breathFlag; }
  float getZcr()           const { return lastZcr; }
  float getHfRatio()       const { return lastHfRatio; }
  float getYinMinValue()   const { return lastClarity; }

private:
  double currentSampleRate = 44100.0;

  // ---- AMDF (Average Magnitude Difference Function) -----------------------
  // d(tau) = (1/N) * sum_{i=0}^{N-1} |x[i] - x[i+tau]|
  //
  // Minimum of d(tau) at tau = period of the signal.
  // More octave-stable than YIN because magnitude (not squared) differences
  // are less sensitive to energy bursts, and no cumulative normalisation is
  // needed — the raw minimum is already at the fundamental period.
  //
  // We run AMDF on a low-pass filtered copy of the signal (kLpfCutoff) to
  // suppress high-frequency formants that inflate |x[i]-x[i+tau]| at short
  // lags and can pull the minimum to a harmonic period.

  static constexpr int   kWindowSize  = 2048;   // analysis window (~46 ms)
  static constexpr float kLpfCutoff   = 600.0f;
  static constexpr float kFmin        =  60.0f;
  static constexpr float kFmax        = 900.0f;
  static constexpr float kRmsGate     = 0.01f;
  // Clarity threshold: normalised AMDF minimum / mean.
  // Values < kClarityThresh are confidently periodic (voiced).
  static constexpr float kClarityThresh = 0.55f;

  // Circular buffer for LPF-filtered signal
  std::vector<float> circularBuffer;
  int writeIndex = 0;

  // 2-stage LPF (≈ 4th order) applied sample-by-sample via IIR biquad
  juce::dsp::IIR::Filter<float> lpf1, lpf2;

  float getPitchAMDF();

  // ---- Smoothing ----------------------------------------------------------
  static constexpr int kMedianHistory = 5;
  float medianHistory[kMedianHistory] = {};
  int   medianIndex  = 0;
  int   medianFilled = 0;
  float medianOf(float a, float b, float c, float d, float e) const;

  int   silentBlockCount = 0;
  static constexpr int kMedianFlushBlocks = 8;

  // After silence/consonant the circular buffer needs kWindowSize/block_size
  // blocks to fill with fresh audio before AMDF is reliable. Track how many
  // voiced blocks have elapsed since the last silent/consonant block.
  int   voicedBlocksSinceOnset_ = 0;
  static constexpr int kOnsetGuardBlocks = 8;
  // Blocks after which the output EMA switches to slow mode (vibrato suppression).
  // 26 × 256/44100 ≈ 150 ms — enough to pass a clean attack before smoothing kicks in.
  static constexpr int kSustainBlocks = 26;

  // Octave fold anchor
  float slowAnchor = 0.0f;
  int   outOfRangeCount = 0;
  float lastOutOfRangePitch = 0.0f;
  float foldToAnchor(float hz) const;

  // Last pitch from a voiced segment — survives silence/consonants so that
  // when a new note starts we can seed the anchor in the correct octave.
  float lastKnownGoodPitch_ = 0.0f;

  // ---- Output state -------------------------------------------------------
  float lastValidPitch = 0.0f;
  float lastRawPitch   = 0.0f;
  float instantPitch   = 0.0f;
  float confidence     = 0.0f;
  int   holdCounter    = 0;
  static constexpr int holdFrames = 8;

  // ---- Consonant / breath -------------------------------------------------
  bool  consonantFlag    = false;
  float lastClarity      = 1.0f;
  float lastZcr          = 0.0f;
  float lastHfRatio      = 0.0f;
  float hpfState         = 0.0f;
  float hpfAlpha         = 0.0f;

  bool  breathFlag           = false;
  int   breathBlockCounter   = 0;

  // Single mid-band HPF for breath detection (replaces 3-band crossover).
  juce::dsp::IIR::Filter<float> midHpFilter;
  std::vector<float> midScratch;
};
