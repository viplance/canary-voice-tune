#include "PitchDetector.h"
#include <cmath>
#include <algorithm>

PitchDetector::PitchDetector() {
  circularBuffer.resize(yinBufferSize, 0.0f);
  yinBuffer.resize(yinBufferSize / 2, 0.0f);
}

PitchDetector::~PitchDetector() {}

void PitchDetector::prepare(double sampleRate, int samplesPerBlock) {
  currentSampleRate = sampleRate > 0 ? sampleRate : 44100.0;
  std::fill(circularBuffer.begin(), circularBuffer.end(), 0.0f);
  std::fill(yinBuffer.begin(), yinBuffer.end(), 0.0f);
  writeIndex = 0;
  lastValidPitch = 0.0f;
  instantPitch = 0.0f;
  confidence = 0.0f;
  holdCounter = 0;
  for (auto& v : medianHistory) v = 0.0f;
  medianIndex = 0;
  medianFilled = 0;
  silentBlockCount = 0;
  slowAnchor = 0.0f;
  consonantFlag = false;
  breathFlag = false;
  breathBlockCounter = 0;
  lastYinMinValue = 1.0f;
  lastZcr = 0.0f;
  lastHfRatio = 0.0f;

  // 1-pole HPF with -3 dB at ~2.5 kHz. Coefficient form:
  //   y[n] = alpha * (y[n-1] + x[n] - x[n-1])
  // alpha ≈ exp(-2π·fc/fs). At fc=2500 Hz, fs=44100 Hz → alpha ≈ 0.70.
  // We store the simpler equivalent: hpfAlpha = exp(-2π·fc/fs).
  const float twoPi = 6.2831853f;
  hpfAlpha = std::exp(-twoPi * 2500.0f / (float)currentSampleRate);
  hpfState = 0.0f;
  lastRawPitch = 0.0f;

  // Prepare 3-band crossover filters and scratch buffers
  juce::dsp::ProcessSpec spec;
  spec.sampleRate = currentSampleRate;
  spec.maximumBlockSize = 2048;
  spec.numChannels = 1;

  lowFilter.prepare(spec);
  midHpFilter.prepare(spec);
  midLpFilter.prepare(spec);
  highFilter.prepare(spec);

  *lowFilter.coefficients = *juce::dsp::IIR::Coefficients<float>::makeLowPass(currentSampleRate, 100.0f);
  *midHpFilter.coefficients = *juce::dsp::IIR::Coefficients<float>::makeHighPass(currentSampleRate, 100.0f);
  *midLpFilter.coefficients = *juce::dsp::IIR::Coefficients<float>::makeLowPass(currentSampleRate, 10000.0f);
  *highFilter.coefficients = *juce::dsp::IIR::Coefficients<float>::makeHighPass(currentSampleRate, 10000.0f);

  lowFilter.reset();
  midHpFilter.reset();
  midLpFilter.reset();
  highFilter.reset();

  int scratchSize = std::max(2048, samplesPerBlock + 16);
  lowScratch.resize((size_t)scratchSize, 0.0f);
  midScratch.resize((size_t)scratchSize, 0.0f);
  highScratch.resize((size_t)scratchSize, 0.0f);
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

  // RMS of the current block only — used as a voiced/unvoiced gate for YIN.
  // Previously computed over the whole 2048-sample circular buffer, which
  // kept RMS above the threshold long after the signal faded (old samples
  // still in the buffer) and caused YIN to run on stale data → spurious
  // low-pitch detections on the note tail. Block-level RMS reacts instantly.
  float sumSq = 0.0f;
  for (int i = 0; i < numSamples; ++i) sumSq += audioData[i] * audioData[i];
  float rms = (numSamples > 0) ? std::sqrt(sumSq / numSamples) : 0.0f;

  // ---- Consonant detection on the JUST-arrived block ---------------------
  // Three cheap features, computed only on the new samples (not the whole
  // circular buffer) so we react to sub-block events like a sudden 's':
  //   ZCR ........ sample-to-sample sign flips per sample
  //   HF ratio ... energy above ~2.5 kHz divided by total energy
  //   YIN depth .. how deep the cumulative-mean-normalised tau minimum is
  //                (filled in below by getPitchYin())
  int   zeroCrossings = 0;
  float prevSample    = (numSamples > 0) ? audioData[0] : 0.0f;
  float blockEnergy   = 0.0f;
  float highEnergy    = 0.0f;
  float prevX         = hpfState; // re-use hpfState as the "prev x" memory
  float prevY         = 0.0f;
  for (int i = 0; i < numSamples; ++i) {
    float x = audioData[i];
    blockEnergy += x * x;
    if ((x >= 0.0f) != (prevSample >= 0.0f)) ++zeroCrossings;
    prevSample = x;
    // 1-pole HPF: y[n] = alpha * (y[n-1] + x[n] - x[n-1])
    float y = hpfAlpha * (prevY + x - prevX);
    highEnergy += y * y;
    prevX = x;
    prevY = y;
  }
  hpfState = prevX;
  float zcr      = (numSamples > 0) ? (float)zeroCrossings / (float)numSamples : 0.0f;
  float hfRatio  = (blockEnergy > 1e-9f) ? (highEnergy / blockEnergy) : 0.0f;
  lastZcr = zcr;
  lastHfRatio = hfRatio;

  float rawPitch = 0.0f;
  if (rms > 0.01f) {
    rawPitch = getPitchYin();
  }

  // Classify. Any one of the three "loud unvoiced" symptoms triggers it,
  // *except* when the block is essentially silent (RMS very low) — there's
  // nothing useful to reset on. Also gate by minimum energy so background
  // hum doesn't flicker the flag.
  // Thresholds picked empirically:
  //   zcr > 0.20            -> fricative/sibilant (s, sh, f, th)
  //   hfRatio > 0.55        -> high-band-heavy unvoiced consonant
  //   yinMinValue > 0.45    -> no clear periodicity in the YIN search
  bool loudEnough  = (rms > 0.015f);
  bool unvoicedLike = (zcr > 0.15f) || (hfRatio > 0.28f)
                  || (lastYinMinValue > 0.40f && rawPitch <= 0.0f);
  consonantFlag = loudEnough && unvoicedLike;

  // ---- Breath detection (3-band Crossover energy analysis) ----------------
  // A breath sound is focused in the Mid range (100 Hz to 10 kHz).
  // Voiced vowels have significant low frequency energy (< 100 Hz).
  // Sibilants/consonants have significant high frequency energy (> 10 kHz).
  
  jassert (lowScratch.size() >= (size_t)numSamples);

  std::copy_n(audioData, numSamples, lowScratch.data());
  std::copy_n(audioData, numSamples, midScratch.data());
  std::copy_n(audioData, numSamples, highScratch.data());

  float* lp = lowScratch.data();
  juce::dsp::AudioBlock<float> lowBlock(&lp, 1, (size_t)numSamples);
  juce::dsp::ProcessContextReplacing<float> lowCtx(lowBlock);
  lowFilter.process(lowCtx);

  float* mp = midScratch.data();
  juce::dsp::AudioBlock<float> midBlock(&mp, 1, (size_t)numSamples);
  juce::dsp::ProcessContextReplacing<float> midCtx(midBlock);
  midHpFilter.process(midCtx);
  midLpFilter.process(midCtx);

  float* hp = highScratch.data();
  juce::dsp::AudioBlock<float> highBlock(&hp, 1, (size_t)numSamples);
  juce::dsp::ProcessContextReplacing<float> highCtx(highBlock);
  highFilter.process(highCtx);

  float lowEnergy = 0.0f;
  float midEnergy = 0.0f;
  float highEnergyBand = 0.0f;
  for (int i = 0; i < numSamples; ++i) {
    lowEnergy += lowScratch[i] * lowScratch[i];
    midEnergy += midScratch[i] * midScratch[i];
    highEnergyBand += highScratch[i] * highScratch[i];
  }

  float lowRms = std::sqrt(lowEnergy / (float)juce::jmax(1, numSamples));
  float midRms = std::sqrt(midEnergy / (float)juce::jmax(1, numSamples));
  float highRms = std::sqrt(highEnergyBand / (float)juce::jmax(1, numSamples));

  bool blockLoudEnough = (midRms > 0.003f); // -50 dBFS absolute noise floor
  bool isUnvoiced = (rawPitch <= 0.0f) || (lastYinMinValue > 0.35f);
  
  // Mid band is dominant: quiet below 100 Hz (<75% of mid) and above 10 kHz (<70% of mid)
  bool isMidBandDominant = (lowRms < midRms * 0.75f) && (highRms < midRms * 0.70f);

  // Dynamic persistence delay: require the breath-like signal to be sustained
  // for at least 138 ms (30% shorter than the 197 ms breath in breath.wav).
  // This guarantees that short consonants (<100 ms) are never false-triggered.
  float blockDuration = (float)numSamples / (float)juce::jmax(1.0, currentSampleRate);
  int minBreathBlocks = (int)std::round(0.138f / juce::jmax(0.0001f, blockDuration));
  if (minBreathBlocks < 2) minBreathBlocks = 2;

  if (blockLoudEnough && isUnvoiced && isMidBandDominant) {
    breathBlockCounter++;
  } else {
    breathBlockCounter = 0;
  }
  breathFlag = (breathBlockCounter >= minBreathBlocks);






  if (rawPitch > 0.0f) {
    lastRawPitch = rawPitch;
    silentBlockCount = 0;

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

    // Octave-continuity guard using a slow anchor.
    //
    // `slowAnchor` is a slow EMA of pitchEst that only updates when the
    // current estimate is within ±6 semitones of itself.  Because it ignores
    // transient high-pitch phonemes (voice breaks, consonant onsets, 2nd-
    // harmonic blips), it stays near the true note pitch even when the normal
    // lastValidPitch has already been contaminated.
    //
    // When pitchEst is ~2×, ~3x, or ~4x slowAnchor (ratio 1.6–2.55, 2.55-3.45, 3.45-4.6) or ~0.5x, ~0.33x, or ~0.25x slowAnchor
    // (ratio 0.44–0.56, 0.30–0.36, 0.22–0.28), we hold slowAnchor instead. This catches mixed-buffer onsets,
    // harmonic and subharmonic errors, and transitional phoneme errors, while preserving large melodic jumps like sixths.
    if (slowAnchor > 0.0f && pitchEst > 0.0f) {
      float octaveRatio = pitchEst / slowAnchor;
      if ((octaveRatio > 1.60f && octaveRatio < 2.55f) ||
          (octaveRatio > 2.55f && octaveRatio < 3.45f) ||
          (octaveRatio > 3.45f && octaveRatio < 4.60f) ||
          (octaveRatio > 0.39f && octaveRatio < 0.63f) ||
          (octaveRatio > 0.29f && octaveRatio < 0.39f) ||
          (octaveRatio > 0.21f && octaveRatio < 0.29f)) {
        pitchEst = slowAnchor;
      }
    }
    // Update slowAnchor only when pitchEst is within ±6 semitones of it.
    if (pitchEst > 0.0f) {
      if (slowAnchor <= 0.0f) {
        slowAnchor = pitchEst;
      } else {
        float r = pitchEst / slowAnchor;
        if (r > 0.75f && r < 1.335f) { // ±6 semitones
          // Track legitimate pitch changes faster (coefficient 0.35f) to prevent
          // tracking lag during slides, keeping the octave guard extremely accurate.
          slowAnchor = slowAnchor * 0.65f + pitchEst * 0.35f;
        }
        // Outside ±6 st: anchor stays frozen until pitch returns to its range.
      }
    }

    // Stash the per-block estimate before exponential smoothing so callers
    // that need the live wobble (e.g. vibrato cancellation in the shifter)
    // can read it without the multi-block lag of `lastValidPitch`.
    instantPitch = pitchEst;

    if (lastValidPitch > 0.0f) {
      float ratio = pitchEst / lastValidPitch;
      if (ratio > 0.85f && ratio < 1.18f) {
        lastValidPitch = lastValidPitch * 0.7f + pitchEst * 0.3f;
      } else {
        lastValidPitch = lastValidPitch * 0.4f + pitchEst * 0.6f;
      }
    } else {
      lastValidPitch = pitchEst;
    }
    confidence = 1.0f;
    holdCounter = 0;
  } else {
    ++silentBlockCount;
    // Flush the median ring after a short run of silent blocks so that the
    // first voiced block of the next note is not dragged toward the previous
    // note's pitch by stale median values.
    if (silentBlockCount >= kMedianFlushBlocks) {
      for (auto& v : medianHistory) v = 0.0f;
      medianIndex = 0;
      medianFilled = 0;
      slowAnchor = 0.0f;
    }

    if (holdCounter < holdFrames && lastValidPitch > 0.0f) {
      holdCounter++;
      confidence = 1.0f - ((float)holdCounter / (float)holdFrames);
    } else {
      lastValidPitch = 0.0f;
      instantPitch = 0.0f;
      confidence = 0.0f;
    }
    if (lastValidPitch <= 0.0f) {
      lastRawPitch = 0.0f;
    }
  }

  return lastValidPitch;
}

float PitchDetector::getPitchYin() {
  int halfBufferSize = yinBufferSize / 2;
  jassert (yinBuffer.size() >= (size_t)halfBufferSize);
  std::fill(yinBuffer.begin(), yinBuffer.end(), 0.0f);
  // Default to "no clear minimum"; overwritten below once we pick tau.
  lastYinMinValue = 1.0f;

  // 1. Difference function
  for (int tau = 1; tau < halfBufferSize; tau++) {
    for (int i = 0; i < halfBufferSize; i++) {
      int idx1 = (writeIndex - yinBufferSize + i + yinBufferSize) % yinBufferSize;
      int idx2 = (writeIndex - yinBufferSize + i + tau + yinBufferSize) % yinBufferSize;
      float delta = circularBuffer[idx1] - circularBuffer[idx2];
      yinBuffer[tau] += delta * delta;
    }
  }

  // 2. Cumulative mean normalized difference function (full pass — no early
  // exit so that steps 4b/4c can inspect the CMNDF at any tau without
  // reading stale raw-difference values left by an early break).
  yinBuffer[0] = 1.0f;
  float runningSum = 0.0f;
  for (int tau = 1; tau < halfBufferSize; tau++) {
    runningSum += yinBuffer[tau];
    if (runningSum == 0.0f) {
      yinBuffer[tau] = 1.0f;
    } else {
      yinBuffer[tau] *= tau / runningSum;
    }
  }

  // 3. Absolute threshold — find the FIRST tau below tolerance, then
  // descend to the local minimum. Picking the first qualifier (rather
  // than the global minimum) is what suppresses 2× harmonic errors
  // because the true fundamental's tau is shorter than its harmonic's.
  int tauEstimate = -1;
  for (int tau = 1; tau < halfBufferSize; tau++) {
    if (yinBuffer[tau] < yinTolerance) {
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
    float maxAllowedMin = (lastValidPitch <= 0.0f) ? 0.22f : 0.45f;
    if (minVal > maxAllowedMin) {
      return 0.0f; // Unvoiced
    }
  }

  // 4b. Harmonic sanity check: when YIN locked onto a harmonic (e.g. 2nd or 3rd) of a
  // low voice, the true fundamental sits at ~mult × tau and gives a strictly
  // DEEPER minimum than the harmonic. We only switch to the longer tau if it
  // is *meaningfully* better — not merely comparable — otherwise pure tones
  // (whose subharmonics produce equally-low YIN minima at every multiple of
  // the period) get mis-detected an octave low.
  if (tauEstimate > 0) {
    float curVal = yinBuffer[tauEstimate];
    for (int mult = 2; mult <= 3; ++mult) {
      int multTau = tauEstimate * mult;
      if (multTau < halfBufferSize - 1) {
        int searchLo = juce::jmax(1, (int)(multTau * 0.9));
        int searchHi = juce::jmin(halfBufferSize - 2, (int)(multTau * 1.1));
        int bestT = multTau;
        float bestV = yinBuffer[multTau];
        for (int t = searchLo; t <= searchHi; ++t) {
          if (yinBuffer[t] < bestV) { bestV = yinBuffer[t]; bestT = t; }
        }
        // Check that the candidate is actually a local minimum.
        bool isLocalMin = (bestT > 1 && bestT < halfBufferSize - 1 &&
                           yinBuffer[bestT] <= yinBuffer[bestT - 1] &&
                           yinBuffer[bestT] <= yinBuffer[bestT + 1]);

        // Original condition: switch to fundamental only when current tau is poor.
        bool origCondition = (curVal > 0.35f && bestV < curVal * 0.7f && bestV < 0.25f);
        // Extended condition: harmonic trap — even when the current tau has a
        // clean dip (curVal ≤ 0.35), if the fundamental is substantially deeper
        // (≤ 75% of current) and reasonably clean (< 0.25), the current tau is
        // likely locking onto a harmonic of the true fundamental.
        bool extCondition = (bestV < curVal * 0.75f && bestV < 0.25f);

        if (isLocalMin && (origCondition || extCondition)) {
          tauEstimate = bestT;
          break; // Found fundamental
        }
      }
    }
  }

  // 4c. Octave-up sanity check (octave-down prevention): when YIN locked onto
  // a subharmonic (e.g. 2×, 3x, or 4x the true period) because the true fundamental
  // was noisy or had strong harmonics and missed the absolute threshold,
  // we check if a shorter period (tauEstimate / divisor) has a
  // reasonably deep local minimum. If so, we snap back to the higher octave.
  if (tauEstimate > 0) {
    float curVal = yinBuffer[tauEstimate];
    // Snapping to a shorter period is only legitimate when the current tau was
    // itself a SUBHARMONIC of the true fundamental — i.e. the difference
    // function dips at least as deep at the shorter period as it does here.
    // YIN's own minima are monotonically meaningful that way: a real
    // fundamental's tau has the deepest (or near-deepest) dip, and integer
    // multiples of it dip comparably. A mere HARMONIC of the true fundamental
    // (the octave-up trap) produces a SHALLOWER dip at the shorter period, so
    // we must refuse to snap there.
    //
    // Rule: the candidate shorter-period minimum must be no worse than the
    // current one within a slack, AND clean in absolute terms. The slack is
    // the crux:
    //   * When curVal is POOR (current tau not a clean fundamental — typical at
    //     a note onset), the true fundamental and its subharmonic dip about
    //     equally; YIN says "prefer the shorter period", so we allow a generous
    //     slack and snap up. This is the legitimate octave-DOWN correction.
    //   * When curVal is CLEAN (current tau already a deep, confident
    //     fundamental — a sustained vowel), a shallower dip at the shorter
    //     period is just a HARMONIC. Snapping there is the octave-UP error that
    //     pushed steady D3 (curVal≈0.14) up to its 2nd harmonic (bestSubV≈0.30).
    //     So the slack collapses to ~0 and we refuse unless the shorter dip is
    //     genuinely at least as deep.
    // Interpolate the slack: full (0.14) at curVal≥0.35, minimum (0.08) at curVal≤0.20.
    float cleanFrac = juce::jlimit(0.0f, 1.0f, (curVal - 0.20f) / (0.35f - 0.20f));
    const float kSubharmonicSlack = 0.08f + 0.06f * cleanFrac; // guarantee a minimum slack of 0.08
    const float kSubharmonicAbsMax = 0.50f;  // allow snapping even during noisy onsets
    for (int divisor = 4; divisor >= 2; --divisor) {
      int subTau = tauEstimate / divisor;
      if (subTau >= 10) { // Keep period reasonable (e.g. >10 samples)
        int searchLo = juce::jmax(2, (int)(subTau * 0.9));
        int searchHi = juce::jmin(halfBufferSize - 2, (int)(subTau * 1.1));
        int bestSubT = subTau;
        float bestSubV = 1000.0f;

        // Find the minimum in the search window around the sub-harmonic
        for (int t = searchLo; t <= searchHi; ++t) {
          if (yinBuffer[t] < bestSubV) {
            bestSubV = yinBuffer[t];
            bestSubT = t;
          }
        }

        // Check if it's a local minimum in the YIN buffer
        bool isLocalMin = (bestSubT > 1 && bestSubT < halfBufferSize - 1 &&
                           yinBuffer[bestSubT] <= yinBuffer[bestSubT - 1] &&
                           yinBuffer[bestSubT] <= yinBuffer[bestSubT + 1]);

        // Snap up only if the shorter period's dip is absolutely clean (bestSubV < 0.18f) OR
        // comparably deep (proving the current tau was a subharmonic, not the fundamental) and clean.
        bool comparablyDeep = (bestSubV < 0.18f) ||
                              ((bestSubV <= curVal + kSubharmonicSlack) && (bestSubV < kSubharmonicAbsMax));
        if (isLocalMin && comparablyDeep) {
          tauEstimate = bestSubT;
          break; // Snapped to the shortest valid divisor (highest pitch)
        }
      }
    }
  }

  // 4d. Final candidate scoring with normalized autocorrelation and continuity.
  //
  // YIN's first-threshold rule is deliberately pitch-responsive, but singing
  // vowels can contain a strong second formant or harmonic that makes a
  // shorter-period dip look locally convincing. Before committing, compare a
  // small family of period candidates (current tau, plausible multiples and
  // divisors, plus the recent continuity anchors) using two independent
  // measures:
  //   * CMNDF depth from YIN (lower is better);
  //   * normalized autocorrelation clarity at the same lag (higher is better).
  // A continuity penalty lets the detector move to real new notes, but makes
  // octave-family candidates beat the current note only when they are clearly
  // better, which is exactly where formant-driven "quack" usually fails.
  if (tauEstimate > 0) {
    auto circularAt = [this](int pos) -> float {
      int p = pos % yinBufferSize;
      if (p < 0) p += yinBufferSize;
      return circularBuffer[(size_t)p];
    };

    auto normalizedCorrAt = [&](int tau) -> float {
      if (tau <= 0 || tau >= halfBufferSize) return -1.0f;
      double xy = 0.0, xx = 0.0, yy = 0.0;
      for (int i = 0; i < halfBufferSize; ++i) {
        float x = circularAt(writeIndex - yinBufferSize + i);
        float y = circularAt(writeIndex - yinBufferSize + i + tau);
        xy += (double)x * (double)y;
        xx += (double)x * (double)x;
        yy += (double)y * (double)y;
      }
      if (xx <= 1e-12 || yy <= 1e-12) return -1.0f;
      return (float)(xy / (std::sqrt(xx * yy) + 1e-12));
    };

    auto bestLocalTau = [&](int centerTau) -> int {
      if (centerTau <= 1) return -1;
      int radius = juce::jmax(2, (int)std::round((float)centerTau * 0.06f));
      int lo = juce::jmax(2, centerTau - radius);
      int hi = juce::jmin(halfBufferSize - 2, centerTau + radius);
      int bestT = centerTau;
      float bestV = yinBuffer[centerTau];
      for (int t = lo; t <= hi; ++t) {
        if (yinBuffer[t] < bestV) {
          bestV = yinBuffer[t];
          bestT = t;
        }
      }
      return bestT;
    };

    auto continuityPenalty = [&](int tau) -> float {
      float candidateHz = (float)currentSampleRate / (float)juce::jmax(1, tau);
      float refHz = 0.0f;
      if (slowAnchor > 0.0f)       refHz = slowAnchor;
      else if (lastRawPitch > 0.0f) refHz = lastRawPitch;
      else if (lastValidPitch > 0.0f) refHz = lastValidPitch;
      if (refHz <= 0.0f) return 0.0f;

      float semitones = 12.0f * std::abs(std::log2(candidateHz / refHz));
      if (semitones <= 0.75f) return 0.0f;
      if (semitones <= 5.0f)  return semitones * 0.015f;
      if (semitones <= 8.0f)  return 0.08f + (semitones - 5.0f) * 0.035f;
      return 0.20f + (semitones - 8.0f) * 0.065f;
    };

    bool hasContinuityRef = (slowAnchor > 0.0f || lastRawPitch > 0.0f || lastValidPitch > 0.0f);
    float baseYin = yinBuffer[tauEstimate];
    float baseCorr = normalizedCorrAt(tauEstimate);

    struct Candidate {
      int tau = -1;
      float yin = 1.0f;
      float corr = -1.0f;
      float score = -1000.0f;
    };

    Candidate best;
    auto consider = [&](int centerTau) {
      int t = bestLocalTau(centerTau);
      if (t <= 1 || t >= halfBufferSize - 1) return;

      bool isLocalMin = (yinBuffer[t] <= yinBuffer[t - 1] &&
                         yinBuffer[t] <= yinBuffer[t + 1]);
      if (! isLocalMin) return;

      float y = yinBuffer[t];
      float c = normalizedCorrAt(t);
      if (c < 0.08f || y > 0.55f) return;

      // With no continuity reference (e.g. after consonants), trust YIN's
      // first-threshold candidate unless a longer-period "fundamental" is
      // decisively better. Otherwise a resonant vowel/formant can pull the
      // detector to an octave-down subharmonic on note starts.
      if (! hasContinuityRef && t > tauEstimate) {
        bool clearlyBetter = (y < baseYin * 0.70f) && (c >= baseCorr - 0.02f);
        if (! clearlyBetter) return;
      }

      // Prefer a lower fundamental only when continuity exists; without that
      // reference the first-threshold YIN candidate is the safer default.
      float longerTauBias = (t > tauEstimate)
          ? (hasContinuityRef ? juce::jlimit(0.0f, 0.04f, 0.014f * std::log2((float)t / (float)tauEstimate)) : 0.0f)
          : 0.0f;
      float shorterTauPenalty = (t < tauEstimate)
          ? juce::jlimit(0.0f, 0.08f, 0.025f * std::log2((float)tauEstimate / (float)t))
          : 0.0f;

      Candidate cand;
      cand.tau = t;
      cand.yin = y;
      cand.corr = c;
      cand.score = (1.0f - y) + 0.45f * c + longerTauBias
                 - shorterTauPenalty - continuityPenalty(t);

      if (cand.score > best.score) best = cand;
    };

    consider(tauEstimate);
    for (int mult = 2; mult <= 4; ++mult) consider(tauEstimate * mult);
    for (int div = 2; div <= 4; ++div) consider(tauEstimate / div);
    if (lastRawPitch > 0.0f)  consider((int)std::round(currentSampleRate / lastRawPitch));
    if (slowAnchor > 0.0f)    consider((int)std::round(currentSampleRate / slowAnchor));

    if (best.tau > 0) {
      tauEstimate = best.tau;
    }
  }


  // Record the minimum's depth so callers can tell "I found a clear vowel"
  // (deep minimum, value near 0) from "I had to fall back to the best of a
  // bad lot" (shallow minimum, value near 0.5). Used by the consonant
  // classifier in process().
  if (tauEstimate > 0 && tauEstimate < halfBufferSize) {
    lastYinMinValue = yinBuffer[tauEstimate];
  } else {
    lastYinMinValue = 1.0f;
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
