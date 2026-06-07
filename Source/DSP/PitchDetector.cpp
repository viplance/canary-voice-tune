#include "PitchDetector.h"
#include <cmath>
#include <algorithm>

PitchDetector::PitchDetector() {
  circularBuffer.resize(kWindowSize, 0.0f);
}

PitchDetector::~PitchDetector() {}

void PitchDetector::prepare(double sampleRate, int samplesPerBlock) {
  currentSampleRate = sampleRate > 0 ? sampleRate : 44100.0;

  std::fill(circularBuffer.begin(), circularBuffer.end(), 0.0f);
  writeIndex = 0;

  juce::dsp::ProcessSpec spec;
  spec.sampleRate       = currentSampleRate;
  spec.maximumBlockSize = (juce::uint32)std::max(2048, samplesPerBlock + 16);
  spec.numChannels      = 1;

  lpf1.prepare(spec);
  lpf2.prepare(spec);
  *lpf1.coefficients = *juce::dsp::IIR::Coefficients<float>::makeLowPass(currentSampleRate, kLpfCutoff);
  *lpf2.coefficients = *juce::dsp::IIR::Coefficients<float>::makeLowPass(currentSampleRate, kLpfCutoff);
  lpf1.reset();
  lpf2.reset();

  for (auto& v : medianHistory) v = 0.0f;
  medianIndex   = 0;
  medianFilled  = 0;
  silentBlockCount = 0;
  voicedBlocksSinceOnset_ = 0;
  slowAnchor    = 0.0f;
  outOfRangeCount = 0;
  lastOutOfRangePitch = 0.0f;
  lastKnownGoodPitch_ = 0.0f;
  lastValidPitch = 0.0f;
  lastRawPitch   = 0.0f;
  instantPitch   = 0.0f;
  confidence     = 0.0f;
  holdCounter    = 0;
  lastClarity    = 1.0f;
  lastZcr        = 0.0f;
  lastHfRatio    = 0.0f;

  const float twoPi = 6.2831853f;
  hpfAlpha = std::exp(-twoPi * 2500.0f / (float)currentSampleRate);
  hpfState = 0.0f;

  consonantFlag      = false;
  breathFlag         = false;
  breathBlockCounter = 0;

  // Breath detection uses a single mid-band HPF + energy ratio — simpler
  // than the 3-band crossover, same behaviour for the breath gate use case.
  midHpFilter.prepare(spec);
  *midHpFilter.coefficients = *juce::dsp::IIR::Coefficients<float>::makeHighPass(currentSampleRate, 100.0f);
  midHpFilter.reset();

  int sz = std::max(2048, samplesPerBlock + 16);
  midScratch.assign(sz, 0.0f);
  // lowScratch / highScratch removed — no longer used
}

// ============================================================================
float PitchDetector::medianOf(float a, float b, float c, float d, float e) const {
  float v[5] = {a, b, c, d, e};
  std::sort(v, v + 5);
  return v[2];
}

// ============================================================================
// foldToAnchor — one pass is enough (handles ×2/÷2 and ×3/÷3 in one check)
float PitchDetector::foldToAnchor(float hz) const {
  if (slowAnchor <= 0.0f) return hz;
  float r = hz / slowAnchor;
  if      (r > 1.60f && r < 2.50f) return hz * 0.5f;
  else if (r > 0.40f && r < 0.63f) return hz * 2.0f;
  else if (r > 2.50f && r < 3.50f) return hz / 3.0f;
  else if (r > 0.29f && r < 0.40f) return hz * 3.0f;
  else if (r > 3.50f && r < 4.60f) return hz / 4.0f;
  else if (r > 0.22f && r < 0.29f) return hz * 4.0f;
  return hz;
}

// ============================================================================
// AMDF with unrolled circular buffer access for speed.
float PitchDetector::getPitchAMDF() {
  const int halfWindow = kWindowSize / 2;
  const int lagMin     = (int)(currentSampleRate / kFmax);
  const int lagMax     = std::min(halfWindow - 1, (int)(currentSampleRate / kFmin));

  if (lagMin >= lagMax) return 0.0f;

  // Build a linear read buffer starting at (writeIndex - kWindowSize) so we
  // can use simple pointer arithmetic instead of modular indexing per sample.
  // This avoids the `% kWindowSize` in the inner loop.
  float buf[kWindowSize];
  {
    int start = (writeIndex - kWindowSize + kWindowSize * 2) % kWindowSize;
    int first = kWindowSize - start;
    if (first >= kWindowSize) {
      std::copy_n(circularBuffer.data() + start, kWindowSize, buf);
    } else {
      std::copy_n(circularBuffer.data() + start, first, buf);
      std::copy_n(circularBuffer.data(), kWindowSize - first, buf + first);
    }
  }

  // AMDF main loop — O(N × lagRange), no modular index, tight inner loop.
  float dMin = 1e9f;
  float dSum = 0.0f;
  int   tauMin = lagMin;

  for (int tau = lagMin; tau <= lagMax; ++tau) {
    float d = 0.0f;
    const float* x  = buf;
    const float* xt = buf + tau;
    for (int i = 0; i < halfWindow; ++i)
      d += std::abs(x[i] - xt[i]);
    d /= (float)halfWindow;
    dSum += d;
    if (d < dMin) { dMin = d; tauMin = tau; }
  }

  float dMean = dSum / (float)(lagMax - lagMin + 1);
  lastClarity = (dMean > 1e-9f) ? (dMin / dMean) : 1.0f;
  if (lastClarity > kClarityThresh) return 0.0f;

  // Subharmonic check: prefer tau×2 if its AMDF is clearly better (octave-up error).
  {
    int tauDouble = tauMin * 2;
    if (tauDouble <= lagMax) {
      float d = 0.0f;
      const float* x  = buf;
      const float* xt = buf + tauDouble;
      for (int i = 0; i < halfWindow; ++i)
        d += std::abs(x[i] - xt[i]);
      d /= (float)halfWindow;
      if (d < dMin * 0.90f) { tauMin = tauDouble; dMin = d; }
    }
  }

  // Parabolic interpolation for sub-sample accuracy.
  float betterTau = (float)tauMin;
  if (tauMin > lagMin && tauMin < lagMax) {
    auto amdfAt = [&](int tau) {
      float d = 0.0f;
      const float* x  = buf;
      const float* xt = buf + tau;
      for (int i = 0; i < halfWindow; ++i) d += std::abs(x[i] - xt[i]);
      return d / (float)halfWindow;
    };
    float s0 = amdfAt(tauMin - 1);
    float s1 = dMin;
    float s2 = amdfAt(tauMin + 1);
    float denom = s0 - 2.0f * s1 + s2;
    if (std::abs(denom) > 1e-9f) {
      float delta = 0.5f * (s0 - s2) / denom;
      if (std::abs(delta) < 1.0f) betterTau += delta;
    }
  }

  if (betterTau <= 0.0f) return 0.0f;
  float hz = (float)currentSampleRate / betterTau;
  return (hz >= kFmin && hz <= kFmax) ? hz : 0.0f;
}

// ============================================================================
float PitchDetector::process(const float* audioData, int numSamples) {
  // --- Block RMS + ZCR + HF ratio (one pass) --------------------------------
  float sumSq      = 0.0f;
  float blockEnergy = 0.0f;
  float highEnergy  = 0.0f;
  int   zeroCrossings = 0;
  float prevS = numSamples > 0 ? audioData[0] : 0.0f;
  float prevX = hpfState, prevY = 0.0f;

  for (int i = 0; i < numSamples; ++i) {
    float x = audioData[i];
    sumSq       += x * x;
    blockEnergy += x * x;
    if ((x >= 0.0f) != (prevS >= 0.0f)) ++zeroCrossings;
    prevS = x;
    float y = hpfAlpha * (prevY + x - prevX);
    highEnergy += y * y;
    prevX = x; prevY = y;
  }
  hpfState    = prevX;
  float rms   = numSamples > 0 ? std::sqrt(sumSq / numSamples) : 0.0f;
  lastZcr     = numSamples > 0 ? (float)zeroCrossings / numSamples : 0.0f;
  lastHfRatio = blockEnergy > 1e-9f ? highEnergy / blockEnergy : 0.0f;

  // --- LPF + circular buffer ------------------------------------------------
  for (int i = 0; i < numSamples; ++i) {
    float s = lpf1.processSample(audioData[i]);
    s = lpf2.processSample(s);
    circularBuffer[writeIndex] = s;
    writeIndex = (writeIndex + 1) % kWindowSize;
  }

  // --- AMDF with onset guard ------------------------------------------------
  float rawPitch    = 0.0f;
  bool  inOnsetGuard = false;
  if (rms > kRmsGate) {
    ++voicedBlocksSinceOnset_;
    if (voicedBlocksSinceOnset_ > kOnsetGuardBlocks)
      rawPitch = getPitchAMDF();
    else
      inOnsetGuard = true;
  } else {
    voicedBlocksSinceOnset_ = 0;
  }

  // --- Consonant flag -------------------------------------------------------
  consonantFlag = (rms > 0.015f)
               && ((lastZcr > 0.15f) || (lastHfRatio > 0.28f)
                   || (lastClarity > kClarityThresh && rawPitch <= 0.0f));

  // --- Breath detection (simplified: single mid-band HPF energy ratio) ------
  // A breath is mid-band dominant and unvoiced. We reuse the already-computed
  // highEnergy from the HPF above as a proxy — true breath has high mid-band
  // energy relative to total and no clear pitch.
  {
    jassert((int)midScratch.size() >= numSamples);
    std::copy_n(audioData, numSamples, midScratch.data());
    float* p = midScratch.data();
    juce::dsp::AudioBlock<float> b(&p, 1, (size_t)numSamples);
    midHpFilter.process(juce::dsp::ProcessContextReplacing<float>(b));

    float midE = 0.0f;
    for (int i = 0; i < numSamples; ++i) midE += midScratch[i] * midScratch[i];
    float midRms = std::sqrt(midE / (float)juce::jmax(1, numSamples));

    bool blockLoud   = midRms > 0.003f;
    bool isUnvoiced  = rawPitch <= 0.0f || lastClarity > 0.35f;
    // Breath: significant mid-band energy, no strong low or high band dominance.
    // Use hfRatio < 0.3 as proxy for "not too sibilant" (not a consonant).
    bool isMidLike   = lastHfRatio < 0.30f && lastZcr < 0.12f;
    float blockDur   = numSamples / (float)juce::jmax(1.0, currentSampleRate);
    int minBreath    = juce::jmax(2, (int)std::round(0.138f / juce::jmax(0.0001f, blockDur)));
    if (blockLoud && isUnvoiced && isMidLike) ++breathBlockCounter;
    else breathBlockCounter = 0;
    breathFlag = breathBlockCounter >= minBreath;
  }

  // --- Onset guard: keep hold alive, skip post-processing -------------------
  if (inOnsetGuard) return lastValidPitch;

  // --- Post-processing: anchor fold + median + EMA --------------------------
  if (rawPitch > 0.0f) {
    lastRawPitch     = rawPitch;
    silentBlockCount = 0;

    float foldedPitch = foldToAnchor(rawPitch);
    float pitchEst = foldedPitch;

    medianHistory[medianIndex] = pitchEst;
    medianIndex = (medianIndex + 1) % kMedianHistory;
    if (medianFilled < kMedianHistory) ++medianFilled;
    if (medianFilled >= kMedianHistory)
      pitchEst = medianOf(medianHistory[0], medianHistory[1],
                          medianHistory[2], medianHistory[3],
                          medianHistory[4]);

    if (slowAnchor <= 0.0f) {
      float ref = lastKnownGoodPitch_;
      if (ref > 0.0f) {
        // Seed anchor from prior note so we fold the first estimate correctly.
        slowAnchor = ref;
        pitchEst   = foldToAnchor(pitchEst);
        // Guard: if the seeded estimate is still >6 st from the reference
        // (ratio outside [0.71, 1.41]), it is likely an octave-up AMDF error
        // on a half-filled analysis window. Reject it and hold the reference.
        float seedRatio = (ref > 0.0f) ? pitchEst / ref : 0.0f;
        if (seedRatio < 0.71f || seedRatio > 1.41f)
          pitchEst = ref;
        slowAnchor = pitchEst;
        for (auto& v : medianHistory) v = pitchEst;
        medianFilled = kMedianHistory;
        if (lastValidPitch > 0.0f)
          lastValidPitch = foldToAnchor(lastValidPitch);
      } else {
        slowAnchor = pitchEst;
      }
      outOfRangeCount = 0;
      lastOutOfRangePitch = 0.0f;
    } else {
      float r = foldedPitch / slowAnchor;
      if (r < 0.749f || r > 1.334f) {
        // Pitch is out of range. Check if it is close/stable compared to the
        // previous out-of-range pitch (within ~1.5 semitones, i.e., ratio 0.917-1.091)
        bool isStable = false;
        if (lastOutOfRangePitch > 0.0f) {
          float rDiff = foldedPitch / lastOutOfRangePitch;
          if (rDiff > 0.917f && rDiff < 1.091f) {
            isStable = true;
          }
        }
        
        if (isStable) {
          outOfRangeCount++;
          if (outOfRangeCount >= 5) {
            slowAnchor = rawPitch;
            pitchEst = foldToAnchor(rawPitch);
            outOfRangeCount = 0;
            lastOutOfRangePitch = 0.0f;
            for (auto& v : medianHistory) v = pitchEst;
            medianFilled = kMedianHistory;
          }
        } else {
          outOfRangeCount = 1;
          lastOutOfRangePitch = foldedPitch;
        }
      } else {
        outOfRangeCount = 0;
        lastOutOfRangePitch = 0.0f;
        float rEst = pitchEst / slowAnchor;
        if (rEst > 0.749f && rEst < 1.334f)
          slowAnchor = slowAnchor * 0.70f + pitchEst * 0.30f;
      }
    }

    instantPitch        = pitchEst;
    confidence          = 1.0f - lastClarity;
    lastKnownGoodPitch_ = pitchEst;

    // Output EMA alpha: fast on note attack (respond quickly to new notes),
    // slow on sustained notes (suppress vibrato from reaching the shifter).
    // kSustainBlocks ≈ 150 ms / block_duration. At 256/44100 ≈ 5.8 ms per block,
    // 26 blocks ≈ 150 ms. After that, alpha drops from 0.30 to 0.08 (τ ≈ 72 ms),
    // which keeps vibrato oscillations at ±2 st from reaching the output at full gain
    // while still following genuine note changes (which exceed 1.18× threshold → snap).
    float outAlpha = (voicedBlocksSinceOnset_ > kSustainBlocks) ? 0.08f : 0.30f;

    float ratio = lastValidPitch > 0.0f ? pitchEst / lastValidPitch : 0.0f;
    if (ratio > 0.85f && ratio < 1.18f)
      lastValidPitch = lastValidPitch * (1.0f - outAlpha) + pitchEst * outAlpha;
    else
      lastValidPitch = pitchEst;
    holdCounter = 0;

  } else {
    ++silentBlockCount;
    if (silentBlockCount >= 2) {
      for (auto& v : medianHistory) v = 0.0f;
      medianIndex = 0; medianFilled = 0;
    }
    if (silentBlockCount >= kMedianFlushBlocks) {
      slowAnchor = 0.0f;
      outOfRangeCount = 0;
      lastOutOfRangePitch = 0.0f;
    }

    if (holdCounter < holdFrames && lastValidPitch > 0.0f) {
      ++holdCounter;
      confidence = 1.0f - (float)holdCounter / holdFrames;
    } else {
      lastValidPitch = 0.0f;
      instantPitch   = 0.0f;
      confidence     = 0.0f;
    }
    if (lastValidPitch <= 0.0f) lastRawPitch = 0.0f;
  }

  return lastValidPitch;
}
