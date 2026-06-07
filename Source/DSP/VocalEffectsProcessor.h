#pragma once

#include <JuceHeader.h>
#include <array>
#include <vector>
#include <atomic>

class VocalEffectsProcessor
{
public:
    VocalEffectsProcessor();
    ~VocalEffectsProcessor();

    void prepare(double sampleRate, int samplesPerBlock);
    void reset();

    // 1. Pre-pitch processing: runs Pop filter & calculates dry input RMS for Breath gate
    void processPrePitch(juce::AudioBuffer<float>& buffer, int engineLatencySamples);

    // 2. Post-pitch processing: runs Exciter, Tone EQ, and applies Breath gate attenuation
    void processPostPitch(juce::AudioBuffer<float>& buffer);

    // Parameter setters
    void setToneShaping(float sibilantsDb, float breathDb);
    void setExciter(float exciterDb, bool isConsonant);
    void setPopFilter(float thresholdDb);
    void setBreathGate(float thresholdDb, bool isBreathDetected);

    // Activity indicators
    float getPopActivity() const { return popActivity.load(); }
    float getBreathActivity() const { return breathActivity.load(); }

private:
    static constexpr int kMaxChans = 2;

    double currentSampleRate = 44100.0;
    int numChans = 1;
    int blockSize = 0;

    // --- Pop Filter ---
    std::array<juce::dsp::LinkwitzRileyFilter<float>, kMaxChans> popLow;
    std::array<juce::dsp::LinkwitzRileyFilter<float>, kMaxChans> popHigh;
    float popThresholdDb = 0.0f;
    float popFastEnv = 0.0f;
    float popSlowEnv = 0.0f;
    float popGain = 1.0f;
    float popFastAlpha = 0.0f;
    float popSlowAlpha = 0.0f;
    float popAttackAlpha = 0.0f;
    float popReleaseAlpha = 0.0f;
    std::atomic<float> popActivity { 0.0f };
    static constexpr float kPopCrossoverHz = 150.0f;
    static constexpr float kPopDuckDb = -12.0f;
    juce::AudioBuffer<float> popBassTemp;
    juce::AudioBuffer<float> popHighTemp;

    // --- Sibilants EQ ---
    std::array<juce::dsp::IIR::Filter<float>, kMaxChans> sibilantsFilter;
    juce::dsp::IIR::Coefficients<float>::Ptr sibilantsCoeffs;
    float currentSibilantsDb = 9999.0f;
    static constexpr float kSibilantsHz = 7000.0f;

    // --- Vocal Exciter ---
    float exciterDb = 0.0f;
    bool isConsonantActive = false;
    std::array<juce::dsp::IIR::Filter<float>, kMaxChans> exciterCrossover;
    std::array<juce::dsp::IIR::Filter<float>, kMaxChans> exciterHarmonicFilter;

    // --- Breath Gate ---
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
};
