#pragma once

#include <JuceHeader.h>
#include <memory>
#include <vector>
#include <array>

namespace RubberBand {
    class RubberBandStretcher;
}

class PitchShifter
{
public:
    PitchShifter();
    ~PitchShifter();

    void prepare(double sampleRate, int samplesPerBlock);

    // Applies pitch shift directly to the buffer. Mono and stereo are both
    // supported — stereo input is shifted with synchronised phase across
    // both channels (RubberBand OptionChannelsTogether) so the panorama is
    // preserved.
    void process(juce::AudioBuffer<float>& buffer);

    int getLatencySamples() const { return currentLatency; }

    void setTargetShift(float ratio, float attackMs, float releaseMs, bool isVoiced, float detectedHz);

    void setToneShaping(float sibilantsDb, float breathDb);
    void setPopFilter(float thresholdDb);
    void triggerOnsetFade(float fadeMs);

    float getPopActivity() const { return popActivity; }

private:
    static constexpr int kMaxChans = 2;

    double currentSampleRate = 44100.0;
    int    numChans = 1;            // configured at prepare()
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

    // Air-band bypass: dry highs (>5kHz) bypass RubberBand to retain treble.
    // One filter pair per channel — they hold per-channel state.
    std::array<juce::dsp::LinkwitzRileyFilter<float>, kMaxChans> dryHighpass;
    std::array<juce::dsp::LinkwitzRileyFilter<float>, kMaxChans> wetLowpass;
    juce::AudioBuffer<float> dryDelayBuffer;   // ring buffer, kMaxChans rows
    int dryDelayWritePos = 0;
    int dryDelayLength = 0;
    int dryDelayCapacity = 0;
    float crossoverHz = 5000.0f;

    // Tone EQ — per-channel.
    std::array<juce::dsp::IIR::Filter<float>, kMaxChans> sibilantsFilter;
    std::array<juce::dsp::IIR::Filter<float>, kMaxChans> breathFilter;
    juce::dsp::IIR::Coefficients<float>::Ptr sibilantsCoeffs;
    juce::dsp::IIR::Coefficients<float>::Ptr breathCoeffs;
    float currentSibilantsDb = 0.0f;
    float currentBreathDb    = 0.0f;
    static constexpr float kSibilantsHz = 7000.0f;
    static constexpr float kBreathHz    = 3000.0f;
    static constexpr float kBreathQ     = 0.9f;

    // Adaptive pop filter (input pre-processing).
    // Crossover is per-channel; envelope follower runs on the channel-summed
    // bass band so a plosive triggers all channels coherently.
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
    juce::AudioBuffer<float> popBassTemp;   // kMaxChans rows
    juce::AudioBuffer<float> popHighTemp;   // kMaxChans rows

    // Onset fade
    int onsetFadeTotal     = 0;
    int onsetFadeRemaining = 0;
    int onsetFadeDelay     = 0;

    // Per-channel lookahead delay (between live input and what gets handed
    // to RubberBand). All channels are delayed by exactly the same amount
    // so stereo phase remains intact.
    juce::AudioBuffer<float> lookaheadBuffer; // ring buffer, kMaxChans rows
    juce::AudioBuffer<float> lookaheadOut;    // scratch for delayed input
    int lookaheadWritePos = 0;
    int lookaheadSize = 0;
    int lookaheadSamples_ = 0;

    // Scratch
    juce::AudioBuffer<float> rubberOut;       // tempOut, kMaxChans rows
};
