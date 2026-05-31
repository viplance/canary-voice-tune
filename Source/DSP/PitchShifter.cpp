#include "PitchShifter.h"

PitchShifter::PitchShifter()
{
    modernEngine = std::make_unique<ModernPitchShifter>();
    classicEngine = std::make_unique<ClassicPitchShifter>();
    activeEngine = modernEngine.get();
}

PitchShifter::~PitchShifter() {}

void PitchShifter::prepare(double sampleRate, int samplesPerBlock)
{
    storedSampleRate = sampleRate;
    storedSamplesPerBlock = samplesPerBlock;

    modernEngine->prepare(sampleRate, samplesPerBlock);
    classicEngine->prepare(sampleRate, samplesPerBlock);
    vocalEffects.prepare(sampleRate, samplesPerBlock);
}

void PitchShifter::reset()
{
    modernEngine->reset();
    classicEngine->reset();
    vocalEffects.reset();
}

void PitchShifter::process(juce::AudioBuffer<float>& buffer)
{
    if (activeEngine != nullptr) {
        vocalEffects.processPrePitch(buffer, activeEngine->getLatencySamples());
        activeEngine->process(buffer);
        vocalEffects.processPostPitch(buffer);
    }
}

int PitchShifter::getLatencySamples() const
{
    if (activeEngine != nullptr) {
        return activeEngine->getLatencySamples();
    }
    return 0;
}

void PitchShifter::setTargetShift(float ratio, float attackMs, float releaseMs,
                                  bool isVoiced, float detectedHz, float vibratoAmount)
{
    if (activeEngine != nullptr) {
        activeEngine->setTargetShift(ratio, attackMs, releaseMs, isVoiced, detectedHz, vibratoAmount);
    }
}

void PitchShifter::setToneShaping(float sibilantsDb, float breathDb)
{
    vocalEffects.setToneShaping(sibilantsDb, breathDb);
}

void PitchShifter::setExciter(float exciterDb, bool isConsonant)
{
    vocalEffects.setExciter(exciterDb, isConsonant);
}

void PitchShifter::setPopFilter(float thresholdDb)
{
    vocalEffects.setPopFilter(thresholdDb);
}

void PitchShifter::setBreathGate(float thresholdDb, bool isBreathDetected)
{
    vocalEffects.setBreathGate(thresholdDb, isBreathDetected);
}

void PitchShifter::triggerOnsetFade(float fadeMs)
{
    if (activeEngine != nullptr) {
        activeEngine->triggerOnsetFade(fadeMs);
    }
}

float PitchShifter::getPopActivity() const
{
    return vocalEffects.getPopActivity();
}

float PitchShifter::getBreathActivity() const
{
    return vocalEffects.getBreathActivity();
}

void PitchShifter::setTuningMode(int modeIndex)
{
    if (currentMode != modeIndex) {
        currentMode = modeIndex;
        if (currentMode == 0) {
            activeEngine = modernEngine.get();
        } else {
            activeEngine = classicEngine.get();
        }
        // Real-time safe state reset instead of full reallocation
        activeEngine->reset();
    }
}
