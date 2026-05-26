#include "ClassicPitchShifter.h"
#include <cmath>
#include <algorithm>

ClassicPitchShifter::ClassicPitchShifter() {}
ClassicPitchShifter::~ClassicPitchShifter() {}

void ClassicPitchShifter::prepare(double sampleRate, int samplesPerBlock)
{
    currentSampleRate = sampleRate;
    numChans = kMaxChans;

    currentRatio = 1.0f;
    smoothedRatio = 1.0f;
    targetRatio = 1.0f;
    alpha = 1.0f;

    historyBuffer.setSize(numChans, kHistorySize, false, true, true);
    historyBuffer.clear();
    absoluteWritePos = 0;
    for (auto& addr : absoluteOutputAddr) {
        addr = 0.0;
    }
    for (auto& addr : crossFadeOutputAddr) {
        addr = 0.0;
    }
    for (auto& gain : crossFadeGain) {
        gain = 0.0f;
    }
    for (auto& p : smoothedPeriodSamples) {
        p = 0.0;
    }
    for (auto& s : samplesSinceLastJump) {
        s = 99999;
    }

    isVoiced_ = false;
    isVoicedDebounced = false;
    voicedBlockCounter = 0;
    unvoicedBlockCounter = 0;
    for (auto& st : wasVoicedState) {
        st = false;
    }
    currentDetectedHz = 0.0f;

    // --- Prepare dynamic vocal FX (identical premium parameters) ---
    juce::dsp::ProcessSpec spec;
    spec.sampleRate = currentSampleRate;
    spec.maximumBlockSize = (juce::uint32)samplesPerBlock;
    spec.numChannels = 1;

    auto crossoverCoeffs = juce::dsp::IIR::Coefficients<float>::makeHighPass(currentSampleRate, 3500.0f);
    auto harmonicCoeffs = juce::dsp::IIR::Coefficients<float>::makeHighPass(currentSampleRate, 4000.0f);

    for (int c = 0; c < numChans; ++c) {
        sibilantsFilter[c].prepare(spec);
        sibilantsFilter[c].reset();

        exciterCrossover[c].prepare(spec);
        *exciterCrossover[c].coefficients = *crossoverCoeffs;
        exciterCrossover[c].reset();

        exciterHarmonicFilter[c].prepare(spec);
        *exciterHarmonicFilter[c].coefficients = *harmonicCoeffs;
        exciterHarmonicFilter[c].reset();

        popLow[c].prepare(spec);
        popHigh[c].prepare(spec);
        popLow[c].setType(juce::dsp::LinkwitzRileyFilterType::lowpass);
        popHigh[c].setType(juce::dsp::LinkwitzRileyFilterType::highpass);
        popLow[c].setCutoffFrequency(kPopCrossoverHz);
        popHigh[c].setCutoffFrequency(kPopCrossoverHz);
        popLow[c].reset();
        popHigh[c].reset();
    }

    currentSibilantsDb = 9999.0f;
    setToneShaping(0.0f, 0.0f);

    popFastAlpha    = 1.0f - std::exp(-1.0f / (0.003f * (float)currentSampleRate));
    popSlowAlpha    = 1.0f - std::exp(-1.0f / (0.150f * (float)currentSampleRate));
    popAttackAlpha  = 1.0f - std::exp(-1.0f / (0.005f * (float)currentSampleRate));
    popReleaseAlpha = 1.0f - std::exp(-1.0f / (0.080f * (float)currentSampleRate));
    popFastEnv = popSlowEnv = 0.0f;
    popGain = 1.0f;
    popThresholdDb = 0.0f;
    popActivity.store(0.0f);
    popBassTemp.setSize(numChans, samplesPerBlock + 16, false, true, true);
    popHighTemp.setSize(numChans, samplesPerBlock + 16, false, true, true);
    popBassTemp.clear();
    popHighTemp.clear();

    onsetFadeTotal = 0;
    onsetFadeRemaining = 0;

    float breathAttackMs = 10.0f;
    float breathReleaseMs = 40.0f;
    breathAttackAlpha = 1.0f - std::exp(-1.0f / (breathAttackMs * 0.001f * currentSampleRate));
    breathReleaseAlpha = 1.0f - std::exp(-1.0f / (breathReleaseMs * 0.001f * currentSampleRate));
    breathGain = 1.0f;
    breathThresholdDb = 0.0f;
    isBreathActive = false;
    breathActivity.store(0.0f);

    breathInputRms.assign(256, 0.0f);
    breathGateDelay.assign(256, false);
    breathBlockIndex = 0;
    this->blockSize = samplesPerBlock;
}

void ClassicPitchShifter::setToneShaping(float sibilantsDb, float breathDb)
{
    juce::ignoreUnused(breathDb);
    if (std::abs(sibilantsDb - currentSibilantsDb) > 0.01f) {
        currentSibilantsDb = sibilantsDb;
        float gain = std::pow(10.0f, sibilantsDb / 20.0f);
        sibilantsCoeffs = juce::dsp::IIR::Coefficients<float>::makeHighShelf(
            currentSampleRate, kSibilantsHz, 0.7071f, gain);
        for (int c = 0; c < numChans; ++c) {
            *sibilantsFilter[c].coefficients = *sibilantsCoeffs;
        }
    }
}

void ClassicPitchShifter::setExciter(float exciterDb, bool isConsonant)
{
    if (exciterDb > 6.0f) exciterDb = 6.0f;
    if (exciterDb < 0.0f) exciterDb = 0.0f;
    this->exciterDb = exciterDb;
    this->isConsonantActive = isConsonant;
}

void ClassicPitchShifter::setPopFilter(float thresholdDb)
{
    if (thresholdDb > 0.0f)   thresholdDb = 0.0f;
    if (thresholdDb < -36.0f) thresholdDb = -36.0f;
    popThresholdDb = thresholdDb;
}

void ClassicPitchShifter::setBreathGate(float thresholdDb, bool isBreathDetected)
{
    if (thresholdDb > 0.0f)   thresholdDb = 0.0f;
    if (thresholdDb < -48.0f) thresholdDb = -48.0f;
    breathThresholdDb = thresholdDb;
    isBreathActive = isBreathDetected;
}

void ClassicPitchShifter::triggerOnsetFade(float fadeMs)
{
    if (fadeMs < 0.5f) fadeMs = 0.5f;
    int n = (int)((float)currentSampleRate * fadeMs / 1000.0f);
    if (n < 16) n = 16;
    onsetFadeTotal     = n;
    onsetFadeRemaining = n;
}

void ClassicPitchShifter::setTargetShift(float ratio, float attackMs, float releaseMs,
                                         bool isVoiced, float detectedHz,
                                         float vibratoAmount)
{
    currentRatio = ratio;
    currentDetectedHz = detectedHz;

    // --- Voicing Hysteresis Debouncer ---
    // Engages after 2 consecutive blocks of voiced signal (prevents transient clicks at onset).
    // Disengages after 4 consecutive blocks of unvoiced signal (prevents dropouts during fry).
    if (isVoiced) {
        voicedBlockCounter++;
        unvoicedBlockCounter = 0;
        if (voicedBlockCounter >= 2) {
            isVoicedDebounced = true;
        }
    } else {
        unvoicedBlockCounter++;
        voicedBlockCounter = 0;
        // If it's a consonant (releaseMs <= 15ms), instantly bypass voiced mode
        // to prevent clicks inside sibilants/noise!
        if (unvoicedBlockCounter >= 4 || releaseMs <= 15.0f) {
            isVoicedDebounced = false;
        }
    }
    
    isVoiced_ = isVoicedDebounced;

    float target = isVoiced_ ? currentRatio : 1.0f;
    float timeMs = isVoiced_ ? attackMs : releaseMs;
    float fastMs = 12.0f;
    float trackingMs = fastMs + (timeMs - fastMs) * vibratoAmount;
    if (trackingMs < 1.0f) trackingMs = 1.0f;
    float timeS = trackingMs / 1000.0f;
    alpha = 1.0f - std::exp(-1.0f / (timeS * currentSampleRate));
    targetRatio = target;
}

void ClassicPitchShifter::process(juce::AudioBuffer<float>& buffer)
{
    int numSamples  = buffer.getNumSamples();
    int hostChans   = buffer.getNumChannels();
    int procChans   = juce::jmin(numChans, hostChans > 0 ? hostChans : 1);

    auto* L = buffer.getWritePointer(0);
    auto* R = (hostChans > 1) ? buffer.getWritePointer(1) : L;

    // ----- 0) Smart breath gate (latency-compensated buffer setup) -----
    int N = breathBlockIndex;
    breathBlockIndex = (breathBlockIndex + 1) % 256;

    float rms = 0.0f;
    for (int i = 0; i < numSamples; ++i) {
        float sumSq = 0.0f;
        for (int c = 0; c < procChans; ++c) {
            float s = (c == 0) ? L[i] : R[i];
            sumSq += s * s;
        }
        rms += sumSq / (float)procChans;
    }
    rms = std::sqrt(rms / (float)numSamples);
    breathInputRms[(size_t)N] = rms;

    // In Classic mode, latency is 0 samples, so delayBlocks is 0
    int delayBlocks = 0;
    float blockDuration = (float)numSamples / (float)juce::jmax(1.0, currentSampleRate);
    int minBreathBlocks = (int)std::round(0.138f / juce::jmax(0.0001f, blockDuration));
    if (minBreathBlocks < 2) minBreathBlocks = 2;

    if (breathThresholdDb < -0.01f) {
        if (isBreathActive) {
            float threshLin = std::pow(10.0f, breathThresholdDb / 20.0f);
            for (int b = N - minBreathBlocks + 1; b <= N; ++b) {
                int idx = (b + 512) % 256;
                if (breathInputRms[(size_t)idx] > threshLin) {
                    breathGateDelay[(size_t)((b + delayBlocks + 512) % 256)] = true;
                }
            }
        } else {
            breathGateDelay[(size_t)((N + delayBlocks + 256) % 256)] = false;
        }
    }

    // ----- 0b) Adaptive pop filter -----
    if (popThresholdDb < -0.01f) {
        if (popBassTemp.getNumSamples() < numSamples) {
            popBassTemp.setSize(numChans, numSamples + 16, false, true, true);
            popHighTemp.setSize(numChans, numSamples + 16, false, true, true);
        }
        for (int c = 0; c < procChans; ++c) {
            const float* src = (c == 0) ? L : R;
            std::copy_n(src, numSamples, popBassTemp.getWritePointer(c));
            std::copy_n(src, numSamples, popHighTemp.getWritePointer(c));
            float* bp = popBassTemp.getWritePointer(c);
            float* hp = popHighTemp.getWritePointer(c);
            juce::dsp::AudioBlock<float> bBlock(&bp, 1, (size_t)numSamples);
            juce::dsp::AudioBlock<float> hBlock(&hp, 1, (size_t)numSamples);
            juce::dsp::ProcessContextReplacing<float> bCtx(bBlock);
            juce::dsp::ProcessContextReplacing<float> hCtx(hBlock);
            popLow[c].process(bCtx);
            popHigh[c].process(hCtx);
        }

        float threshLin   = std::pow(10.0f, popThresholdDb / 20.0f);
        float duckGainLin = std::pow(10.0f, kPopDuckDb     / 20.0f);

        for (int i = 0; i < numSamples; ++i) {
            float bassMag = 0.0f;
            for (int c = 0; c < procChans; ++c)
                bassMag += std::abs(popBassTemp.getSample(c, i));
            bassMag /= (float)procChans;

            popFastEnv += popFastAlpha * (bassMag - popFastEnv);
            popSlowEnv += popSlowAlpha * (bassMag - popSlowEnv);
            bool trigger = (popFastEnv > threshLin) && (popFastEnv > popSlowEnv * 2.0f);
            float targetGain = trigger ? duckGainLin : 1.0f;
            float a = (targetGain < popGain) ? popAttackAlpha : popReleaseAlpha;
            popGain += a * (targetGain - popGain);

            for (int c = 0; c < procChans; ++c) {
                popBassTemp.getWritePointer(c)[i] *= popGain;
            }
        }
        for (int c = 0; c < procChans; ++c) {
            float* dst = (c == 0) ? L : R;
            const float* lo = popBassTemp.getReadPointer(c);
            const float* hi = popHighTemp.getReadPointer(c);
            for (int i = 0; i < numSamples; ++i) dst[i] = lo[i] + hi[i];
        }

        float activity = juce::jlimit(0.0f, 1.0f,
                                      (1.0f - popGain) / (1.0f - duckGainLin));
        popActivity.store(activity);
    } else {
        popFastEnv = popSlowEnv = 0.0f;
        popGain = 1.0f;
        for (int c = 0; c < procChans; ++c) {
            popLow[c].reset();
            popHigh[c].reset();
        }
        popActivity.store(0.0f);
    }

    // ----- 1) Time-domain Cycle-Resampling pitch shifter -----
    float startRatio = smoothedRatio;
    for (int i = 0; i < numSamples; ++i)
        smoothedRatio += alpha * (targetRatio - smoothedRatio);

    // Dynamic period calculation in samples with voice clipping
    float targetPeriod = 0.0f;
    if (isVoiced_ && currentDetectedHz > 50.0f) {
        float rawPeriod = (float)(currentSampleRate / currentDetectedHz);
        float minPeriod = (float)(currentSampleRate / 1200.0f);
        float maxPeriod = (float)(currentSampleRate / 60.0f);
        targetPeriod = juce::jlimit(minPeriod, maxPeriod, rawPeriod);
    }

    constexpr int kGuardSamples = 8;

    for (int i = 0; i < numSamples; ++i) {
        // Write the incoming sample to the circular history buffer
        for (int c = 0; c < procChans; ++c) {
            float s = (c == 0) ? L[i] : R[i];
            historyBuffer.setSample(c, absoluteWritePos % kHistorySize, s);
        }

        // Compute local ratio for smooth interpolation across block
        float r = startRatio + ((float)i / (float)numSamples) * (smoothedRatio - startRatio);
        r = juce::jlimit(0.25f, 4.0f, r);

        for (int c = 0; c < procChans; ++c) {
            // Smooth the period per channel to prevent rapid jitter and sudden threshold snaps
            if (targetPeriod > 0.0f) {
                if (smoothedPeriodSamples[c] <= 0.0) {
                    smoothedPeriodSamples[c] = targetPeriod;
                } else {
                    smoothedPeriodSamples[c] += 0.002 * ((double)targetPeriod - smoothedPeriodSamples[c]);
                }
            } else if (! isVoiced_) {
                // Period holds/flywheels during temporary dropouts if isVoiced_ remains debounced as true.
                // We only drop it to 0.0 when we are actually debounced to unvoiced mode.
                smoothedPeriodSamples[c] = 0.0;
            }

            double period = smoothedPeriodSamples[c];
            samplesSinceLastJump[c]++;

            bool crossFadeTriggered = false;
            double jumpAmount = 0.0;

            if (period > 0.0) {
                // Syllable Onset Transition Cross-fade:
                // Pre-emptively shift the play pointer back by one period + guard band
                // and cross-fade from the unvoiced consonant to the periodic voiced wave.
                if (! wasVoicedState[c]) {
                    wasVoicedState[c] = true;
                    double targetVoicedAddr = (double)(absoluteWritePos - (int64_t)period - kGuardSamples);
                    
                    crossFadeOutputAddr[c] = absoluteOutputAddr[c];
                    crossFadeGain[c] = 1.0f;
                    absoluteOutputAddr[c] = targetVoicedAddr;
                    samplesSinceLastJump[c] = 0; // Reset jump counter on onset
                }

                // Advance fractional output play pointer
                absoluteOutputAddr[c] += r;

                // Only allow pointer jumps if the refractory period has expired to prevent double-jumps/clicks
                if (samplesSinceLastJump[c] >= 128) {
                    // Overrun prevention (pitching up)
                    if (r > 1.0f && absoluteOutputAddr[c] >= (double)(absoluteWritePos - kGuardSamples)) {
                        jumpAmount = -period;
                        crossFadeTriggered = true;
                    }
                    // Underrun prevention (pitching down)
                    else if (r < 1.0f && ((double)(absoluteWritePos - kGuardSamples) - absoluteOutputAddr[c]) > (2.0 * period)) {
                        jumpAmount = period;
                        crossFadeTriggered = true;
                    }
                }
            } else {
                wasVoicedState[c] = false;

                // DLL-like tracking: smoothly slide pointer back to kGuardSamples distance to avoid transitions clicks
                absoluteOutputAddr[c] += 1.0;
                double currentDistance = (double)absoluteWritePos - absoluteOutputAddr[c];
                absoluteOutputAddr[c] += 0.01 * (currentDistance - (double)kGuardSamples);
            }

            // Trigger pointer cross-fade to smooth cycle repeats/drops
            if (crossFadeTriggered) {
                crossFadeOutputAddr[c] = absoluteOutputAddr[c];
                crossFadeGain[c] = 1.0f;
                absoluteOutputAddr[c] += jumpAmount;
                samplesSinceLastJump[c] = 0; // Reset jump counter
            }

            // Interpolate resampled voice sample using premium cubic spline Hermite
            float resampled = 0.0f;
            if (crossFadeGain[c] > 0.0001f) {
                float sampleMain = interpolateCubic(historyBuffer.getReadPointer(c), absoluteOutputAddr[c]);
                float sampleSecondary = interpolateCubic(historyBuffer.getReadPointer(c), crossFadeOutputAddr[c]);
                
                resampled = sampleMain * (1.0f - crossFadeGain[c]) + sampleSecondary * crossFadeGain[c];
                
                // Advance secondary pointer at matching speed
                crossFadeOutputAddr[c] += r;
                // Decrement cross-fade gain
                crossFadeGain[c] -= (1.0f / (float)kCrossFadeDuration);
                if (crossFadeGain[c] < 0.0f) {
                    crossFadeGain[c] = 0.0f;
                }
            } else {
                resampled = interpolateCubic(historyBuffer.getReadPointer(c), absoluteOutputAddr[c]);
            }

            // Apply soft onset fade if active to cushion voiced start transitions
            if (onsetFadeRemaining > 0) {
                float fade = 1.0f - ((float)onsetFadeRemaining / (float)onsetFadeTotal);
                resampled *= fade;
            }

            float* dst = (c == 0) ? L : R;
            dst[i] = resampled;
        }

        if (onsetFadeRemaining > 0) {
            --onsetFadeRemaining;
        }
        absoluteWritePos++;
    }

    for (int c = 0; c < procChans; ++c) {
        float* dst = (c == 0) ? L : R;
        lastOutSample[c] = dst[numSamples - 1];
    }

    // ----- 5.5) Exciter -----
    if (exciterDb > 0.01f) {
        float consonantScale = isConsonantActive ? 0.0f : 1.0f;
        float exciterGain = consonantScale * std::pow(10.0f, (exciterDb - 12.0f) / 20.0f);
        
        for (int c = 0; c < procChans; ++c) {
            float* dst = (c == 0) ? L : R;
            for (int i = 0; i < numSamples; ++i) {
                float x = dst[i];
                float xHigh = exciterCrossover[c].processSample(x);
                
                float xDrive = xHigh * 4.0f;
                float f_pos = (xDrive + 1.0f) / (1.0f + std::abs(xDrive + 1.0f)) - 0.5f;
                float f_neg = (-xDrive + 1.0f) / (1.0f + std::abs(-xDrive + 1.0f)) - 0.5f;
                
                float xEven = 0.5f * (f_pos + f_neg);
                float xOdd  = 0.5f * (f_pos - f_neg);
                float xHarm = xEven - xOdd;
                
                float xHarmFiltered = exciterHarmonicFilter[c].processSample(xHarm);
                dst[i] += exciterGain * xHarmFiltered;
            }
        }
    } else {
        for (int c = 0; c < procChans; ++c) {
            exciterCrossover[c].reset();
            exciterHarmonicFilter[c].reset();
        }
    }

    // ----- 6) Tone EQ (post) -----
    if (std::abs(currentSibilantsDb) > 0.01f) {
        for (int c = 0; c < procChans; ++c) {
            float* dst = (c == 0) ? L : R;
            juce::dsp::AudioBlock<float> tBlock(&dst, 1, (size_t)numSamples);
            juce::dsp::ProcessContextReplacing<float> tCtx(tBlock);
            sibilantsFilter[c].process(tCtx);
        }
    } else {
        for (int c = 0; c < procChans; ++c) {
            sibilantsFilter[c].reset();
        }
    }

    // ----- 7) Smart breath gate -----
    if (breathThresholdDb < -0.01f) {
        bool trigger = breathGateDelay[(size_t)N];
        float duckGainLin = std::pow(10.0f, kBreathDuckDb / 20.0f);
        float targetGain = trigger ? duckGainLin : 1.0f;
        
        float a = (targetGain < breathGain) ? breathAttackAlpha : breathReleaseAlpha;
        for (int i = 0; i < numSamples; ++i) {
            breathGain += a * (targetGain - breathGain);
            for (int c = 0; c < procChans; ++c) {
                float* dst = (c == 0) ? L : R;
                dst[i] *= breathGain;
            }
        }
        breathActivity.store(1.0f - breathGain);
    } else {
        breathGain = 1.0f;
        breathActivity.store(0.0f);
    }
    breathGateDelay[(size_t)N] = false;

    for (int c = procChans; c < hostChans; ++c) {
        std::copy_n(L, numSamples, buffer.getWritePointer(c));
    }
}
