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

    // Sibilants: high-shelf gain (dB) around 7 kHz, controls "s/sh/t" presence.
    // Breath: bell gain (dB) around 3 kHz, controls breathiness/air.
    void setToneShaping(float sibilantsDb, float breathDb);

    // Pop filter: trigger threshold (dB) for the plosive detector. 0 dB
    // disables the filter; values down to -24 dB make it trigger on
    // progressively quieter plosives.
    void setPopFilter(float thresholdDb);

    // Tell the shifter that a new voicing onset just happened. The shifter
    // will crossfade dry -> wet over the next `fadeMs` milliseconds so that
    // the slewing pitch ratio at the very start of the phrase is hidden
    // (audible "swoop" / quack at low Attack values).
    void triggerOnsetFade(float fadeMs);

    // Returns 0..1 = current pop-ducking activity, for UI lamp display.
    // 1 = fully ducking right now, 0 = idle.
    float getPopActivity() const { return popActivity; }

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

    // High-frequency air-band bypass: the input is split via a high-shelf
    // crossover. The low/mid band goes through RubberBand (which loses some
    // high-frequency content via its formant cepstral analysis); the high
    // band is delayed by the engine's latency and mixed back in dry. This
    // restores the original treble while keeping the formant-preserved
    // pitch shift on the harmonic body.
    juce::dsp::LinkwitzRileyFilter<float> dryHighpass;   // extract dry highs from input
    juce::dsp::LinkwitzRileyFilter<float> wetLowpass;    // remove duplicated highs from wet
    std::vector<float> dryDelayBuffer;
    int dryDelayWritePos = 0;
    int dryDelayLength = 0;
    float crossoverHz = 5000.0f;

    // Tone-shaping EQ applied at the very end of the chain.
    // Sibilants: high-shelf at 7 kHz (consonant clarity).
    // Breath: bell at 3 kHz (presence/air on breath noise).
    juce::dsp::IIR::Filter<float> sibilantsFilter;
    juce::dsp::IIR::Filter<float> breathFilter;
    float currentSibilantsDb = 0.0f;
    float currentBreathDb    = 0.0f;
    static constexpr float kSibilantsHz = 7000.0f;
    static constexpr float kBreathHz    = 3000.0f;
    static constexpr float kBreathQ     = 0.9f;

    // Adaptive pop filter (input-side, runs before RubberBand):
    //   - Splits the input via Linkwitz-Riley LR4 at ~150 Hz into bass and
    //     non-bass bands.
    //   - Runs a transient detector on the bass band: a fast envelope follower
    //     compared against a slow baseline. A sharp ratio spike => plosive.
    //   - Smoothly ducks the bass band's gain when a plosive is detected,
    //     then restores it. Only the bass band is touched, so the body of
    //     the voice is unaffected.
    juce::dsp::LinkwitzRileyFilter<float> popLow;
    juce::dsp::LinkwitzRileyFilter<float> popHigh;
    float popThresholdDb = 0.0f;    // user setting; 0 dB = bypass
    float popFastEnv    = 0.0f;     // fast peak envelope of bass band
    float popSlowEnv    = 0.0f;     // slow baseline envelope of bass band
    float popGain       = 1.0f;     // current bass-band gain (1.0 = no duck)
    float popFastAlpha  = 0.0f;     // computed in prepare
    float popSlowAlpha  = 0.0f;
    float popAttackAlpha  = 0.0f;
    float popReleaseAlpha = 0.0f;
    std::atomic<float> popActivity { 0.0f }; // 0..1 for UI lamp
    static constexpr float kPopCrossoverHz = 150.0f;
    // When triggered, bass band is ducked by this many dB regardless of
    // user setting — the user setting controls *how readily* the filter
    // triggers (threshold), not how deep it ducks.
    static constexpr float kPopDuckDb = -12.0f;
    std::vector<float> popBassTemp;
    std::vector<float> popHighTemp;

    // Onset fade-in: at the start of each voiced segment, the wet output is
    // hidden behind dry while the smoothedRatio is racing to its target.
    // This eliminates the "swoop"/quack audible at low Attack values when the
    // user hears the engine ramping in real time. After onsetFadeRemaining
    // counts down to zero, output is pure wet.
    int   onsetFadeTotal     = 0; // total samples of the fade
    int   onsetFadeRemaining = 0; // samples left
    int   onsetFadeDelay     = 0; // samples to wait before starting the fade
                                  // (== wet path latency, so the fade lands
                                  // exactly when the onset arrives at output)

    // Lookahead delay between the live input the processor analyses
    // (PitchDetector + ratio computation) and the input we actually feed
    // into RubberBand. This gives the engine time to "see" the correct
    // pitch ratio applied via setPitchScale BEFORE the audio for that pitch
    // even reaches the stretcher — eliminating the start-of-phrase swoop
    // that was audible at very low Attack values.
    std::vector<float> lookaheadBuffer;
    std::vector<float> lookaheadOut;     // scratch for delayed input
    int lookaheadWritePos = 0;
    int lookaheadSize = 0;
    int lookaheadSamples_ = 0;
};
