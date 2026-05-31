#include "Test_LongMelody.h"
#include "TestHelpers.h"
#include "../../Source/DSP/PitchShifter.h"
#include "../../Source/DSP/PitchDetector.h"
#include "../../Source/DSP/NoteSelector.h"
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

// Per-voiced-segment tuning state (mirrors PluginProcessor members).
struct TuneState {
    float smoothedMidi = -1.0f;
    float smoothedTargetMidi = -1.0f;
    int   releaseMidi = -1;
    int   attackSamples = 0;
    int   noteHeldSamples = 0;
    int   candidateMidi = -1;
    int   candidateStableSamples = 0;
    long  voicedSampleCount = 0;
    void reset() { *this = TuneState{}; }
    void resetNoteLock() {
        // Mirrors PluginProcessor::resetNoteLockState — resets smoothedMidi so
        // the selector re-seeds from the new note's pitch, not the previous one.
        releaseMidi = -1; attackSamples = 0; noteHeldSamples = 0;
        smoothedMidi = -1.0f; smoothedTargetMidi = -1.0f; voicedSampleCount = 0;
        candidateMidi = -1; candidateStableSamples = 0;
    }
};

// Mirror of chooseTargetNoteAndRatio: returns ratio.
float computeRatio(TuneState& s, float detectedHz, const bool* activeKeys,
                   float blockSize, float sr, float attackMs, float releaseMs,
                   float vibratoAmount)
{
    juce::ignoreUnused(releaseMs);
    s.voicedSampleCount += (long)blockSize;
    float floatMidi = 69.0f + 12.0f * std::log2(detectedHz / 440.0f);

    float blockDt = blockSize / sr;
    const float vibTimeMs = 120.0f;
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
    int bestMidi = NoteSelector::chooseActiveNote(activeKeys, lockPitch, s.releaseMidi, hysteresisSt);
    if (bestMidi < 0) bestMidi = (int)std::round(lockPitch);

    const int graceSamples        = (int)(sr * 0.050f);
    const int attackStableSamples = (int)(sr * std::max(attackMs, 1.0f) / 1000.0f);
    // Decoupled responsive note-switching refractory period (fixed 30 ms) to allow
    // rapid and precise melodic transitions, independent of vocal effects releaseMs.
    const int releaseHoldSamples  = (int)(sr * 0.030f);

    if (s.voicedSampleCount >= graceSamples && s.releaseMidi < 0) {
        s.releaseMidi = bestMidi; s.attackSamples = 0; s.noteHeldSamples = 0;
        s.candidateMidi = -1; s.candidateStableSamples = 0;
    }
    if (s.releaseMidi >= 0) {
        s.attackSamples   += (int)blockSize;
        s.noteHeldSamples += (int)blockSize;
        if (bestMidi != s.releaseMidi) {
            if (s.noteHeldSamples >= releaseHoldSamples) {
                if (bestMidi == s.candidateMidi) s.candidateStableSamples += (int)blockSize;
                else { s.candidateMidi = bestMidi; s.candidateStableSamples = (int)blockSize; }
                if (s.candidateStableSamples >= attackStableSamples) {
                    s.releaseMidi = s.candidateMidi; s.attackSamples = 0; s.noteHeldSamples = 0;
                    s.candidateMidi = -1; s.candidateStableSamples = 0;
                }
            }
        } else { s.candidateMidi = -1; s.candidateStableSamples = 0; }
    }

    float engageFade = std::max(0.0f, std::min(1.0f,
                          1000.0f * (float)s.attackSamples / sr / std::max(attackMs, 1.0f)));
    float lockBypass = 1.0f - engageFade;

    int selectedMidi = (s.releaseMidi >= 0) ? s.releaseMidi : bestMidi;
    float rawTargetMidi = selectedMidi + clampedDev;
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

    const float vibratoAmount = 0.0f;
    const float attackMs      = 0.1f;
    const float releaseMs     = 10.0f;

    PitchDetector detector;
    PitchShifter  shifter;
    detector.prepare(sampleRate, block_size);
    shifter.prepare(sampleRate, block_size);
    shifter.setTuningMode(tuningMode);
    shifter.setBreathGate(0.0f, false);
    shifter.setExciter(0.0f, false);

    // Pre-feed silence through the shifter so state-machines (especially
    // RubberBand's internal analysis window) reach steady state before any
    // vocal audio arrives. This mirrors real-time plugin use where the engine
    // has been running since DAW playback started.
    {
        juce::AudioBuffer<float> silenceBuf(2, block_size);
        silenceBuf.clear();
        int warmupBlocks = (int)(sampleRate * 0.5) / block_size; // 500 ms
        for (int wb = 0; wb < warmupBlocks; ++wb) {
            shifter.setTargetShift(1.0f, attackMs, 10.0f, false, 0.0f, 0.0f);
            shifter.process(silenceBuf);
        }
    }

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

        // smoothedHz = lastValidPitch (held across short silences via hold frames)
        float smoothedHz  = detector.process(blockData, block_size);
        bool  isConsonant = detector.isConsonant();
        float pitchHz     = detector.getInstantPitch();
        if (pitchHz <= 0.0f) pitchHz = smoothedHz;

        // Octave-jump guard: mirrors PluginProcessor.
        // smoothedHz (lastValidPitch) survives short silences via hold frames,
        // so we compare against it rather than a manually-reset variable.
        if (pitchHz > 0.0f && smoothedHz > 0.0f) {
            float st = 12.0f * std::abs(std::log2(pitchHz / smoothedHz));
            if (st >= 11.0f) pitchHz = smoothedHz;
        }

        bool  isVoiced = (pitchHz > 50.0f) && !isConsonant && rms > 0.01f;

        juce::AudioBuffer<float> blockBuf(2, block_size);
        for (int c = 0; c < 2; ++c) blockBuf.copyFrom(c, 0, blockData, block_size);

        float ratio = 1.0f;
        if (isVoiced)
            ratio = computeRatio(tune, pitchHz, activeKeys, (float)block_size, sr,
                                 attackMs, releaseMs, vibratoAmount);
        else
            tune.resetNoteLock();

        shifter.setTargetShift(ratio, attackMs, isVoiced ? releaseMs : 10.0f,
                               isVoiced, isVoiced ? pitchHz : 0.0f, vibratoAmount);
        shifter.process(blockBuf);
        out.copyFrom(0, b * block_size, blockBuf.getReadPointer(0), block_size);
    }

    TestHelpers::writeWavFile(wavPath, out, sampleRate);
    int latSamples = shifter.getLatencySamples();
    // Write latency metadata so the Python verifier can compensate.
    {
        std::string metaPath = std::string(wavPath) + ".lat";
        if (FILE* f = std::fopen(metaPath.c_str(), "w")) {
            std::fprintf(f, "%d\n", latSamples);
            std::fclose(f);
        }
    }
    std::cout << "  [" << label << "] rendered -> " << wavPath
              << "  latency=" << latSamples
              << " samples (" << (1000.0 * latSamples / sampleRate) << " ms)"
              << std::endl;
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
}
