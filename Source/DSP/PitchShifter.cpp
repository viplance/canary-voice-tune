#include "PitchShifter.h"
#include <rubberband/RubberBandStretcher.h>
#include <cmath>
#include <algorithm>

PitchShifter::PitchShifter() {}
PitchShifter::~PitchShifter() {}

void PitchShifter::prepare(double sampleRate, int samplesPerBlock)
{
    juce::ignoreUnused(samplesPerBlock);
    currentSampleRate = sampleRate;

    currentRatio = 1.0f;
    smoothedRatio = 1.0f;
    targetRatio = 1.0f;
    appliedRatio = 1.0f;
    alpha = 1.0f;

    // R3 engine ("Finer") — explicitly designed by the RubberBand author
    // for vocals and material with smooth, soft onsets and time-varying
    // pitch shifts. It's substantially more CPU-intensive than R2 but the
    // sound quality is in a different league for sustained vocal tones,
    // and it's stable under continuous setPitchScale changes (the exact
    // workload of auto-tune).
    //
    // Pairings inside R3:
    //   - OptionPitchHighConsistency: required for time-varying pitch
    //     without phase artefacts; works correctly across the 1.0 boundary.
    //   - OptionFormantPreserved: preserves vocal formant envelope so
    //     shifted vowels still sound like the same singer, not chipmunked.
    //     R3 implements formant preservation via cepstral analysis, which
    //     is much higher quality than R2's version (R2's formant code was
    //     a click source — R3's is not).
    //   - OptionWindowStandard: enables R3's full multi-resolution scheme
    //     (different FFT sizes for different frequency bands) — the main
    //     source of R3's quality advantage.
    //   - OptionChannelsTogether: better mono compatibility (we sum to
    //     mono after processing anyway, so this avoids stereo phase
    //     issues).
    RubberBand::RubberBandStretcher::Options options =
        RubberBand::RubberBandStretcher::OptionProcessRealTime |
        RubberBand::RubberBandStretcher::OptionEngineFiner |
        RubberBand::RubberBandStretcher::OptionPitchHighConsistency |
        RubberBand::RubberBandStretcher::OptionFormantPreserved |
        RubberBand::RubberBandStretcher::OptionWindowStandard |
        RubberBand::RubberBandStretcher::OptionChannelsTogether;

    stretcher = std::make_unique<RubberBand::RubberBandStretcher>(
        (size_t)currentSampleRate, 1, options);
    stretcher->setMaxProcessSize((size_t)samplesPerBlock);
    stretcher->setPitchScale(1.0);

    outputFifo.reset();
    outputBuffer.clear();

    // R3 has a larger and more variable buffering delay than R2 — give it
    // generous headroom so we never starve the output FIFO. The slack here
    // is what protects against any residual click on rapid pitch changes.
    int delay = (int)stretcher->getStartDelay();
    int slack = juce::jmax(16384, samplesPerBlock * 16);
    int prefill = delay + slack;
    currentLatency = prefill;

    int start1, size1, start2, size2;
    outputFifo.prepareToWrite(prefill, start1, size1, start2, size2);
    if (size1 > 0) outputBuffer.clear(0, start1, size1);
    if (size2 > 0) outputBuffer.clear(0, start2, size2);
    outputFifo.finishedWrite(size1 + size2);

    tempOut.resize(131072, 0.0f);
    lastOutSample = 0.0f;

    // Crossover filters: Linkwitz-Riley LR4 (24dB/oct) gives a perfectly
    // flat magnitude response when the low and high outputs are summed
    // back, so a dry signal split-then-summed equals the original. We use
    // the same crossover frequency for the wet lowpass and the dry highpass
    // so the recombination at the output is also flat.
    juce::dsp::ProcessSpec spec;
    spec.sampleRate = currentSampleRate;
    spec.maximumBlockSize = (juce::uint32)samplesPerBlock;
    spec.numChannels = 1;
    dryHighpass.prepare(spec);
    wetLowpass.prepare(spec);
    dryHighpass.setType(juce::dsp::LinkwitzRileyFilterType::highpass);
    wetLowpass.setType(juce::dsp::LinkwitzRileyFilterType::lowpass);
    dryHighpass.setCutoffFrequency(crossoverHz);
    wetLowpass.setCutoffFrequency(crossoverHz);
    dryHighpass.reset();
    wetLowpass.reset();

    // Dry delay must equal the wet path's total latency so that the high
    // band realigns with the shifted low/mid band when summed.
    dryDelayLength = currentLatency;
    if (dryDelayLength < 1) dryDelayLength = 1;
    dryDelayBuffer.assign((size_t)dryDelayLength + 16, 0.0f);
    dryDelayWritePos = 0;
}

void PitchShifter::setTargetShift(float ratio, float attackMs, float releaseMs, bool isVoiced, float detectedHz)
{
    juce::ignoreUnused(detectedHz);
    currentRatio = ratio;

    float target = isVoiced ? currentRatio : 1.0f;
    float timeMs = isVoiced ? attackMs : releaseMs;
    if (timeMs < 1.0f) timeMs = 1.0f;

    float timeS = timeMs / 1000.0f;
    alpha = 1.0f - std::exp(-1.0f / (timeS * currentSampleRate));
    targetRatio = target;
}

void PitchShifter::process(juce::AudioBuffer<float>& buffer)
{
    if (stretcher == nullptr) return;

    int numSamples = buffer.getNumSamples();
    auto* channelData = buffer.getWritePointer(0);

    // 1) Stash dry input into the delay line and pre-extract its
    //    high-frequency component. The high-band needs to come out exactly
    //    in sync with the shifted low/mid coming back from RubberBand.
    if ((int)tempIn.size() < numSamples) tempIn.resize((size_t)numSamples * 2);
    std::copy_n(channelData, numSamples, tempIn.data());

    // Push the unfiltered dry into the delay line; we'll filter on read so
    // the highpass operates on a continuous stream without splice points.
    int dryBufLen = (int)dryDelayBuffer.size();
    for (int i = 0; i < numSamples; ++i) {
        dryDelayBuffer[(size_t)dryDelayWritePos] = tempIn[(size_t)i];
        dryDelayWritePos = (dryDelayWritePos + 1) % dryBufLen;
    }

    // Compute average ratio across this block. We don't update RubberBand's
    // pitch scale per-sample — that would tax the engine. We update once per
    // block, but only when the change exceeds a small threshold, because each
    // setPitchScale call inside the engine momentarily disturbs the OLA seam
    // and that's an audible micro-click on every block.
    float startRatio = smoothedRatio;
    for (int i = 0; i < numSamples; ++i) {
        smoothedRatio += alpha * (targetRatio - smoothedRatio);
    }
    float blockRatio = (startRatio + smoothedRatio) * 0.5f;
    if (blockRatio < 0.25f) blockRatio = 0.25f;
    if (blockRatio > 4.0f)  blockRatio = 4.0f;

    // R3 + HighConsistency handles continuous pitch changes cleanly, so
    // we only filter out truly negligible deltas (< 1 cent) — enough to
    // skip redundant engine work without losing pitch tracking precision.
    float ratioDeltaCents = 1200.0f * std::abs(std::log2(blockRatio / appliedRatio));
    if (ratioDeltaCents > 1.0f) {
        stretcher->setPitchScale((double)blockRatio);
        appliedRatio = blockRatio;
    }

    // Push input samples into RubberBand
    const float* inPtrs[1] = { channelData };
    stretcher->process(inPtrs, (size_t)numSamples, false);

    // Drain everything RubberBand has ready into our output FIFO. Doing this
    // greedily (rather than only when we need numSamples) keeps the FIFO
    // well-stocked so we never underrun mid-block.
    int avail = (int)stretcher->available();
    if (avail > 0) {
        int writable = outputFifo.getFreeSpace();
        if (avail > writable) avail = writable;
        if (avail > (int)tempOut.size()) avail = (int)tempOut.size();
        if (avail > 0) {
            float* outPtrs[1] = { tempOut.data() };
            stretcher->retrieve(outPtrs, (size_t)avail);

            int start1, size1, start2, size2;
            outputFifo.prepareToWrite(avail, start1, size1, start2, size2);
            if (size1 > 0) outputBuffer.copyFrom(0, start1, tempOut.data(), size1);
            if (size2 > 0) outputBuffer.copyFrom(0, start2, tempOut.data() + size1, size2);
            outputFifo.finishedWrite(size1 + size2);
        }
    }

    // Read numSamples from the FIFO into the output buffer.
    int ready = outputFifo.getNumReady();
    int toRead = juce::jmin(numSamples, ready);
    if (toRead > 0) {
        int start1, size1, start2, size2;
        outputFifo.prepareToRead(toRead, start1, size1, start2, size2);
        if (size1 > 0) std::copy_n(outputBuffer.getReadPointer(0, start1), size1, channelData);
        if (size2 > 0) std::copy_n(outputBuffer.getReadPointer(0, start2), size2, channelData + size1);
        outputFifo.finishedRead(size1 + size2);
        lastOutSample = channelData[toRead - 1];
    }

    // Underrun handling: if FIFO didn't have enough samples, hold the last
    // good sample with a short decay rather than zero-padding (which would
    // produce a very audible click). This is rare but happens around large
    // pitch jumps where RubberBand temporarily produces less output than it
    // consumes.
    if (toRead < numSamples) {
        float decay = 0.999f;
        for (int i = toRead; i < numSamples; ++i) {
            lastOutSample *= decay;
            channelData[i] = lastOutSample;
        }
    }

    // 2) Apply the air-band crossover. Lowpass the wet (RubberBand) output
    //    and add a time-aligned highpass of the dry input. Result: shifted
    //    body up to crossoverHz, original treble above. Linkwitz-Riley LR4
    //    sums to flat magnitude, so unshifted material reconstructs exactly.
    juce::dsp::AudioBlock<float> wetBlock(&channelData, 1, (size_t)numSamples);
    juce::dsp::ProcessContextReplacing<float> wetCtx(wetBlock);
    wetLowpass.process(wetCtx);

    // Read the delayed dry into a temp buffer and highpass it.
    if ((int)tempOut.size() < numSamples) tempOut.resize((size_t)numSamples);
    int readPos = dryDelayWritePos - dryDelayLength;
    while (readPos < 0) readPos += dryBufLen;
    for (int i = 0; i < numSamples; ++i) {
        tempOut[(size_t)i] = dryDelayBuffer[(size_t)readPos];
        readPos = (readPos + 1) % dryBufLen;
    }
    float* dryHighData = tempOut.data();
    juce::dsp::AudioBlock<float> hiBlock(&dryHighData, 1, (size_t)numSamples);
    juce::dsp::ProcessContextReplacing<float> hiCtx(hiBlock);
    dryHighpass.process(hiCtx);

    // Sum dry highs onto the wet lows.
    for (int i = 0; i < numSamples; ++i) {
        channelData[i] += dryHighData[i];
    }

    // Mirror to right channel
    if (buffer.getNumChannels() > 1) {
        auto* rightData = buffer.getWritePointer(1);
        std::copy_n(channelData, numSamples, rightData);
    }
}
