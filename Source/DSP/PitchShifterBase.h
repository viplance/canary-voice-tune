#pragma once
#include <JuceHeader.h>

class PitchShifterBase
{
public:
    virtual ~PitchShifterBase() = default;

    virtual void prepare(double sampleRate, int samplesPerBlock) = 0;
    virtual void process(juce::AudioBuffer<float>& buffer) = 0;
    virtual int getLatencySamples() const = 0;
    virtual void setTargetShift(float ratio, float attackMs, float releaseMs,
                                bool isVoiced, float detectedHz, float vibratoAmount) = 0;
    virtual void setToneShaping(float sibilantsDb, float breathDb) = 0;
    virtual void setExciter(float exciterDb, bool isConsonant) = 0;
    virtual void setPopFilter(float thresholdDb) = 0;
    virtual void setBreathGate(float thresholdDb, bool isBreathDetected) = 0;
    virtual void triggerOnsetFade(float fadeMs) = 0;
    virtual float getPopActivity() const = 0;
    virtual float getBreathActivity() const = 0;
};
