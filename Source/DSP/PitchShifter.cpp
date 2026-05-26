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
}

void PitchShifter::process(juce::AudioBuffer<float>& buffer)
{
    if (activeEngine != nullptr) {
        activeEngine->process(buffer);
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
    if (activeEngine != nullptr) {
        activeEngine->setToneShaping(sibilantsDb, breathDb);
    }
}

void PitchShifter::setExciter(float exciterDb, bool isConsonant)
{
    if (activeEngine != nullptr) {
        activeEngine->setExciter(exciterDb, isConsonant);
    }
}

void PitchShifter::setPopFilter(float thresholdDb)
{
    if (activeEngine != nullptr) {
        activeEngine->setPopFilter(thresholdDb);
    }
}

void PitchShifter::setBreathGate(float thresholdDb, bool isBreathDetected)
{
    if (activeEngine != nullptr) {
        activeEngine->setBreathGate(thresholdDb, isBreathDetected);
    }
}

void PitchShifter::triggerOnsetFade(float fadeMs)
{
    if (activeEngine != nullptr) {
        activeEngine->triggerOnsetFade(fadeMs);
    }
}

float PitchShifter::getPopActivity() const
{
    if (activeEngine != nullptr) {
        return activeEngine->getPopActivity();
    }
    return 0.0f;
}

float PitchShifter::getBreathActivity() const
{
    if (activeEngine != nullptr) {
        return activeEngine->getBreathActivity();
    }
    return 0.0f;
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
        // Prepare on engine switch to ensure fresh history
        activeEngine->prepare(storedSampleRate, storedSamplesPerBlock);
    }
}
