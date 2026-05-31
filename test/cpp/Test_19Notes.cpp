#include "Test_19Notes.h"
#include "TestHelpers.h"
#include "../../Source/DSP/PitchDetector.h"
#include <iostream>
#include <cmath>
#include <vector>

// Test 10: Pitch-detector accuracy on long_melody.wav
//
// The melody contains 19 clearly voiced note segments. The windows and target
// notes mirror test/result/note_alignment.txt ("your note"). This test is
// primarily an octave-stability audit: a detector can be a little flat/sharp
// on a sung transition, but it must not emit an octave-family note inside a
// manually aligned note window.
//
// Before the 2nd-harmonic guard fix, many segments had large numbers of
// octave-up errors (detected note = expected+12), driving accuracy well below
// the threshold. After the fix the detector and note-mapping layers should be
// close to 19/19 clean segments.

void run19NotesTest(const juce::String& filename)
{
    std::cout << "\nTest 10: Pitch-detector 19-note accuracy on long_melody.wav" << std::endl;

    double sampleRate = 0.0;
    juce::AudioBuffer<float> monoBuffer = TestHelpers::loadAndNormalizeWav(filename, sampleRate);
    int numSamples = monoBuffer.getNumSamples();

    const int block_size = 256;
    PitchDetector detector;
    detector.prepare(sampleRate, block_size);

    int numBlocks = numSamples / block_size;

    // Ground-truth note segments from test/result/note_alignment.txt.
    // Each entry: { startSeconds, endSeconds, expectedMidi, name }.
    struct NoteSegment {
        float startSeconds, endSeconds;
        int expectedMidi;
        const char* name;
    };
    static const NoteSegment segments[] = {
        { 0.15f,  1.30f, 59, "B3  (seg 1)"  },
        { 1.34f,  1.46f, 61, "C#4 (seg 2)"  },
        { 1.47f,  1.79f, 63, "D#4 (seg 3)"  },
        { 1.86f,  2.35f, 61, "C#4 (seg 4)"  },
        { 2.58f,  2.76f, 61, "C#4 (seg 5)"  },
        { 2.84f,  3.24f, 59, "B3  (seg 6)"  },
        { 3.24f,  3.62f, 58, "A#3 (seg 7)"  },
        { 3.80f,  4.40f, 59, "B3  (seg 8)"  },
        { 4.92f,  5.15f, 61, "C#4 (seg 9)"  },
        { 5.27f,  5.52f, 63, "D#4 (seg10)"  },
        { 5.72f,  6.05f, 61, "C#4 (seg11)"  },
        { 6.24f,  6.43f, 63, "D#4 (seg12)"  },
        { 6.93f,  7.15f, 59, "B3  (seg13)"  },
        { 7.15f,  7.33f, 58, "A#3 (seg14)"  },
        { 7.40f,  7.62f, 59, "B3  (seg15)"  },
        { 8.76f,  9.00f, 61, "C#4 (seg16)"  },
        { 9.14f,  9.26f, 63, "D#4 (seg17)"  },
        { 9.50f,  9.93f, 61, "C#4 (seg18)"  },
        {10.00f, 10.50f, 61, "C#4 (seg19)"  },
    };
    static const int kNumSegments = (int)(sizeof(segments) / sizeof(segments[0]));
    static const int kSemitoneWindow = 3; // allow sung approach notes / pitch drift
    static const int kOctaveErrorWindow = 8;

    // Run detector over all blocks
    std::vector<float> detectedHz(numBlocks, 0.0f);
    for (int b = 0; b < numBlocks; ++b) {
        const float* blockData = monoBuffer.getReadPointer(0, b * block_size);
        detector.process(blockData, block_size);
        detectedHz[b] = detector.getInstantPitch();
    }

    // Evaluate per segment
    int totalCorrect = 0, totalSegBlocks = 0, totalOctaveErrors = 0;
    int failedSegments = 0;

    for (int s = 0; s < kNumSegments; ++s) {
        const auto& seg = segments[s];
        int lo = juce::jlimit(0, numBlocks - 1,
                              (int)std::floor(seg.startSeconds * (float)sampleRate / (float)block_size));
        int hi = juce::jlimit(0, numBlocks - 1,
                              (int)std::ceil (seg.endSeconds   * (float)sampleRate / (float)block_size));
        int correct = 0, total = 0, octaveErrors = 0;
        float worstAbsError = 0.0f;

        for (int b = lo; b <= hi; ++b) {
            float hz = detectedHz[b];
            if (hz <= 0.0f) continue;
            float midiF = 69.0f + 12.0f * std::log2(hz / 440.0f);
            int midi = (int)std::round(midiF);
            int absError = std::abs(midi - seg.expectedMidi);
            ++total;
            if (absError <= kSemitoneWindow) ++correct;
            if (absError >= kOctaveErrorWindow) ++octaveErrors;
            worstAbsError = juce::jmax(worstAbsError, std::abs(midiF - (float)seg.expectedMidi));
        }

        float acc = (total > 0) ? (float)correct / (float)total : 0.0f;
        bool ok = (total > 0 && octaveErrors == 0);
        totalCorrect += correct;
        totalSegBlocks += total;
        totalOctaveErrors += octaveErrors;

        std::cout << "  " << seg.name
                  << " [" << seg.startSeconds << "-" << seg.endSeconds << "s]"
                  << ": near-note " << correct << "/" << total
                  << " (" << (int)(acc * 100.0f) << "%), octave-errors "
                  << octaveErrors << ", worst "
                  << (int)std::round(worstAbsError * 10.0f) / 10.0f << " st"
                  << (ok ? " OK" : " FAIL") << std::endl;

        if (!ok) ++failedSegments;
    }

    float overallAcc = (totalSegBlocks > 0)
                     ? (float)totalCorrect / (float)totalSegBlocks : 0.0f;
    std::cout << "  Overall: " << totalCorrect << "/" << totalSegBlocks
              << " (" << (int)(overallAcc * 100.0f) << "%)" << std::endl;

    if (failedSegments > 0) {
        std::cerr << "  RESULT: FAIL (" << failedSegments
                  << " segments have octave-family errors, total octave errors "
                  << totalOctaveErrors << ")" << std::endl;
        std::exit(1);
    }
    std::cout << "  RESULT: PASS (detector has no octave-family errors in aligned note windows)" << std::endl;
}
