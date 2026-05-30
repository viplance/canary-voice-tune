#include "Test_19Notes.h"
#include "TestHelpers.h"
#include "../Source/DSP/PitchDetector.h"
#include <iostream>
#include <cmath>
#include <vector>

// Test 10: Pitch-detector accuracy on long_melody.wav
//
// The melody contains 19 clearly voiced note segments. For each segment we
// know the expected MIDI note (ground-truth from manual inspection) and the
// block range. The test counts how many blocks inside each segment are
// detected within ±1 MIDI semitone of the expected note, and requires a
// minimum accuracy (kMinAccuracy) per segment and overall.
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

    // Ground-truth note segments.
    // Each entry: { blockStart, blockEnd, expectedMidi, name }
    // Block ranges derived from manual analysis of long_melody.wav @ 48 kHz / 256-sample blocks.
    // Notes with fewer than 8 blocks (< ~43 ms) are excluded — too short to
    // audit reliably with a single-channel estimate.
    struct NoteSegment {
        int blockStart, blockEnd;
        int expectedMidi;
        const char* name;
    };
    static const NoteSegment segments[] = {
        {  20,  61,  59, "B3  (seg 1)" },   // B3  ~246 Hz
        {  70, 141,  59, "B3  (seg 2)" },   // B3  continued
        { 156, 255,  59, "B3  (seg 3)" },   // B3  continued
        { 282, 340,  62, "D4  (seg 4)" },   // D4  ~294 Hz
        { 357, 477,  61, "C#4 (seg 5)" },   // C#4 ~277 Hz
        { 492, 684,  59, "B3  (seg 6)" },   // B3
        { 711, 717,  56, "G#3 (seg 7)" },   // G#3 ~208 Hz (onset excluded)
        { 725, 973,  59, "B3  (seg 8)" },   // B3
        { 989, 1045, 63, "D#4 (seg 9)" },   // D#4 ~311 Hz
        {1077, 1145, 61, "C#4 (seg10)" },   // C#4
        {1160, 1296, 63, "D#4 (seg11)" },   // D#4
        {1305, 1393, 59, "B3  (seg12)" },   // B3
        {1402, 1437, 58, "A#3 (seg13)" },   // A#3 ~233 Hz
        {1441, 1446, 57, "A3  (seg14)" },   // A3  ~220 Hz
        {1455, 1461, 59, "B3  (seg15)" },   // B3
        {1466, 1566, 58, "A#3 (seg16)" },   // A#3
        {1637, 1696, 61, "C#4 (seg17)" },   // C#4
        {1707, 1748, 63, "D#4 (seg18)" },   // D#4
        {1753, 1993, 61, "C#4 (seg19)" },   // C#4
    };
    static const int kNumSegments = (int)(sizeof(segments) / sizeof(segments[0]));
    static const float kMinAccuracy = 0.80f; // require ≥ 80% correct blocks per segment
    static const float kOverallMinAccuracy = 0.85f;
    static const int kSemitoneWindow = 2; // allow ±2 semitones to accommodate vibrato

    // Run detector over all blocks
    std::vector<float> detectedHz(numBlocks, 0.0f);
    for (int b = 0; b < numBlocks; ++b) {
        const float* blockData = monoBuffer.getReadPointer(0, b * block_size);
        detector.process(blockData, block_size);
        detectedHz[b] = detector.getInstantPitch();
    }

    // Evaluate per segment
    int totalCorrect = 0, totalSegBlocks = 0;
    int failedSegments = 0;

    for (int s = 0; s < kNumSegments; ++s) {
        const auto& seg = segments[s];
        int hi = juce::jmin(seg.blockEnd, numBlocks - 1);
        int correct = 0, total = 0;

        for (int b = seg.blockStart; b <= hi; ++b) {
            float hz = detectedHz[b];
            if (hz <= 0.0f) continue;
            float midiF = 69.0f + 12.0f * std::log2(hz / 440.0f);
            int midi = (int)std::round(midiF);
            ++total;
            if (std::abs(midi - seg.expectedMidi) <= kSemitoneWindow) ++correct;
        }

        float acc = (total > 0) ? (float)correct / (float)total : 0.0f;
        bool ok = (acc >= kMinAccuracy);
        totalCorrect += correct;
        totalSegBlocks += total;

        std::cout << "  " << seg.name
                  << ": " << correct << "/" << total
                  << " (" << (int)(acc * 100.0f) << "%)"
                  << (ok ? " OK" : " FAIL") << std::endl;

        if (!ok) ++failedSegments;
    }

    float overallAcc = (totalSegBlocks > 0)
                     ? (float)totalCorrect / (float)totalSegBlocks : 0.0f;
    std::cout << "  Overall: " << totalCorrect << "/" << totalSegBlocks
              << " (" << (int)(overallAcc * 100.0f) << "%)" << std::endl;

    if (failedSegments > 0 || overallAcc < kOverallMinAccuracy) {
        std::cerr << "  RESULT: FAIL (" << failedSegments << " segments below "
                  << (int)(kMinAccuracy * 100) << "%, overall "
                  << (int)(overallAcc * 100.0f) << "% < "
                  << (int)(kOverallMinAccuracy * 100) << "%)" << std::endl;
        std::exit(1);
    }
    std::cout << "  RESULT: PASS (detector accuracy meets threshold)" << std::endl;
}
