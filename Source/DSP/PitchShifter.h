#pragma once

#include "PitchShifterBase.h"
#include "ModernPitchShifter.h"
#include "ClassicPitchShifter.h"
#include "VocalEffectsProcessor.h"
#include <memory>

class PitchShifter : public PitchShifterBase
{
public:
    PitchShifter();
    ~PitchShifter() override;

    void prepare(double sampleRate, int samplesPerBlock) override;
    void reset() override;
    void process(juce::AudioBuffer<float>& buffer) override;
    int getLatencySamples() const override;

    void setTargetShift(float ratio, float attackMs, float releaseMs,
                        bool isVoiced, float detectedHz, float vibratoAmount) override;

    void setToneShaping(float sibilantsDb, float breathDb) override;
    void setExciter(float exciterDb, bool isConsonant) override;
    void setPopFilter(float thresholdDb) override;
    void setBreathGate(float thresholdDb, bool isBreathDetected) override;
    void triggerOnsetFade(float fadeMs) override;

    float getPopActivity() const override;
    float getBreathActivity() const override;

    // --- Dynamic Tuning Mode Toggling (RT-Safe) ---
    void setTuningMode(int modeIndex); // 0 = Modern, 1 = Classic
    int getTuningMode() const { return currentMode; }

private:
    std::unique_ptr<ModernPitchShifter> modernEngine;
    std::unique_ptr<ClassicPitchShifter> classicEngine;
    VocalEffectsProcessor vocalEffects;
    
    int currentMode = 0; // 0 = Modern, 1 = Classic
    PitchShifterBase* activeEngine = nullptr;

    double storedSampleRate = 44100.0;
    int storedSamplesPerBlock = 512;
};
