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
        
        // Positions in delay line
        float readPos1 = (float)writePos - (phase * currentWindowSize);
        if (readPos1 < 0) readPos1 += (float)bufSize;
        
        float readPos2 = (float)writePos - (phase2 * currentWindowSize);
        if (readPos2 < 0) readPos2 += (float)bufSize;
        
        // Interpolate
        int idx1 = (int)readPos1;
        float frac1 = readPos1 - (float)idx1;
        float val1 = delayBuffer[idx1] * (1.0f - frac1) + delayBuffer[(idx1 + 1) % bufSize] * frac1;
        
        int idx2 = (int)readPos2;
        float frac2 = readPos2 - (float)idx2;
        float val2 = delayBuffer[idx2] * (1.0f - frac2) + delayBuffer[(idx2 + 1) % bufSize] * frac2;
        
        // Windows (hanning-like crossfade)
        // A simple triangle or shifted cosine works well
        float win1 = std::sin(phase * juce::MathConstants<float>::pi);
        float win2 = std::sin(phase2 * juce::MathConstants<float>::pi);
        
        // Normalize
        float norm = win1 + win2;
        if (norm > 0.0001f) {
            win1 /= norm;
            win2 /= norm;
        }
        
        float outSample = val1 * win1 + val2 * win2;
        channelData[i] = outSample;
        
        // If stereo, copy to right
        if (buffer.getNumChannels() > 1) {
            buffer.getWritePointer(1)[i] = outSample;
        }
        
        writePos++;
        if (writePos >= bufSize) writePos = 0;
    }
}
