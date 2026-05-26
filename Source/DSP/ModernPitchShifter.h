#pragma once

#include "PitchShifterBase.h"
#include <JuceHeader.h>
#include <memory>
#include <vector>
#include <array>

namespace RubberBand {
    class RubberBandStretcher;
}

class ModernPitchShifter : public PitchShifterBase
{
public:
    ModernPitchShifter();
    ~ModernPitchShifter() override;

    void prepare(double sampleRate, int samplesPerBlock) override;
    void process(juce::AudioBuffer<float>& buffer) override;
    int getLatencySamples() const override { return currentLatency; }

    void setTargetShift(float ratio, float attackMs, float releaseMs,
                        bool isVoiced, float detectedHz, float vibratoAmount) override;

    void setToneShaping(float sibilantsDb, float breathDb) override;
    void setExciter(float exciterDb, bool isConsonant) override;
    void setPopFilter(float thresholdDb) override;
    void setBreathGate(float thresholdDb, bool isBreathDetected) override;
    void triggerOnsetFade(float fadeMs) override;

    float getPopActivity() const override { return popActivity; }
    float getBreathActivity() const override { return breathActivity.load(); }

private:
    static constexpr int kMaxChans = 2;

    double currentSampleRate = 44100.0;
    int    numChans = 1;
    float currentRatio = 1.0f;
    float smoothedRatio = 1.0f;
    float targetRatio = 1.0f;
    float appliedRatio = 1.0f;
    float alpha = 0.01f;
    int currentLatency = 0;
    std::array<float, kMaxChans> lastOutSample { 0.0f, 0.0f };

    std::unique_ptr<RubberBand::RubberBandStretcher> stretcher;

    juce::AbstractFifo outputFifo { 131072 };
    juce::AudioBuffer<float> outputBuffer { kMaxChans, 131072 };

    std::array<juce::dsp::LinkwitzRileyFilter<float>, kMaxChans> dryHighpass;
    std::array<juce::dsp::LinkwitzRileyFilter<float>, kMaxChans> wetLowpass;
    juce::AudioBuffer<float> dryDelayBuffer;
    int dryDelayWritePos = 0;
    int dryDelayLength = 0;
    int dryDelayCapacity = 0;
    float crossoverHz = 5000.0f;

    std::array<juce::dsp::IIR::Filter<float>, kMaxChans> sibilantsFilter;
    juce::dsp::IIR::Coefficients<float>::Ptr sibilantsCoeffs;
    float currentSibilantsDb = 0.0f;
    static constexpr float kSibilantsHz = 7000.0f;

    float breathThresholdDb = 0.0f;
    bool isBreathActive = false;
    float breathGain = 1.0f;
    float breathAttackAlpha = 0.0f;
    float breathReleaseAlpha = 0.0f;
    std::atomic<float> breathActivity { 0.0f };
    static constexpr float kBreathDuckDb = -6.0f;

    std::vector<float> breathInputRms;
    std::vector<bool> breathGateDelay;
    int breathBlockIndex = 0;
    int blockSize = 0;

    float exciterDb = 0.0f;
    bool isConsonantActive = false;
    std::array<juce::dsp::IIR::Filter<float>, kMaxChans> exciterCrossover;
    std::array<juce::dsp::IIR::Filter<float>, kMaxChans> exciterHarmonicFilter;

    std::array<juce::dsp::LinkwitzRileyFilter<float>, kMaxChans> popLow;
    std::array<juce::dsp::LinkwitzRileyFilter<float>, kMaxChans> popHigh;
    float popThresholdDb = 0.0f;
    float popFastEnv    = 0.0f;
    float popSlowEnv    = 0.0f;
    float popGain       = 1.0f;
    float popFastAlpha  = 0.0f;
    float popSlowAlpha  = 0.0f;
    float popAttackAlpha  = 0.0f;
    float popReleaseAlpha = 0.0f;
    std::atomic<float> popActivity { 0.0f };
    static constexpr float kPopCrossoverHz = 150.0f;
    static constexpr float kPopDuckDb = -12.0f;
    juce::AudioBuffer<float> popBassTemp;
    juce::AudioBuffer<float> popHighTemp;

    int onsetFadeTotal     = 0;
    int onsetFadeRemaining = 0;
    int onsetFadeDelay     = 0;

    juce::AudioBuffer<float> lookaheadBuffer;
    juce::AudioBuffer<float> lookaheadOut;
    int lookaheadWritePos = 0;
    int lookaheadSize = 0;
    int lookaheadSamples_ = 0;

    juce::AudioBuffer<float> rubberOut;
};
