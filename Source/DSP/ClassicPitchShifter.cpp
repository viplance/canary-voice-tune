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

    onsetFadeTotal = 0;
    onsetFadeRemaining = 0;
}

void ClassicPitchShifter::setToneShaping(float sibilantsDb, float breathDb)
{
    juce::ignoreUnused(sibilantsDb, breathDb);
}

void ClassicPitchShifter::setExciter(float exciterDb, bool isConsonant)
{
    juce::ignoreUnused(exciterDb, isConsonant);
}

void ClassicPitchShifter::setPopFilter(float thresholdDb)
{
    juce::ignoreUnused(thresholdDb);
}

void ClassicPitchShifter::setBreathGate(float thresholdDb, bool isBreathDetected)
{
    juce::ignoreUnused(thresholdDb, isBreathDetected);
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

    for (int c = procChans; c < hostChans; ++c) {
        std::copy_n(L, numSamples, buffer.getWritePointer(c));
    }
}
