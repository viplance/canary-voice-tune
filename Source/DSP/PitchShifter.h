#pragma once

#include <JuceHeader.h>
#include <memory>
#include <vector>

namespace RubberBand {
    class RubberBandStretcher;
}

class PitchShifter
{
public:
    PitchShifter();
    ~PitchShifter();

    void prepare(double sampleRate, int samplesPerBlock);
    
    // Applies pitch shift directly to the buffer.
    void process(juce::AudioBuffer<float>& buffer);

    int getLatencySamples() const { return currentLatency; }

    void setTargetShift(float ratio, float attackMs, float releaseMs, bool isVoiced, float detectedHz);

private:
    double currentSampleRate = 44100.0;
    float currentRatio = 1.0f;
    float smoothedRatio = 1.0f;
    float targetRatio = 1.0f;
    float appliedRatio = 1.0f;   // last value pushed to RubberBand
    float alpha = 0.01f;
    int currentLatency = 0;
    float lastOutSample = 0.0f;  // for underrun hold-and-decay

    std::unique_ptr<RubberBand::RubberBandStretcher> stretcher;

    juce::AbstractFifo outputFifo { 131072 };
    juce::AudioBuffer<float> outputBuffer { 1, 131072 };

    std::vector<float> tempIn;
    std::vector<float> tempOut;
};
