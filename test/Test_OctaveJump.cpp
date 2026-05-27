#include "Test_OctaveJump.h"
#include "TestHelpers.h"
#include "../Source/DSP/PitchDetector.h"
#include "../Source/DSP/PitchShifter.h"
#include <iostream>
#include <cmath>

void runOctaveJumpTest(const juce::String& filename)
{
    double sampleRate = 0.0;
    juce::AudioBuffer<float> monoBuffer = TestHelpers::loadAndNormalizeWav(filename, sampleRate);
    int numSamples = monoBuffer.getNumSamples();

    PitchDetector detector;
    PitchShifter shifter;
    
    int block_size = 256;
    detector.prepare(sampleRate, block_size);
    shifter.prepare(sampleRate, block_size);
    
    shifter.setBreathGate(0.0f, false);
    shifter.setExciter(0.0f, false);

    int numBlocks = numSamples / block_size;
    juce::AudioBuffer<float> outputBuffer(1, numSamples);
    outputBuffer.clear();

    int octaveDropCount = 0;
    
    for (int b = 0; b < numBlocks; ++b) {
        juce::AudioBuffer<float> blockBuf(2, block_size);
        blockBuf.clear();
        for (int c = 0; c < 2; ++c) {
            blockBuf.copyFrom(c, 0, monoBuffer.getReadPointer(0, b * block_size), block_size);
        }

        // Run pitch detection
        detector.process(blockBuf.getReadPointer(0), block_size);
        bool isConsonant = detector.isConsonant();
        float pitchHz = detector.getInstantPitch();
        bool isVoiced = (pitchHz > 0.0f) && !isConsonant;

        float sumSq = 0.0f;
        const float* blockData = monoBuffer.getReadPointer(0, b * block_size);
        for (int i = 0; i < block_size; ++i) sumSq += blockData[i] * blockData[i];
        float rms = std::sqrt(sumSq / block_size);

        // Robust block-by-block pitch range verification
        if (isVoiced && rms > 0.03f) {
            // Note 1: D3 (expected ~140-165 Hz, no drop to D2)
            if (b >= 26 && b <= 33) {
                if (pitchHz < 130.0f || pitchHz > 175.0f) {
                    std::cout << "  ERROR: Block " << b << " (D3 Note) tracked incorrectly as " << pitchHz << " Hz (Expected 130-175 Hz)" << std::endl;
                    octaveDropCount++;
                }
            }
            // Note 2: D4 (expected ~290-330 Hz, correctly tracks jump to first octave)
            else if (b >= 44 && b <= 54) {
                if (pitchHz < 270.0f || pitchHz > 340.0f) {
                    std::cout << "  ERROR: Block " << b << " (D4 Note) tracked incorrectly as " << pitchHz << " Hz (Expected 270-340 Hz)" << std::endl;
                    octaveDropCount++;
                }
            }
            // Note 3: F#3 (expected ~170-190 Hz)
            else if (b >= 198 && b <= 220) {
                if (pitchHz < 165.0f || pitchHz > 195.0f) {
                    std::cout << "  ERROR: Block " << b << " (F#3 Note) tracked incorrectly as " << pitchHz << " Hz (Expected 165-195 Hz)" << std::endl;
                    octaveDropCount++;
                }
            }
            // Note 4: A3 (expected ~190-240 Hz)
            else if (b >= 270 && b <= 330) {
                if (pitchHz < 185.0f || pitchHz > 245.0f) {
                    std::cout << "  ERROR: Block " << b << " (A3 Note) tracked incorrectly as " << pitchHz << " Hz (Expected 185-245 Hz)" << std::endl;
                    octaveDropCount++;
                }
            }
        }

        // Apply shift
        float ratio = 1.0f;
        if (isVoiced && pitchHz > 50.0f) {
            float midiNote = 69.0f + 12.0f * std::log2(pitchHz / 440.0f);
            float nearestMidiNote = std::round(midiNote);
            float targetHz = 440.0f * std::pow(2.0f, (nearestMidiNote - 69.0f) / 12.0f);
            ratio = targetHz / pitchHz;
        }

        shifter.setTargetShift(ratio, 0.0f, 0.0f, isVoiced, pitchHz, 0.0f);
        shifter.process(blockBuf);

        outputBuffer.copyFrom(0, b * block_size, blockBuf.getReadPointer(0), block_size);
    }

    // Save processed artifact
    TestHelpers::writeWavFile("test/result/jump_notes_out.wav", outputBuffer, sampleRate);

    std::cout << "File: " << juce::File(filename).getFileName().toStdString() << " (Octave Jump Error Verification Test)" << std::endl;
    std::cout << "  Voiced octave drop/tracking error instances: " << octaveDropCount << std::endl;
    std::cout << "  Processed output saved to: test/result/jump_notes_out.wav" << std::endl;

    if (octaveDropCount == 0) {
        std::cout << "  RESULT: PASS (Pitch tracking is perfectly octave-aligned)" << std::endl;
    } else {
        std::cerr << "  RESULT: FAIL (Pitch tracking dropped or locked to wrong octave " << octaveDropCount << " times!)" << std::endl;
        std::exit(1);
    }
}
