#include "ClassicPitchShifter.h"
#include <cmath>
#include <algorithm>

ClassicPitchShifter::ClassicPitchShifter() {}
ClassicPitchShifter::~ClassicPitchShifter() {}

double ClassicPitchShifter::findOptimalJump(double nominalPeriod) const
{
    // Correlate a short window at the current write position in the RAW input
    // history (corrHistoryBuffer) against windows shifted back by `lag` samples,
    // searching ±30% around nominalPeriod.
    // Using raw input avoids phase artefacts from previously shifted audio.
    static constexpr int kCorrWindow = 128;
    const float* buf = corrHistoryBuffer.getReadPointer(0);

    int lagMin = (int)std::max(8.0,  nominalPeriod * 0.70);
    int lagMax = (int)std::min((double)(kHistorySize / 4), nominalPeriod * 1.30);

    // Reference window: kCorrWindow samples ending at corrWritePos
    int refEnd = corrWritePos;
    int refStart = refEnd - kCorrWindow;

    // Compute energy of reference window for normalisation
    double refEnergy = 0.0;
    for (int i = 0; i < kCorrWindow; ++i)
    {
        int pos = refStart + i;
        float v = buf[(pos + kCorrHistorySize * 10) % kCorrHistorySize];
        refEnergy += (double)v * v;
    }
    // If signal is too quiet, fall back to nominal period
    if (refEnergy < 1e-6) return nominalPeriod;

    double bestCorr  = -2.0;
    int    bestLag   = (int)std::round(nominalPeriod);

    for (int lag = lagMin; lag <= lagMax; ++lag)
    {
        double corr    = 0.0;
        double lagEnergy = 0.0;
        for (int i = 0; i < kCorrWindow; ++i)
        {
            int rPos = refStart + i;
            int lPos = rPos - lag;
            float rv = buf[(rPos + kCorrHistorySize * 10) % kCorrHistorySize];
            float lv = buf[(lPos + kCorrHistorySize * 10) % kCorrHistorySize];
            corr      += (double)rv * lv;
            lagEnergy += (double)lv * lv;
        }
        double normCorr = corr / (std::sqrt(refEnergy * lagEnergy) + 1e-10);
        if (normCorr > bestCorr)
        {
            bestCorr = normCorr;
            bestLag  = lag;
        }
    }
    return (double)bestLag;
}

void ClassicPitchShifter::prepare(double sampleRate, int samplesPerBlock)
{
    juce::ignoreUnused(samplesPerBlock);
    currentSampleRate = sampleRate;
    numChans = kMaxChans;

    historyBuffer.setSize(numChans, kHistorySize, false, true, true);
    corrHistoryBuffer.setSize(1, kCorrHistorySize, false, true, true);

    reset();
}

void ClassicPitchShifter::reset()
{
    currentRatio = 1.0f;
    smoothedRatio = 1.0f;
    targetRatio = 1.0f;
    alpha = 1.0f;

    historyBuffer.clear();
    absoluteWritePos = 0;

    corrHistoryBuffer.clear();
    corrWritePos = 0;

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
    for (auto& p : prevR) {
        p = 1.0f;
    }
    for (auto& s : strandedSamples) {
        s = 0;
    }
    for (auto& b : crossFadeIsOnset) {
        b = false;
    }
    for (auto& u : unvoicedSamples) {
        u = 99999; // start as "long unvoiced" so first onset always fires
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
    // Enforce a safe minimum smoothing time of 15.0 ms for both attack and release in Classic mode
    // to prevent block-boundary ratio step clicks and modulation buzz artifacts at very short attack settings.
    float timeMs = isVoiced_ ? juce::jmax(attackMs, 15.0f) : 15.0f;
    float fastMs = 15.0f;
    float trackingMs = fastMs + (timeMs - fastMs) * vibratoAmount;
    if (trackingMs < 15.0f) trackingMs = 15.0f;
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

    constexpr int kGuardSamples = 64;

    for (int i = 0; i < numSamples; ++i) {
        // Write the incoming sample to the circular history buffers.
        // corrHistoryBuffer stores raw (unprocessed) input for correlation.
        float rawL = L[i];
        corrHistoryBuffer.setSample(0, corrWritePos % kCorrHistorySize, rawL);
        corrWritePos++;

        for (int c = 0; c < procChans; ++c) {
            float s = (c == 0) ? rawL : R[i];
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

            // smoothedPeriodSamples is used only for threshold decisions (stable).
            // jumpPeriod uses the instantaneous targetPeriod so the pointer lands
            // at the correct phase even when the note just changed and the smoother
            // hasn't caught up yet (a 24-sample lag at B3->D#4 = 56° phase error).
            // Sanity clamp: if targetPeriod deviates by >2x from smoothed, the
            // detector had a transient octave-flip — fall back to smoothed so the
            // jump doesn't land a half-period off (polarity flip artefact).
            double period     = smoothedPeriodSamples[c];
            double jumpPeriod = period; // fallback
            if (targetPeriod > 0.0f) {
                double ratio = (period > 0.0) ? (double)targetPeriod / period : 1.0;
                // Accept targetPeriod only if it's within a musical fifth of smoothed
                // (factor 0.67–1.5).  Outside that range the detector had a transient
                // octave flip and the jump would land a half-period off (polarity flip).
                jumpPeriod = (ratio > 0.67 && ratio < 1.5) ? (double)targetPeriod : period;
            }
            samplesSinceLastJump[c]++;

            bool crossFadeTriggered = false;
            bool onsetJump = false;
            double jumpAmount = 0.0;

            if (period > 0.0) {
                // Syllable Onset Transition Cross-fade:
                // Pre-emptively shift the play pointer back by one period + guard band
                // and cross-fade from the unvoiced consonant to the periodic voiced wave.
                // Only fire if the channel has been unvoiced long enough for the DLL
                // tracker to have reached the guard position (~2 periods).  Shorter
                // gaps (vibrato amplitude dips, breaths within a phrase) don't warrant
                // a pointer repositioning — the pointer is still close to correct phase.
                bool trueOnset = (unvoicedSamples[c] >= (int)(2.0 * period));
                unvoicedSamples[c] = 0; // reset: we are back in voiced mode

                if (! wasVoicedState[c]) {
                    wasVoicedState[c] = true;
                    if (trueOnset) {
                        // After silence the DLL tracker sits at writePos - kGuardSamples.
                        // kGuardSamples (64) is smaller than a typical voice period
                        // (~178 samples at B3), so the overrun guard fires almost
                        // immediately and jumps the pointer back by one period, creating
                        // a phase-discontinuity glitch at the start of the note.
                        // Mask this unavoidable first jump by starting a short crossfade:
                        // the output linearly fades in over kOnsetCrossFadeDuration
                        // samples, suppressing the glitch while the cycle-tracker settles.
                        if (onsetFadeRemaining <= 0) {
                            onsetFadeTotal     = kOnsetCrossFadeDuration;
                            onsetFadeRemaining = kOnsetCrossFadeDuration;
                        }
                        samplesSinceLastJump[c] = 0;
                        strandedSamples[c] = 0;
                    }
                    // If not a true onset: pointer was still near correct phase, continue.
                }

                // Advance fractional output play pointer
                absoluteOutputAddr[c] += r;

                // Refractory: wait at least one crossfade duration between jumps so
                // the previous blend finishes before the next starts — overlapping
                // crossfades overwrite the secondary pointer and create a click.
                // Also enforce at least one voice period so we don't jump twice in
                // the same cycle when the ratio is large.
                int refractory = (int)std::max((double)kCrossFadeDuration, jumpPeriod);
                if (samplesSinceLastJump[c] >= refractory) {
                    // Overrun prevention (pitching up)
                    if (r > 1.0f && absoluteOutputAddr[c] >= (double)(absoluteWritePos - kGuardSamples)) {
                        // Use correlation-refined period so the crossfade secondary
                        // lands in phase with the primary (prevents destructive
                        // cancellation when vibrato shifts the true period away
                        // from the nominal estimate).
                        jumpAmount = -findOptimalJump(jumpPeriod);
                        crossFadeTriggered = true;
                    }
                    // Underrun prevention (pitching down)
                    else if (r < 1.0f && ((double)(absoluteWritePos - kGuardSamples) - absoluteOutputAddr[c]) > (2.0 * period)) {
                        jumpAmount = findOptimalJump(jumpPeriod);
                        crossFadeTriggered = true;
                    }
                    // Direction-change rescue: pointer is stranded too close to the
                    // write head after a sustained pitch-up phase (vibrato peak).
                    // The standard underrun threshold (2*period) takes 200-300 ms to
                    // fire in this case, causing phase-drift stutter on vibrato.
                    // Only rescue if the pointer has been close to the write head for
                    // at least one full period of samples — this filters out normal
                    // onset transitions where ratio also crosses 1.0 briefly.
                    else if (r <= 1.0f && period > 0.0) {
                        double distance = (double)(absoluteWritePos - kGuardSamples) - absoluteOutputAddr[c];
                        if (distance < 0.5 * period) {
                            strandedSamples[c]++;
                        } else {
                            strandedSamples[c] = 0;
                        }
                        if (prevR[c] > 1.0f && strandedSamples[c] > (int)period) {
                            jumpAmount = findOptimalJump(jumpPeriod);
                            crossFadeTriggered = true;
                            strandedSamples[c] = 0;
                        }
                    } else {
                        strandedSamples[c] = 0;
                    }
                }
                prevR[c] = r;
            } else {
                // Syllable Offset Transition Cross-fade (Voiced to Unvoiced):
                // Smoothly cross-fade from the active voiced play pointer position
                // to the clean unvoiced guard tracking position to prevent sudden pointer speed jumps!
                unvoicedSamples[c]++; // count how long we've been unvoiced
                if (wasVoicedState[c]) {
                    wasVoicedState[c] = false;
                    double targetUnvoicedAddr = (double)(absoluteWritePos - kGuardSamples);

                    // Use a short onset-duration fade so the unvoiced tail doesn't
                    // bleed the secondary pointer far into the past.
                    crossFadeOutputAddr[c] = absoluteOutputAddr[c] + r;
                    crossFadeGain[c] = 1.0f;
                    crossFadeIsOnset[c] = true; // fade quickly like an onset
                    absoluteOutputAddr[c] = targetUnvoicedAddr;
                    strandedSamples[c] = 0;
                }

                wasVoicedState[c] = false;

                // DLL-like tracking: smoothly slide pointer back to kGuardSamples distance to avoid transitions clicks
                absoluteOutputAddr[c] += 1.0;
                double currentDistance = (double)absoluteWritePos - absoluteOutputAddr[c];
                absoluteOutputAddr[c] += 0.01 * (currentDistance - (double)kGuardSamples);
            }

            // Trigger pointer cross-fade to smooth cycle repeats/drops.
            // If a crossfade is already in progress, the new primary position
            // becomes the secondary for the NEXT crossfade, starting from the
            // currently-blended gain so the fade continues from where it is
            // rather than resetting to 1.0 and creating a sudden level jump.
            if (crossFadeTriggered) {
                crossFadeOutputAddr[c] = absoluteOutputAddr[c];
                // Preserve remaining gain if a crossfade is active so we don't
                // snap the secondary level back to full and cause a discontinuity.
                if (crossFadeGain[c] <= 0.001f)
                    crossFadeGain[c] = 1.0f;
                absoluteOutputAddr[c] += jumpAmount;
                samplesSinceLastJump[c] = 0; // Reset jump counter
                strandedSamples[c] = 0;
                crossFadeIsOnset[c] = onsetJump;
            }

            // Interpolate resampled voice sample using premium cubic spline Hermite
            float resampled = 0.0f;
            if (crossFadeGain[c] > 0.0001f) {
                float sampleMain = interpolateCubic(historyBuffer.getReadPointer(c), absoluteOutputAddr[c]);
                float sampleSecondary = interpolateCubic(historyBuffer.getReadPointer(c), crossFadeOutputAddr[c]);
                
                resampled = sampleMain * (1.0f - crossFadeGain[c]) + sampleSecondary * crossFadeGain[c];
                
                // Advance secondary pointer at matching speed
                crossFadeOutputAddr[c] += r;
                // Decrement gain: onset fades quickly (crisp attack), cycle
                // jumps fade slowly (inaudible blend over many voice periods).
                float fadeStep = crossFadeIsOnset[c]
                    ? (1.0f / (float)kOnsetCrossFadeDuration)
                    : (1.0f / (float)kCrossFadeDuration);
                crossFadeGain[c] -= fadeStep;
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
