#include "Test_LongMelody.h"
#include "TestHelpers.h"
#include "../Source/DSP/PitchDetector.h"
#include "../Source/DSP/PitchShifter.h"
#include "../Source/DSP/NoteSelector.h"
#include <iostream>
#include <cmath>
#include <vector>

// Renders long_melody.wav through the full plugin signal path — PitchDetector
// -> note selection / lock -> tuning ratio -> real PitchShifter — once per
// shift engine, writing test/result/long_melody_modern.wav (engine 0) and
// test/result/long_melody_classic.wav (engine 1) so the tuned result can be
// auditioned. The tuning math mirrors
// CanaryVoiceTuneAudioProcessor::chooseTargetNoteAndRatio (note matching is
// always maximal — no Range control). All 88 keys enabled (plugin default).

namespace {

// Per-voiced-segment tuning state (mirrors the processor members).
struct TuneState {
    float smoothedMidi = -1.0f;
    float smoothedTargetMidi = -1.0f;
    int   lockedMidi = -1;
    int   lockEngageSamples = 0;
    int   candidateMidi = -1;
    int   candidateStableSamples = 0;
    long  voicedSampleCount = 0;
    void reset() { *this = TuneState{}; }
};

// Mirror of chooseTargetNoteAndRatio (hard tune, no Range): returns ratio.
float computeRatio(TuneState& s, float detectedHz, const bool* activeKeys,
                   float blockSize, float sr, float attackMs, float vibratoAmount)
{
    s.voicedSampleCount += (long)blockSize;
    float floatMidi = 69.0f + 12.0f * std::log2(detectedHz / 440.0f);

    float blockDt = blockSize / sr;
    const float vibTimeMs = 201.0f;
    if (s.smoothedMidi < 0.0f) {
        s.smoothedMidi = floatMidi;
    } else {
        float delta = std::abs(floatMidi - s.smoothedMidi);
        float jumpLo = vibratoAmount + 0.5f, jumpHi = vibratoAmount + 1.2f;
        float jumpFactor = (delta < jumpLo) ? 0.0f
                         : (delta > jumpHi) ? 1.0f
                         : (delta - jumpLo) / (jumpHi - jumpLo);
        float effectiveTimeMs = vibTimeMs * (1.0f - jumpFactor) + 1.0f * jumpFactor;
        float adaptiveAlpha = 1.0f - std::exp(-blockDt / (effectiveTimeMs / 1000.0f));
        s.smoothedMidi += adaptiveAlpha * (floatMidi - s.smoothedMidi);
    }
    float deviation  = floatMidi - s.smoothedMidi;
    float clampedDev = std::max(-vibratoAmount, std::min(vibratoAmount, deviation));
    float effectiveMidi = s.smoothedMidi + clampedDev;

    const float hysteresisSt = 0.1f;
    float lockPitch = (s.smoothedMidi > 0.0f) ? s.smoothedMidi : effectiveMidi;
    int bestMidi = NoteSelector::chooseActiveNote(activeKeys, lockPitch, s.lockedMidi, hysteresisSt);
    if (bestMidi < 0) bestMidi = (int)std::round(lockPitch);

    const int lockGraceSamples    = (int)(sr * 0.050f);
    const int reLockStableSamples = (int)(sr * std::max(attackMs, 1.0f) / 1000.0f);
    if (s.voicedSampleCount >= lockGraceSamples && s.lockedMidi < 0) {
        s.lockedMidi = bestMidi; s.lockEngageSamples = 0;
        s.candidateMidi = -1; s.candidateStableSamples = 0;
    }
    if (s.lockedMidi >= 0) {
        s.lockEngageSamples += (int)blockSize;
        if (bestMidi != s.lockedMidi) {
            if (bestMidi == s.candidateMidi) s.candidateStableSamples += (int)blockSize;
            else { s.candidateMidi = bestMidi; s.candidateStableSamples = (int)blockSize; }
            if (s.candidateStableSamples >= reLockStableSamples) {
                s.lockedMidi = s.candidateMidi; s.lockEngageSamples = 0;
                s.candidateMidi = -1; s.candidateStableSamples = 0;
            }
        } else { s.candidateMidi = -1; s.candidateStableSamples = 0; }
    }

    float engageFade = std::max(0.0f, std::min(1.0f,
                          1000.0f * (float)s.lockEngageSamples / sr / std::max(attackMs, 1.0f)));
    float lockBypass = 1.0f - engageFade;

    float rawTargetMidi = bestMidi + clampedDev;
    rawTargetMidi = rawTargetMidi * (1.0f - lockBypass) + effectiveMidi * lockBypass;

    float blockDtMs = 1000.0f * blockSize / sr;
    float portamentoTimeMs = 15.0f;
    float targetAlpha = 1.0f - std::exp(-blockDtMs / portamentoTimeMs);
    if (s.smoothedTargetMidi < 0.0f) s.smoothedTargetMidi = rawTargetMidi;
    else s.smoothedTargetMidi += targetAlpha * (rawTargetMidi - s.smoothedTargetMidi);

    float targetHz = 440.0f * std::pow(2.0f, (s.smoothedTargetMidi - 69.0f) / 12.0f);
    return std::max(0.5f, std::min(2.0f, targetHz / detectedHz));
}

void renderEngine(const juce::AudioBuffer<float>& input, double sampleRate,
                  int tuningMode, const char* label, const char* wavPath)
{
    const int   block_size = 256;
    const float sr = (float)sampleRate;
    const int   numSamples = input.getNumSamples();
    const int   numBlocks  = numSamples / block_size;

    const float vibratoAmount = 0.5f;
    const float attackMs      = 50.0f;
    const float releaseMs     = 100.0f;

    PitchDetector detector;
    PitchShifter  shifter;
    detector.prepare(sampleRate, block_size);
    shifter.prepare(sampleRate, block_size);
    shifter.setTuningMode(tuningMode);
    shifter.setBreathGate(0.0f, false);
    shifter.setExciter(0.0f, false);

    bool activeKeys[88];
    for (int i = 0; i < 88; ++i) activeKeys[i] = true;

    TuneState tune;
    juce::AudioBuffer<float> out(1, numSamples);
    out.clear();

    for (int b = 0; b < numBlocks; ++b)
    {
        const float* blockData = input.getReadPointer(0, b * block_size);
        float sumSq = 0.0f;
        for (int i = 0; i < block_size; ++i) sumSq += blockData[i] * blockData[i];
        float rms = std::sqrt(sumSq / block_size);

        detector.process(blockData, block_size);
        bool  isConsonant = detector.isConsonant();
        float pitchHz     = detector.getInstantPitch();
        bool  isVoiced    = (pitchHz > 50.0f) && !isConsonant && rms > 0.02f;

        juce::AudioBuffer<float> blockBuf(2, block_size);
        for (int c = 0; c < 2; ++c) blockBuf.copyFrom(c, 0, blockData, block_size);

        float ratio = 1.0f;
        if (isVoiced)
            ratio = computeRatio(tune, pitchHz, activeKeys, (float)block_size, sr,
                                 attackMs, vibratoAmount);
        else
            tune.reset();

        shifter.setTargetShift(ratio, attackMs, isVoiced ? releaseMs : 10.0f,
                               isVoiced, isVoiced ? pitchHz : 0.0f, vibratoAmount);
        shifter.process(blockBuf);
        out.copyFrom(0, b * block_size, blockBuf.getReadPointer(0), block_size);
    }

    TestHelpers::writeWavFile(wavPath, out, sampleRate);
    std::cout << "  [" << label << "] rendered -> " << wavPath << std::endl;
}

} // namespace

void runLongMelodyTest(const juce::String& filename)
{
    double sampleRate = 0.0;
    juce::AudioBuffer<float> monoBuffer = TestHelpers::loadAndNormalizeWav(filename, sampleRate);

    std::cout << "File: " << juce::File(filename).getFileName().toStdString()
              << " (Long Melody — render both engines)" << std::endl;

    renderEngine(monoBuffer, sampleRate, 0, "MODERN",  "test/result/long_melody_modern.wav");
    renderEngine(monoBuffer, sampleRate, 1, "CLASSIC", "test/result/long_melody_classic.wav");

    std::cout << "  RESULT: PASS (rendered long_melody_modern.wav and long_melody_classic.wav)" << std::endl;
}
