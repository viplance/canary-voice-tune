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
  instantPitch = 0.0f;
  confidence = 0.0f;
  holdCounter = 0;
  for (auto& v : medianHistory) v = 0.0f;
  medianIndex = 0;
  medianFilled = 0;
}

float PitchDetector::medianOf(float a, float b, float c, float d, float e) const {
  float v[5] = { a, b, c, d, e };
  std::sort(v, v + 5);
  return v[2];
}

float PitchDetector::process(const float *audioData, int numSamples) {
  for (int i = 0; i < numSamples; ++i) {
    circularBuffer[writeIndex] = audioData[i];
    writeIndex = (writeIndex + 1) % yinBufferSize;
  }

  float rms = 0.0f;
  for (float sample : circularBuffer) rms += sample * sample;
  rms = std::sqrt(rms / yinBufferSize);

  float rawPitch = 0.0f;
  if (rms > 0.01f) {
    rawPitch = getPitchYin();
  }

  if (rawPitch > 0.0f) {
    // Octave correction against history. YIN is most prone to picking the
    // 2× harmonic when the fundamental is weak, or the 0.5× sub-harmonic
    // when window length lets a half-period fit. Compare against the
    // long-term lastValidPitch and snap obvious octaves back into place.
    if (lastValidPitch > 0.0f) {
      float r = rawPitch / lastValidPitch;
      // Up an octave?
      if (r > 1.7f && r < 2.3f) {
        rawPitch *= 0.5f;
      }
      // Down an octave?
      else if (r > 0.43f && r < 0.6f) {
        rawPitch *= 2.0f;
      }
      // 3:2 / 2:3 — perfect-fifth confusions sometimes happen on whistled
      // tones; don't auto-correct those, they're usually genuine.
    }

    // Push the (possibly corrected) raw pitch through a 5-frame median
    // filter to reject single-frame outliers.
    medianHistory[medianIndex] = rawPitch;
    medianIndex = (medianIndex + 1) % kMedianHistory;
    if (medianFilled < kMedianHistory) ++medianFilled;
    float pitchEst = rawPitch;
    if (medianFilled >= kMedianHistory) {
      pitchEst = medianOf(medianHistory[0], medianHistory[1],
                          medianHistory[2], medianHistory[3],
                          medianHistory[4]);
    }

    // Stash the per-block estimate before exponential smoothing so callers
    // that need the live wobble (e.g. vibrato cancellation in the shifter)
    // can read it without the multi-block lag of `lastValidPitch`.
    instantPitch = pitchEst;

    // Smooth into lastValidPitch. Big jumps now (after octave correction +
    // median) are most likely real — accept with light smoothing rather
    // than the previous "raw assignment" that caused single-block flips.
    if (lastValidPitch > 0.0f) {
      float ratio = pitchEst / lastValidPitch;
      if (ratio > 0.85f && ratio < 1.18f) {
        lastValidPitch = lastValidPitch * 0.7f + pitchEst * 0.3f;
      } else {
        // Larger but still plausible step (≤ ~3 semitones) — accept with
        // a bit of smoothing to absorb residual noise.
        lastValidPitch = lastValidPitch * 0.4f + pitchEst * 0.6f;
      }
    } else {
      lastValidPitch = pitchEst;
    }
    confidence = 1.0f;
    holdCounter = 0;
  } else {
    if (holdCounter < holdFrames && lastValidPitch > 0.0f) {
      holdCounter++;
      confidence = 1.0f - ((float)holdCounter / (float)holdFrames);
    } else {
      lastValidPitch = 0.0f;
      instantPitch = 0.0f;
      confidence = 0.0f;
      // Clear the median ring so the next voiced phrase doesn't get
      // contaminated by the previous one's pitches.
      for (auto& v : medianHistory) v = 0.0f;
      medianIndex = 0;
      medianFilled = 0;
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

    // 3. Absolute threshold — find the FIRST tau below tolerance, then
    // descend to the local minimum. Picking the first qualifier (rather
    // than the global minimum) is what suppresses 2× harmonic errors
    // because the true fundamental's tau is shorter than its harmonic's.
    if (tauEstimate == -1 && yinBuffer[tau] < yinTolerance) {
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

  // 4b. Octave-down sanity check: when YIN locked onto the 2nd harmonic of a
  // low voice, the true fundamental sits at ~2× tau and gives a strictly
  // DEEPER minimum than the harmonic. We only switch to the longer tau if it
  // is *meaningfully* better — not merely comparable — otherwise pure tones
  // (whose subharmonics produce equally-low YIN minima at every multiple of
  // the period) get mis-detected an octave low.
  if (tauEstimate > 0) {
    int doubleTau = tauEstimate * 2;
    if (doubleTau < halfBufferSize - 1) {
      float curVal = yinBuffer[tauEstimate];
      int searchLo = juce::jmax(1, (int)(doubleTau * 0.9));
      int searchHi = juce::jmin(halfBufferSize - 2, (int)(doubleTau * 1.1));
      int bestT = doubleTau;
      float bestV = yinBuffer[doubleTau];
      for (int t = searchLo; t <= searchHi; ++t) {
        if (yinBuffer[t] < bestV) { bestV = yinBuffer[t]; bestT = t; }
      }
      // Require the long-tau minimum to be clearly deeper (≤ 70% of the
      // short-tau value). This catches genuine 2nd-harmonic captures while
      // leaving clean sine tones untouched.
      if (bestV < curVal * 0.7f && bestV < 0.15f) {
        tauEstimate = bestT;
      }
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
