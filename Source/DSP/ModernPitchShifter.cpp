#include "ModernPitchShifter.h"
#include <rubberband/RubberBandStretcher.h>
#include <cmath>
#include <algorithm>

ModernPitchShifter::ModernPitchShifter() {}
ModernPitchShifter::~ModernPitchShifter() {}

void ModernPitchShifter::prepare(double sampleRate, int samplesPerBlock)
{
    currentSampleRate = sampleRate;
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
        RubberBand::RubberBandStretcher::OptionPhaseLaminar |
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

    juce::dsp::ProcessSpec spec;
    spec.sampleRate = currentSampleRate;
    spec.maximumBlockSize = (juce::uint32)samplesPerBlock;
    spec.numChannels = 1;

    for (int c = 0; c < numChans; ++c) {
        dryHighpass[c].prepare(spec);
        wetLowpass[c].prepare(spec);
        dryHighpass[c].setType(juce::dsp::LinkwitzRileyFilterType::highpass);
        wetLowpass[c].setType(juce::dsp::LinkwitzRileyFilterType::lowpass);
        dryHighpass[c].setCutoffFrequency(crossoverHz);
        wetLowpass[c].setCutoffFrequency(crossoverHz);
        dryHighpass[c].reset();
        wetLowpass[c].reset();
    }

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

void ModernPitchShifter::setToneShaping(float sibilantsDb, float breathDb)
{
    juce::ignoreUnused(sibilantsDb, breathDb);
}

void ModernPitchShifter::setExciter(float exciterDb, bool isConsonant)
{
    juce::ignoreUnused(exciterDb, isConsonant);
}

void ModernPitchShifter::setPopFilter(float thresholdDb)
{
    juce::ignoreUnused(thresholdDb);
}

void ModernPitchShifter::setBreathGate(float thresholdDb, bool isBreathDetected)
{
    juce::ignoreUnused(thresholdDb, isBreathDetected);
}

void ModernPitchShifter::triggerOnsetFade(float fadeMs)
{
    if (fadeMs < 0.5f) fadeMs = 0.5f;
    int n = (int)((float)currentSampleRate * fadeMs / 1000.0f);
    if (n < 16) n = 16;
    onsetFadeTotal     = n;
    onsetFadeRemaining = n;
    onsetFadeDelay     = currentLatency;
}

void ModernPitchShifter::setTargetShift(float ratio, float attackMs, float releaseMs,
                                        bool isVoiced, float detectedHz,
                                        float vibratoAmount)
{
    juce::ignoreUnused(detectedHz, releaseMs);
    currentRatio = ratio;

    float target = isVoiced ? currentRatio : 1.0f;
    float timeMs = isVoiced ? attackMs : 5.0f; // Fixed extremely fast 5ms release to prevent "quacking"
    float fastMs = 12.0f;
    float trackingMs = fastMs + (timeMs - fastMs) * vibratoAmount;
    if (trackingMs < 1.0f) trackingMs = 1.0f;
    float timeS = trackingMs / 1000.0f;
    alpha = 1.0f - std::exp(-1.0f / (timeS * currentSampleRate));
    targetRatio = target;
}

void ModernPitchShifter::process(juce::AudioBuffer<float>& buffer)
{
    if (stretcher == nullptr) return;

    int numSamples  = buffer.getNumSamples();
    int hostChans   = buffer.getNumChannels();
    int procChans   = juce::jmin(numChans, hostChans > 0 ? hostChans : 1);

    auto* L = buffer.getWritePointer(0);
    auto* R = (hostChans > 1) ? buffer.getWritePointer(1) : L;

    // ----- 1) Push input into dry-delay ring -----
    int dryBufLen = dryDelayCapacity;
    for (int i = 0; i < numSamples; ++i) {
        for (int c = 0; c < procChans; ++c) {
            const float* src = (c == 0) ? L : R;
            dryDelayBuffer.getWritePointer(c)[(size_t)dryDelayWritePos] = src[i];
        }
        dryDelayWritePos = (dryDelayWritePos + 1) % dryBufLen;
    }

    // ----- 2) Compute pitch ratio -----
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

    for (int c = procChans; c < hostChans; ++c) {
        std::copy_n(L, numSamples, buffer.getWritePointer(c));
    }
}
