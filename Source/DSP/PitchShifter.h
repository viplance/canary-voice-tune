#pragma once

#include <JuceHeader.h>
#include <vector>

class PitchShifter
{
public:
    PitchShifter();
    ~PitchShifter();

    void prepare(double sampleRate, int samplesPerBlock);
    
    // Applies pitch shift directly to the buffer.
    void process(juce::AudioBuffer<float>& buffer);

    void setTargetShift(float ratio, float attackMs, float releaseMs, bool isVoiced, float detectedHz);

private:
    double currentSampleRate = 44100.0;
    float currentRatio = 1.0f;
    float smoothedRatio = 1.0f;
    float targetRatio = 1.0f;
    float alpha = 0.01f;
    float smoothedPeriod = 1000.0f;
    
    std::vector<float> delayBuffer;
    int writePos = 0;
    float phase = 0.0f;
    int windowSize = 2048; 
};
