#pragma once
#include <JuceHeader.h>
#include "../../Source/DSP/PitchShifter.h"
#include "../../Source/DSP/PitchDetector.h"
#include "../../Source/DSP/NoteSelector.h"
#include <cmath>

// Faithful, header-only mirror of the plugin's tuning signal path —
// PitchDetector -> note selection / lock -> tuning ratio -> real PitchShifter.
// Both the Long Melody render and the Classic click test use this so the test
// artifacts (test/result/*.wav) behave exactly like the plugin does in a DAW.
// The tuning math mirrors CanaryVoiceTuneAudioProcessor::chooseTargetNoteAndRatio
// (note matching is maximal — no Range control; all 88 keys enabled).
namespace TuneRender {

// Per-voiced-segment tuning state (mirrors PluginProcessor members).
struct TuneState {
    float smoothedMidi = -1.0f;
    float smoothedTargetMidi = -1.0f;
    int   releaseMidi = -1;
    int   attackSamples = 0;
    int   fadeSamples = 0;       // phrase-level counter; NOT reset on consonants
    int   noteHeldSamples = 0;
    int   candidateMidi = -1;
    int   candidateStableSamples = 0;
    long  voicedSampleCount = 0;
    void reset() { *this = TuneState{}; }
    void resetNoteLock() {
        // Mirrors PluginProcessor::resetNoteLockState — resets smoothedMidi so
        // the selector re-seeds from the new note's pitch, not the previous one.
        // fadeSamples intentionally NOT reset here (phrase-level counter).
        releaseMidi = -1; attackSamples = 0; noteHeldSamples = 0;
        smoothedMidi = -1.0f; smoothedTargetMidi = -1.0f; voicedSampleCount = 0;
        candidateMidi = -1; candidateStableSamples = 0;
    }
};

// Mirror of chooseTargetNoteAndRatio: returns ratio.
inline float computeRatio(TuneState& s, float detectedHz, const bool* activeKeys,
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
        s.fadeSamples     += (int)blockSize;
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
                          1000.0f * (float)s.fadeSamples / sr / std::max(attackMs, 1.0f)));
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

// Core render implementation — shared by both render() overloads.
inline juce::AudioBuffer<float> renderWithKeys(const juce::AudioBuffer<float>& input,
                                               double sampleRate, int tuningMode,
                                               float attackMs, float releaseMs,
                                               float vibratoAmount,
                                               const bool activeKeys[88])
{
    const int   block_size = 256;
    const float sr = (float)sampleRate;
    const int   numSamples = input.getNumSamples();
    const int   numBlocks  = numSamples / block_size;

    PitchDetector detector;
    PitchShifter  shifter;
    detector.prepare(sampleRate, block_size);
    shifter.prepare(sampleRate, block_size);
    shifter.setTuningMode(tuningMode);
    shifter.setBreathGate(0.0f, false);
    shifter.setExciter(0.0f, false);

    // Pre-feed silence so engine state-machines (especially RubberBand's
    // analysis window) reach steady state before any vocal audio arrives,
    // mirroring real-time plugin use.
    {
        juce::AudioBuffer<float> silenceBuf(2, block_size);
        silenceBuf.clear();
        int warmupBlocks = (int)(sampleRate * 0.5) / block_size; // 500 ms
        for (int wb = 0; wb < warmupBlocks; ++wb) {
            shifter.setTargetShift(1.0f, attackMs, 10.0f, false, 0.0f, 0.0f);
            shifter.process(silenceBuf);
        }
    }

    TuneState tune;
    juce::AudioBuffer<float> out(1, numSamples);
    out.clear();

    for (int b = 0; b < numBlocks; ++b)
    {
        const float* blockData = input.getReadPointer(0, b * block_size);
        float sumSq = 0.0f;
        for (int i = 0; i < block_size; ++i) sumSq += blockData[i] * blockData[i];
        float rms = std::sqrt(sumSq / block_size);

        float smoothedHz  = detector.process(blockData, block_size);
        bool  isConsonant = detector.isConsonant();
        float pitchHz     = detector.getInstantPitch();
        if (pitchHz <= 0.0f) pitchHz = smoothedHz;

        // Octave-jump guard: mirrors PluginProcessor.
        if (pitchHz > 0.0f && smoothedHz > 0.0f) {
            float st = 12.0f * std::abs(std::log2(pitchHz / smoothedHz));
            if (st >= 11.0f) pitchHz = smoothedHz;
        }

        bool isVoiced = (pitchHz > 50.0f) && !isConsonant && rms > 0.01f;

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

    return out;
}

// Renders with all 88 keys active (plugin default / long-melody use case).
inline juce::AudioBuffer<float> render(const juce::AudioBuffer<float>& input,
                                       double sampleRate, int tuningMode,
                                       float attackMs, float releaseMs,
                                       float vibratoAmount = 0.0f)
{
    bool keys[88];
    for (int i = 0; i < 88; ++i) keys[i] = true;
    return renderWithKeys(input, sampleRate, tuningMode, attackMs, releaseMs, vibratoAmount, keys);
}

// Renders with a specific set of MIDI notes active (piano-roll use case).
// midiNotes is a list of MIDI note numbers (21–108) that are pressed.
inline juce::AudioBuffer<float> renderWithNotes(const juce::AudioBuffer<float>& input,
                                                double sampleRate, int tuningMode,
                                                float attackMs, float releaseMs,
                                                std::initializer_list<int> midiNotes,
                                                float vibratoAmount = 0.0f)
{
    bool keys[88] = {};
    for (int m : midiNotes) {
        int idx = m - 21;
        if (idx >= 0 && idx < 88) keys[idx] = true;
    }
    return renderWithKeys(input, sampleRate, tuningMode, attackMs, releaseMs, vibratoAmount, keys);
}

} // namespace TuneRender
