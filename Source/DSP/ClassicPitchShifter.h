#pragma once

#include "PitchShifterBase.h"
#include <JuceHeader.h>
#include <vector>
#include <array>
#include <atomic>

class ClassicPitchShifter : public PitchShifterBase
{
public:
    ClassicPitchShifter();
    ~ClassicPitchShifter() override;

    void prepare(double sampleRate, int samplesPerBlock) override;
    void process(juce::AudioBuffer<float>& buffer) override;
    int getLatencySamples() const override { return 0; } // Zero latency!

    void setTargetShift(float ratio, float attackMs, float releaseMs,
                        bool isVoiced, float detectedHz, float vibratoAmount) override;

    void setToneShaping(float sibilantsDb, float breathDb) override;
    void setExciter(float exciterDb, bool isConsonant) override;
    void setPopFilter(float thresholdDb) override;
    void setBreathGate(float thresholdDb, bool isBreathDetected) override;
    void triggerOnsetFade(float fadeMs) override;

    float getPopActivity() const override { return 0.0f; }
    float getBreathActivity() const override { return 0.0f; }

private:
    static constexpr int kMaxChans = 2;

    double currentSampleRate = 44100.0;
    int    numChans = 1;
    float currentRatio = 1.0f;
    float smoothedRatio = 1.0f;
    float targetRatio = 1.0f;
    float alpha = 0.01f;
    std::array<float, kMaxChans> lastOutSample { 0.0f, 0.0f };

    // --- Circular history buffer for time-domain cycle-drop/repeat resampling ---
    static const int kHistorySize = 131072;
    juce::AudioBuffer<float> historyBuffer;

    // Unprocessed input history, used only for correlation-based jump search.
    // kCorrHistorySize only needs to cover the search window + max period.
    static const int kCorrHistorySize = 4096;
    juce::AudioBuffer<float> corrHistoryBuffer; // channel 0 only (mono input)
    int corrWritePos = 0;
    int64_t absoluteWritePos = 0;
    std::array<double, kMaxChans> absoluteOutputAddr;
    std::array<double, kMaxChans> crossFadeOutputAddr;
    std::array<float, kMaxChans> crossFadeGain;
    // Onset crossfade: short so the tuned vowel starts crisply.
    static constexpr int kOnsetCrossFadeDuration = 128;
    // Cycle-repeat/drop crossfade: covers ~1.5 periods of the lowest supported
    // note (60 Hz -> period ~800 samples) while staying short enough that
    // overlapping crossfades don't corrupt the secondary pointer too far into
    // the past (which causes dropouts and polarity flips).
    static constexpr int kCrossFadeDuration = 256;
    
    // Voiced tracking
    bool isVoiced_ = false;
    bool isVoicedDebounced = false;
    int voicedBlockCounter = 0;
    int unvoicedBlockCounter = 0;
    std::array<bool, kMaxChans> wasVoicedState;
    float currentDetectedHz = 0.0f;
    std::array<double, kMaxChans> smoothedPeriodSamples;
    std::array<int, kMaxChans> samplesSinceLastJump;
    std::array<float, kMaxChans> prevR;              // ratio from previous sample, per channel
    std::array<int,   kMaxChans> strandedSamples;   // consecutive samples pointer is close to write head
    std::array<bool,  kMaxChans> crossFadeIsOnset;  // true if current crossfade is an onset transition
    std::array<int,   kMaxChans> unvoicedSamples;   // samples since this channel left voiced mode



    int onsetFadeTotal     = 0;
    int onsetFadeRemaining = 0;

    // Find the jump distance (near nominalPeriod) that maximises normalised
    // cross-correlation on the RAW input history (corrHistoryBuffer).  Using
    // raw input prevents correlation artefacts from previously shifted audio.
    // Search window: ±30 % of nominalPeriod.
    // Returns nominalPeriod unchanged if the signal is too quiet to correlate.
    double findOptimalJump(double nominalPeriod) const;

    // Cubic interpolation helper
    inline float interpolateCubic(const float* buffer, double pos) const
    {
        int64_t idx = (int64_t)std::floor(pos);
        double frac = pos - idx;
        
        float y0 = buffer[(idx - 1 + kHistorySize * 10) % kHistorySize];
        float y1 = buffer[(idx     + kHistorySize * 10) % kHistorySize];
        float y2 = buffer[(idx + 1 + kHistorySize * 10) % kHistorySize];
        float y3 = buffer[(idx + 2 + kHistorySize * 10) % kHistorySize];
        
        float a0 = -0.5f * y0 + 1.5f * y1 - 1.5f * y2 + 0.5f * y3;
        float a1 = y0 - 2.5f * y1 + 2.0f * y2 - 0.5f * y3;
        float a2 = -0.5f * y0 + 0.5f * y2;
        float a3 = y1;
        
        return a0 * (float)(frac * frac * frac) + a1 * (float)(frac * frac) + a2 * (float)frac + a3;
    }
};
