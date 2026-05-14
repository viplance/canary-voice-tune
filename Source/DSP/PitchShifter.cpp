#include "PitchShifter.h"
#include <rubberband/RubberBandStretcher.h>
#include <cmath>
#include <algorithm>

PitchShifter::PitchShifter() {}
PitchShifter::~PitchShifter() {}

void PitchShifter::prepare(double sampleRate, int samplesPerBlock)
{
    currentSampleRate = sampleRate;

    // We always allocate for 2 channels (mono input is processed in channel 0
    // only and mirrored on output if the host happens to send mono).
    numChans = kMaxChans;

    currentRatio = 1.0f;
    smoothedRatio = 1.0f;
    targetRatio = 1.0f;
    appliedRatio = 1.0f;
    alpha = 1.0f;

    RubberBand::RubberBandStretcher::Options options =
        RubberBand::RubberBandStretcher::OptionProcessRealTime |
        RubberBand::RubberBandStretcher::OptionEngineFiner |
        RubberBand::RubberBandStretcher::OptionPitchHighConsistency |
        RubberBand::RubberBandStretcher::OptionFormantPreserved |
        RubberBand::RubberBandStretcher::OptionWindowStandard |
        RubberBand::RubberBandStretcher::OptionChannelsTogether;

    stretcher = std::make_unique<RubberBand::RubberBandStretcher>(
        (size_t)currentSampleRate, (size_t)numChans, options);
    stretcher->setMaxProcessSize((size_t)samplesPerBlock);
    stretcher->setPitchScale(1.0);

    outputFifo.reset();
    outputBuffer.setSize(numChans, outputBuffer.getNumSamples(), false, true, true);
    outputBuffer.clear();

    int delay = (int)stretcher->getStartDelay();
    int slack = juce::jmax(16384, samplesPerBlock * 16);
    int prefill = delay + slack;

    lookaheadSamples_ = (int)(currentSampleRate * 0.025);
    if (lookaheadSamples_ < 16) lookaheadSamples_ = 16;
    lookaheadSize = lookaheadSamples_ + samplesPerBlock + 16;
    lookaheadBuffer.setSize(numChans, lookaheadSize, false, true, true);
    lookaheadBuffer.clear();
    lookaheadOut.setSize(numChans, samplesPerBlock + 16, false, true, true);
    lookaheadOut.clear();
    lookaheadWritePos = 0;

    currentLatency = prefill + lookaheadSamples_;

    int start1, size1, start2, size2;
    outputFifo.prepareToWrite(prefill, start1, size1, start2, size2);
    if (size1 > 0) outputBuffer.clear(start1, size1);
    if (size2 > 0) outputBuffer.clear(start2, size2);
    outputFifo.finishedWrite(size1 + size2);

    rubberOut.setSize(numChans, 131072, false, true, true);
    rubberOut.clear();
    for (auto& s : lastOutSample) s = 0.0f;

    // dsp prep spec
    juce::dsp::ProcessSpec spec;
    spec.sampleRate = currentSampleRate;
    spec.maximumBlockSize = (juce::uint32)samplesPerBlock;
    spec.numChannels = 1; // each filter is per-channel
    for (int c = 0; c < numChans; ++c) {
        dryHighpass[c].prepare(spec);
        wetLowpass[c].prepare(spec);
        dryHighpass[c].setType(juce::dsp::LinkwitzRileyFilterType::highpass);
        wetLowpass[c].setType(juce::dsp::LinkwitzRileyFilterType::lowpass);
        dryHighpass[c].setCutoffFrequency(crossoverHz);
        wetLowpass[c].setCutoffFrequency(crossoverHz);
        dryHighpass[c].reset();
        wetLowpass[c].reset();

        sibilantsFilter[c].prepare(spec);
        breathFilter[c].prepare(spec);
        sibilantsFilter[c].reset();
        breathFilter[c].reset();

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
    currentBreathDb    = 9999.0f;
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
    onsetFadeDelay = 0;

    dryDelayLength = currentLatency;
    if (dryDelayLength < 1) dryDelayLength = 1;
    dryDelayCapacity = dryDelayLength + samplesPerBlock + 16;
    dryDelayBuffer.setSize(numChans, dryDelayCapacity, false, true, true);
    dryDelayBuffer.clear();
    dryDelayWritePos = 0;
}

void PitchShifter::setToneShaping(float sibilantsDb, float breathDb)
{
    if (std::abs(sibilantsDb - currentSibilantsDb) > 0.01f) {
        currentSibilantsDb = sibilantsDb;
        float gain = std::pow(10.0f, sibilantsDb / 20.0f);
        sibilantsCoeffs = juce::dsp::IIR::Coefficients<float>::makeHighShelf(
            currentSampleRate, kSibilantsHz, 0.7071f, gain);
        for (int c = 0; c < numChans; ++c) {
            *sibilantsFilter[c].coefficients = *sibilantsCoeffs;
        }
    }
    if (std::abs(breathDb - currentBreathDb) > 0.01f) {
        currentBreathDb = breathDb;
        float gain = std::pow(10.0f, breathDb / 20.0f);
        breathCoeffs = juce::dsp::IIR::Coefficients<float>::makePeakFilter(
            currentSampleRate, kBreathHz, kBreathQ, gain);
        for (int c = 0; c < numChans; ++c) {
            *breathFilter[c].coefficients = *breathCoeffs;
        }
    }
}

void PitchShifter::setPopFilter(float thresholdDb)
{
    if (thresholdDb > 0.0f)   thresholdDb = 0.0f;
    if (thresholdDb < -36.0f) thresholdDb = -36.0f;
    popThresholdDb = thresholdDb;
}

void PitchShifter::triggerOnsetFade(float fadeMs)
{
    if (fadeMs < 0.5f) fadeMs = 0.5f;
    int n = (int)((float)currentSampleRate * fadeMs / 1000.0f);
    if (n < 16) n = 16;
    onsetFadeTotal     = n;
    onsetFadeRemaining = n;
    onsetFadeDelay     = currentLatency;
}

void PitchShifter::setTargetShift(float ratio, float attackMs, float releaseMs,
                                  bool isVoiced, float detectedHz,
                                  float vibratoAmount)
{
    juce::ignoreUnused(detectedHz);
    currentRatio = ratio;

    float target = isVoiced ? currentRatio : 1.0f;
    float timeMs = isVoiced ? attackMs : releaseMs;
    // When the user has asked us to remove vibrato, the ratio (which embeds
    // 1/detectedHz) wobbles at the vibrato rate and we must track it fast
    // enough to actually cancel that wobble. Otherwise the Attack time
    // constant lags ~5 Hz vibrato and the wobble survives at the output.
    float fastMs = 5.0f;
    float trackingMs = fastMs + (timeMs - fastMs) * vibratoAmount;
    if (trackingMs < 1.0f) trackingMs = 1.0f;
    float timeS = trackingMs / 1000.0f;
    alpha = 1.0f - std::exp(-1.0f / (timeS * currentSampleRate));
    targetRatio = target;
}

void PitchShifter::process(juce::AudioBuffer<float>& buffer)
{
    if (stretcher == nullptr) return;

    int numSamples  = buffer.getNumSamples();
    int hostChans   = buffer.getNumChannels();
    int procChans   = juce::jmin(numChans, hostChans > 0 ? hostChans : 1);

    // Convenience: get write pointers per processed channel. If host gave us
    // mono, we still process kMaxChans=2 internally by duplicating channel 0.
    auto* L = buffer.getWritePointer(0);
    auto* R = (hostChans > 1) ? buffer.getWritePointer(1) : L;

    // ----- 0) Adaptive pop filter -----
    if (popThresholdDb < -0.01f) {
        if (popBassTemp.getNumSamples() < numSamples) {
            popBassTemp.setSize(numChans, numSamples + 16, false, true, true);
            popHighTemp.setSize(numChans, numSamples + 16, false, true, true);
        }
        // Per-channel split
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

        // Channel-summed envelope so plosives are detected coherently.
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
        // Recombine into the host buffer
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

    // ----- 1) Push input into dry-delay ring (used by air-band split) -----
    int dryBufLen = dryDelayCapacity;
    for (int i = 0; i < numSamples; ++i) {
        for (int c = 0; c < procChans; ++c) {
            const float* src = (c == 0) ? L : R;
            dryDelayBuffer.getWritePointer(c)[(size_t)dryDelayWritePos] = src[i];
        }
        dryDelayWritePos = (dryDelayWritePos + 1) % dryBufLen;
    }

    // ----- 2) Compute pitch ratio for this block, push into RubberBand -----
    float startRatio = smoothedRatio;
    for (int i = 0; i < numSamples; ++i)
        smoothedRatio += alpha * (targetRatio - smoothedRatio);
    float blockRatio = (startRatio + smoothedRatio) * 0.5f;
    blockRatio = juce::jlimit(0.25f, 4.0f, blockRatio);

    float ratioDeltaCents = 1200.0f * std::abs(std::log2(blockRatio / appliedRatio));
    if (ratioDeltaCents > 1.0f) {
        stretcher->setPitchScale((double)blockRatio);
        appliedRatio = blockRatio;
    }

    // Lookahead delay: stash live input, hand the engine the sample that
    // was written `lookaheadSamples_` ago. For mono input we still feed the
    // engine 2 channels to keep RubberBand state consistent.
    if (lookaheadOut.getNumSamples() < numSamples) {
        lookaheadOut.setSize(numChans, numSamples + 16, false, true, true);
    }
    for (int i = 0; i < numSamples; ++i) {
        for (int c = 0; c < numChans; ++c) {
            const float* src = (c == 0) ? L : R;
            lookaheadBuffer.getWritePointer(c)[(size_t)lookaheadWritePos] = src[i];
        }
        int readP = lookaheadWritePos - lookaheadSamples_;
        if (readP < 0) readP += lookaheadSize;
        for (int c = 0; c < numChans; ++c) {
            lookaheadOut.getWritePointer(c)[i] =
                lookaheadBuffer.getReadPointer(c)[(size_t)readP];
        }
        lookaheadWritePos = (lookaheadWritePos + 1) % lookaheadSize;
    }
    const float* inPtrs[kMaxChans];
    for (int c = 0; c < numChans; ++c) inPtrs[c] = lookaheadOut.getReadPointer(c);
    stretcher->process(inPtrs, (size_t)numSamples, false);

    // ----- 3) Drain RubberBand into FIFO -----
    int avail = (int)stretcher->available();
    if (avail > 0) {
        int writable = outputFifo.getFreeSpace();
        if (avail > writable) avail = writable;
        if (avail > rubberOut.getNumSamples()) avail = rubberOut.getNumSamples();
        if (avail > 0) {
            float* outPtrs[kMaxChans];
            for (int c = 0; c < numChans; ++c) outPtrs[c] = rubberOut.getWritePointer(c);
            stretcher->retrieve(outPtrs, (size_t)avail);

            int s1, sz1, s2, sz2;
            outputFifo.prepareToWrite(avail, s1, sz1, s2, sz2);
            for (int c = 0; c < numChans; ++c) {
                if (sz1 > 0) outputBuffer.copyFrom(c, s1, rubberOut.getReadPointer(c),       sz1);
                if (sz2 > 0) outputBuffer.copyFrom(c, s2, rubberOut.getReadPointer(c) + sz1, sz2);
            }
            outputFifo.finishedWrite(sz1 + sz2);
        }
    }

    // ----- 4) Read processed output from FIFO into the host buffer -----
    int ready  = outputFifo.getNumReady();
    int toRead = juce::jmin(numSamples, ready);
    if (toRead > 0) {
        int s1, sz1, s2, sz2;
        outputFifo.prepareToRead(toRead, s1, sz1, s2, sz2);
        for (int c = 0; c < numChans && c < hostChans; ++c) {
            float* dst = buffer.getWritePointer(c);
            if (sz1 > 0) std::copy_n(outputBuffer.getReadPointer(c, s1), sz1, dst);
            if (sz2 > 0) std::copy_n(outputBuffer.getReadPointer(c, s2), sz2, dst + sz1);
        }
        outputFifo.finishedRead(sz1 + sz2);
        for (int c = 0; c < procChans; ++c) {
            float* dst = (c == 0) ? L : R;
            lastOutSample[c] = dst[toRead - 1];
        }
    }
    if (toRead < numSamples) {
        float decay = 0.999f;
        for (int c = 0; c < procChans; ++c) {
            float* dst = (c == 0) ? L : R;
            float v = lastOutSample[c];
            for (int i = toRead; i < numSamples; ++i) {
                v *= decay;
                dst[i] = v;
            }
            lastOutSample[c] = v;
        }
    }

    // ----- 5) Air-band split: lowpass wet, add highpassed delayed dry -----
    for (int c = 0; c < procChans; ++c) {
        float* dst = (c == 0) ? L : R;
        juce::dsp::AudioBlock<float> wetBlock(&dst, 1, (size_t)numSamples);
        juce::dsp::ProcessContextReplacing<float> wetCtx(wetBlock);
        wetLowpass[c].process(wetCtx);
    }

    // Build delayed-dry highs in rubberOut (reuse as scratch — it has 2 rows).
    int readPos0 = dryDelayWritePos - dryDelayLength;
    while (readPos0 < 0) readPos0 += dryBufLen;
    for (int c = 0; c < procChans; ++c) {
        int rp = readPos0;
        float* tmp = rubberOut.getWritePointer(c);
        const float* dr = dryDelayBuffer.getReadPointer(c);
        for (int i = 0; i < numSamples; ++i) {
            tmp[i] = dr[(size_t)rp];
            rp = (rp + 1) % dryBufLen;
        }
        juce::dsp::AudioBlock<float> hiBlock(&tmp, 1, (size_t)numSamples);
        juce::dsp::ProcessContextReplacing<float> hiCtx(hiBlock);
        dryHighpass[c].process(hiCtx);

        float* dst = (c == 0) ? L : R;
        for (int i = 0; i < numSamples; ++i) dst[i] += tmp[i];
    }

    // ----- 6) Tone EQ (post) -----
    if (std::abs(currentBreathDb) > 0.01f || std::abs(currentSibilantsDb) > 0.01f) {
        for (int c = 0; c < procChans; ++c) {
            float* dst = (c == 0) ? L : R;
            juce::dsp::AudioBlock<float> tBlock(&dst, 1, (size_t)numSamples);
            juce::dsp::ProcessContextReplacing<float> tCtx(tBlock);
            if (std::abs(currentBreathDb)    > 0.01f) breathFilter[c].process(tCtx);
            if (std::abs(currentSibilantsDb) > 0.01f) sibilantsFilter[c].process(tCtx);
        }
    } else {
        for (int c = 0; c < procChans; ++c) {
            breathFilter[c].reset();
            sibilantsFilter[c].reset();
        }
    }

    // If host expects more channels than we processed, fill them.
    for (int c = procChans; c < hostChans; ++c) {
        std::copy_n(L, numSamples, buffer.getWritePointer(c));
    }

    // If host gave us mono only (procChans==1) but the underlying engine is
    // stereo, the L pointer already holds the processed signal — done.
}
