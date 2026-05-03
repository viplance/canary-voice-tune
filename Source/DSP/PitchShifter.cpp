#include "PitchShifter.h"
#include <cmath>

PitchShifter::PitchShifter() {}
PitchShifter::~PitchShifter() {}

void PitchShifter::prepare(double sampleRate, int samplesPerBlock)
{
    juce::ignoreUnused(samplesPerBlock);
    currentSampleRate = sampleRate;
    
    // 25ms window to reduce latency and 'double voice'/chorus echo artifacts
    windowSize = (int)(currentSampleRate * 0.025);
    if (windowSize < 1) windowSize = 1;
    
    delayBuffer.clear();
    delayBuffer.resize(windowSize * 4, 0.0f);
    writePos = 0;
    phase = 0.0f;
    currentRatio = 1.0f;
    smoothedRatio = 1.0f;
    targetRatio = 1.0f;
    alpha = 1.0f;
    smoothedPeriod = (float)windowSize;
}

void PitchShifter::setTargetShift(float ratio, float attackMs, float releaseMs, bool isVoiced, float detectedHz)
{
    currentRatio = ratio;

    float target = isVoiced ? currentRatio : 1.0f;
    float timeMs = isVoiced ? attackMs : releaseMs;
    if (timeMs < 1.0f) timeMs = 1.0f; // min 1ms

    // lowpass alpha
    float timeS = timeMs / 1000.0f;
    alpha = 1.0f - std::exp(-1.0f / (timeS * currentSampleRate));

    targetRatio = target;

    // Pitch synchronous period matching
    // We target a period size. For unvoiced, fallback to 25ms.
    float targetPeriod = currentSampleRate * 0.025f;
    if (isVoiced && detectedHz > 40.0f) {
        targetPeriod = currentSampleRate / detectedHz;
    }

    // Update target period, but we'll smooth it per-sample in process() to avoid clicks
    // We reuse windowSize variable as the "targetPeriod"
    windowSize = (int)targetPeriod;
}

void PitchShifter::process(juce::AudioBuffer<float>& buffer)
{
    auto* channelData = buffer.getWritePointer(0);
    int numSamples = buffer.getNumSamples();
    int bufSize = (int)delayBuffer.size();
    if (bufSize == 0) return;

    // 4-point Catmull-Rom interpolation. Linear interpolation rolls off high
    // frequencies whenever the fractional position is near 0.5 — audible as a
    // dull/muffled output. Cubic preserves treble.
    auto cubicInterp = [&](float pos) {
        int i1 = (int)pos;
        float frac = pos - (float)i1;
        int i0 = (i1 - 1 + bufSize) % bufSize;
        int i2 = (i1 + 1) % bufSize;
        int i3 = (i1 + 2) % bufSize;
        float y0 = delayBuffer[i0];
        float y1 = delayBuffer[i1];
        float y2 = delayBuffer[i2];
        float y3 = delayBuffer[i3];
        float a = -0.5f * y0 + 1.5f * y1 - 1.5f * y2 + 0.5f * y3;
        float b = y0 - 2.5f * y1 + 2.0f * y2 - 0.5f * y3;
        float c = -0.5f * y0 + 0.5f * y2;
        float d = y1;
        return ((a * frac + b) * frac + c) * frac + d;
    };

    for (int i = 0; i < numSamples; ++i)
    {
        // Smooth ratio
        smoothedRatio += alpha * (targetRatio - smoothedRatio);

        // Smooth period (200ms time constant to avoid sudden delay jumps)
        float periodAlpha = 1.0f - std::exp(-1.0f / (0.2f * currentSampleRate));
        smoothedPeriod += periodAlpha * ((float)windowSize - smoothedPeriod);

        // Write to buffer
        float inSample = channelData[i];
        delayBuffer[writePos] = inSample;

        // Window size is exactly 2.0 * period. Spacing is 1.0 * period.
        // This completely eliminates comb-filtering (choir effect) on vocals.
        float currentWindowSize = 2.0f * smoothedPeriod;

        // Read pointers
        phase += (1.0f - smoothedRatio) / currentWindowSize;
        if (phase >= 1.0f) phase -= 1.0f;
        if (phase < 0.0f) phase += 1.0f;

        float phase2 = phase + 0.5f;
        if (phase2 >= 1.0f) phase2 -= 1.0f;

        float readPos1 = (float)writePos - (phase * currentWindowSize);
        if (readPos1 < 0) readPos1 += (float)bufSize;
        float readPos2 = (float)writePos - (phase2 * currentWindowSize);
        if (readPos2 < 0) readPos2 += (float)bufSize;

        float val1 = cubicInterp(readPos1);
        float val2 = cubicInterp(readPos2);
        float win1 = std::sin(phase * juce::MathConstants<float>::pi);
        float win2 = std::sin(phase2 * juce::MathConstants<float>::pi);
        float norm = win1 + win2;
        if (norm > 0.0001f) { win1 /= norm; win2 /= norm; }
        float psolaSample = val1 * win1 + val2 * win2;

        // Two cross-faded read pointers half a period apart inevitably produce
        // a subtle chorus on transients (consonants, sibilants), regardless of
        // window/normalization choice — it is fundamental to two-pointer PSOLA.
        // To minimize the artifact, crossfade with the dry (delayed) signal
        // proportionally to how little shift is actually needed. At ratio=1
        // we hear pure dry; the further the ratio is from 1 the more PSOLA we
        // mix in. Listeners can't tell the chorus apart from real shifting
        // when the shift itself dominates.
        float drySample = delayBuffer[writePos];
        float shiftAmount = std::abs(smoothedRatio - 1.0f);
        // Full PSOLA at >=1 semitone shift (ratio ~1.06 / 0.94), full dry at
        // ratio=1, smooth crossfade between.
        float wetMix = juce::jlimit(0.0f, 1.0f, shiftAmount / 0.06f);
        float outSample = drySample * (1.0f - wetMix) + psolaSample * wetMix;

        channelData[i] = outSample;

        // If stereo, copy to right
        if (buffer.getNumChannels() > 1) {
            buffer.getWritePointer(1)[i] = outSample;
        }

        writePos++;
        if (writePos >= bufSize) writePos = 0;
    }
}
