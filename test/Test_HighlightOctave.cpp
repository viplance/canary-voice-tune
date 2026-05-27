#include "Test_HighlightOctave.h"
#include "TestHelpers.h"
#include "../Source/DSP/PitchDetector.h"
#include <iostream>
#include <cmath>

// Mirror of CanaryVoiceTuneAudioProcessor::chooseTargetNoteAndRatio, reduced to
// the part that produces the displayed (highlighted) note. State that the
// plugin keeps as members is held in this struct so a block-by-block run
// reproduces the exact smoothing/jump-bypass behaviour. Kept deliberately
// close to PluginProcessor.cpp so the two can be diffed by eye.
struct HighlightChain {
    float smoothedMidi = -1.0f;

    // Returns the displayed MIDI note (chromatic = every key active, which is
    // the worst case the user reported as octave-independent), or -1.
    int displayedMidi(float detectedHz, float blockSize, float sr,
                      float vibratoAmount) {
        if (detectedHz <= 0.0f) return -1;

        float floatMidi = 69.0f + 12.0f * std::log2(detectedHz / 440.0f);

        float blockDt = blockSize / sr;
        float vibTimeMs = 201.0f;
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

        // Chromatic: nearest semitone to the smoothed pitch. With every key
        // active this is exactly the plugin's bestMidi selection.
        float lockPitch = smoothedMidi;
        int bestMidi = (int)std::round(lockPitch);
        return bestMidi;
    }
};

void runHighlightOctaveTest(const juce::String& filename)
{
    double sampleRate = 0.0;
    juce::AudioBuffer<float> monoBuffer = TestHelpers::loadAndNormalizeWav(filename, sampleRate);
    int numSamples = monoBuffer.getNumSamples();

    PitchDetector detector;
    // Host-realistic block size — the plugin runs at the host's buffer size
    // (commonly 512), not the 256 used elsewhere in the test suite.
    int block_size = 512;
    detector.prepare(sampleRate, block_size);

    HighlightChain chain;
    int numBlocks = numSamples / block_size;

    // Track the displayed note over time and flag octave jumps: a single-note
    // melody should never have the highlight land ~12 semitones off the local
    // trend. We compare each displayed note against a slow reference of the
    // recently displayed notes (ignoring the first few stabilisation blocks).
    int   octaveJumpCount = 0;
    float refMidi = -1.0f;  // slow EMA of displayed note
    int   displayedCount = 0;

    std::cout << "File: " << juce::File(filename).getFileName().toStdString()
              << " (Keyboard Highlight Octave-Stability Test, block=" << block_size << ")" << std::endl;

    for (int b = 0; b < numBlocks; ++b) {
        const float* blockData = monoBuffer.getReadPointer(0, b * block_size);

        float sumSq = 0.0f;
        for (int i = 0; i < block_size; ++i) sumSq += blockData[i] * blockData[i];
        float rms = std::sqrt(sumSq / block_size);

        // Reproduce the plugin's pitch selection exactly (PluginProcessor.cpp:
        // 490-492): process() returns the smoothed pitch, getInstantPitch() the
        // per-block estimate; the tuner/highlight uses instant if available,
        // else the smoothed (held) pitch.
        float smoothedHz = detector.process(blockData, block_size);
        float instantHz  = detector.getInstantPitch();
        float detectedHz = (instantHz > 0.0f) ? instantHz : smoothedHz;
        bool  isConsonant = detector.isConsonant();
        bool  isVoiced = (detectedHz > 0.0f) && !isConsonant;

        int displayed = isVoiced ? chain.displayedMidi(detectedHz, (float)block_size,
                                                        (float)sampleRate, 0.0f)
                                  : -1;

        if (displayed >= 0 && rms > 0.03f) {
            displayedCount++;
            if (refMidi < 0.0f) {
                refMidi = (float)displayed;
            } else {
                // An octave jump = displayed note ~12 semitones from the slow
                // reference. Anything beyond 7 semitones (a fifth) on a held
                // single note is already a tracking defect.
                float dist = std::abs((float)displayed - refMidi);
                if (dist >= 7.0f) {
                    std::cout << "  ERROR: Block " << b << " highlighted MIDI " << displayed
                              << " is " << dist << " semitones off the local trend (refMidi="
                              << refMidi << ") — octave jump" << std::endl;
                    octaveJumpCount++;
                }
                // Slow EMA so the reference follows real melodic moves but not
                // single-block octave spikes.
                refMidi += 0.15f * ((float)displayed - refMidi);
            }
        }
    }

    std::cout << "  Displayed (voiced) blocks: " << displayedCount << std::endl;
    std::cout << "  Highlight octave-jump instances: " << octaveJumpCount << std::endl;

    if (octaveJumpCount == 0) {
        std::cout << "  RESULT: PASS (Keyboard highlight stayed octave-stable)" << std::endl;
    } else {
        std::cerr << "  RESULT: FAIL (Keyboard highlight jumped octaves "
                  << octaveJumpCount << " times!)" << std::endl;
        std::exit(1);
    }
}
