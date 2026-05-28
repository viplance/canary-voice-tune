#include "Test_LongMelody.h"
#include "TestHelpers.h"
#include "../Source/DSP/PitchDetector.h"
#include "../Source/DSP/NoteSelector.h"
#include <iostream>
#include <cmath>
#include <vector>

// We analyse the FINAL note the user actually sees highlighted on the keyboard
// — the `displayedMidi` the processor pushes per block — not the raw per-block
// pitch. In CanaryVoiceTuneAudioProcessor::chooseTargetNoteAndRatio that
// displayed note is `bestMidi`: the NoteSelector nearest-active-key choice
// (with the 0.1-semitone incumbent hysteresis), evaluated every block BEFORE
// the slower lock/re-lock state machine. The keyboard highlight therefore
// flickers up with every octave misdetection that the NoteSelector resolves to
// a key, which is what the user counts (about ten extra highlights on
// long_melody.wav). Counting raw flagged blocks would massively over-count
// (one jump spans many blocks).
//
// The selection + lock logic below mirrors the processor: same vibrato
// smoothing, same NoteSelector::chooseActiveNote call, same fixed 0.1-semitone
// hysteresis, same lock/re-lock state machine (it feeds `lockedMidi` back in as
// the hysteresis incumbent) and timing constants. All 88 keys are enabled (the
// plugin default), so the displayed note follows the chromatic nearest-key
// choice. Jumps are counted on the displayed `bestMidi`, exactly as the
// keyboard is driven.
void runLongMelodyTest(const juce::String& filename)
{
    double sampleRate = 0.0;
    juce::AudioBuffer<float> monoBuffer = TestHelpers::loadAndNormalizeWav(filename, sampleRate);
    int numSamples = monoBuffer.getNumSamples();

    PitchDetector detector;
    const int block_size = 256;
    detector.prepare(sampleRate, block_size);

    int numBlocks = numSamples / block_size;

    // --- All keys enabled, matching the plugin default. --------------------
    bool activeKeys[88];
    for (int i = 0; i < 88; ++i) activeKeys[i] = true;

    // --- Processor-mirrored constants. -------------------------------------
    const float vibratoAmount = 0.5f;      // default vibrato tolerance (semitones)
    const float attackMs      = 50.0f;     // default snap responsiveness
    const float hysteresisSt  = 0.1f;      // fixed NoteSelector stick-band
    const float sr            = (float)sampleRate;

    // --- Processor-mirrored running state. ---------------------------------
    float smoothedMidi          = -1.0f;
    int   lockedMidi            = -1;
    int   candidateMidi         = -1;
    int   lockEngageSamples     = 0;
    int   candidateStableSamples = 0;
    long  voicedSampleCount     = 0;

    const int lockGraceSamples    = (int)(sr * 0.050f);
    const int reLockStableSamples = (int)(sr * std::max(attackMs, 1.0f) / 1000.0f);

    // The displayed-note history (the keyboard highlight = `bestMidi`). We
    // detect octave-UP jumps in this sequence relative to a slowly-adapting
    // stable reference note (the surrounding sustained note).
    int   prevDisplayed = -1;
    float stableNote    = -1.0f; // exponential average of the displayed MIDI note

    int octaveUpJumps = 0;
    int firstJumpBlock = -1;

    for (int b = 0; b < numBlocks; ++b)
    {
        const float* blockData = monoBuffer.getReadPointer(0, b * block_size);

        float sumSq = 0.0f;
        for (int i = 0; i < block_size; ++i) sumSq += blockData[i] * blockData[i];
        float rms = std::sqrt(sumSq / block_size);

        detector.process(blockData, block_size);
        bool isConsonant = detector.isConsonant();
        float pitchHz = detector.getInstantPitch();
        bool isVoiced = (pitchHz > 50.0f) && !isConsonant && rms > 0.02f;

        if (!isVoiced)
            continue;

        voicedSampleCount += block_size;

        // ---- Vibrato smoothing (semitone domain), as in the processor. ----
        float floatMidi = 69.0f + 12.0f * std::log2(pitchHz / 440.0f);
        float blockDt   = (float)block_size / sr;
        const float vibTimeMs = 201.0f;
        if (smoothedMidi < 0.0f) {
            smoothedMidi = floatMidi;
        } else {
            float delta = std::abs(floatMidi - smoothedMidi);
            float jumpLo = vibratoAmount + 0.5f;
            float jumpHi = vibratoAmount + 1.2f;
            float jumpFactor;
            if (delta < jumpLo)      jumpFactor = 0.0f;
            else if (delta > jumpHi) jumpFactor = 1.0f;
            else                     jumpFactor = (delta - jumpLo) / (jumpHi - jumpLo);
            float effectiveTimeMs = vibTimeMs * (1.0f - jumpFactor) + 1.0f * jumpFactor;
            float adaptiveAlpha = 1.0f - std::exp(-blockDt / (effectiveTimeMs / 1000.0f));
            smoothedMidi += adaptiveAlpha * (floatMidi - smoothedMidi);
        }

        // ---- Closest active key + hysteresis. -----------------------------
        float lockPitch = (smoothedMidi > 0.0f) ? smoothedMidi : floatMidi;
        int bestMidi = NoteSelector::chooseActiveNote(activeKeys, lockPitch,
                                                      lockedMidi, hysteresisSt);
        if (bestMidi < 0) bestMidi = (int)std::round(lockPitch);

        // ---- Note-lock state machine. -------------------------------------
        if (voicedSampleCount >= lockGraceSamples && lockedMidi < 0) {
            lockedMidi = bestMidi;
            lockEngageSamples = 0;
            candidateMidi = -1;
            candidateStableSamples = 0;
        }
        if (lockedMidi >= 0) {
            lockEngageSamples += block_size;
            if (bestMidi != lockedMidi) {
                if (bestMidi == candidateMidi) {
                    candidateStableSamples += block_size;
                } else {
                    candidateMidi = bestMidi;
                    candidateStableSamples = block_size;
                }
                if (candidateStableSamples >= reLockStableSamples) {
                    lockedMidi = candidateMidi;
                    lockEngageSamples = 0;
                    candidateMidi = -1;
                    candidateStableSamples = 0;
                }
            } else {
                candidateMidi = -1;
                candidateStableSamples = 0;
            }
        }

        // ---- Detect octave-UP jumps in the displayed (keyboard) note. -----
        // The keyboard highlight follows `bestMidi`. A real stepwise melody
        // moves it a few semitones at a time; a spurious octave error makes it
        // leap ~12 semitones above the surrounding sustained note and then fall
        // back. We flag a jump when the displayed note rises >= 9 semitones
        // above the stable reference (an octave is 12; 9 cleanly separates
        // octave errors from any musical step in this melody) on a fresh
        // transition into that note.
        const float kJumpSemitones = 9.0f;
        if (stableNote < 0.0f) {
            stableNote = (float)bestMidi;
        } else if (bestMidi != prevDisplayed
                   && (float)bestMidi - stableNote >= kJumpSemitones) {
            float t = (float)(b * block_size) / sr;
            std::cout << "  ERROR: Octave jump #" << (octaveUpJumps + 1)
                      << " at block " << b << " (t=" << t << "s): displayed note jumped to MIDI "
                      << bestMidi << " (~" << ((float)bestMidi - stableNote)
                      << " semitones above the held note " << stableNote << ")" << std::endl;
            octaveUpJumps++;
            if (firstJumpBlock < 0) firstJumpBlock = b;
            // Do NOT let the spike pull the reference up.
        } else {
            stableNote = 0.9f * stableNote + 0.1f * (float)bestMidi;
        }
        prevDisplayed = bestMidi;
    }

    std::cout << "File: " << juce::File(filename).getFileName().toStdString()
              << " (Long Melody Octave-Jump Detection Test)" << std::endl;
    std::cout << "  Spurious octave-up jump events (displayed/keyboard note): "
              << octaveUpJumps << std::endl;
    if (firstJumpBlock >= 0)
    {
        float t = (float)(firstJumpBlock * block_size) / sr;
        std::cout << "  First jump at block " << firstJumpBlock
                  << " (t=" << t << "s)" << std::endl;
    }

    if (octaveUpJumps == 0)
    {
        std::cout << "  RESULT: PASS (No spurious octave-up jumps in the displayed note)" << std::endl;
    }
    else
    {
        std::cerr << "  RESULT: FAIL (PitchDetector produced " << octaveUpJumps
                  << " spurious octave-up jumps in the displayed note on long_melody.wav)" << std::endl;
        std::exit(1);
    }
}
