#include "Test_ClassicClick.h"
#include "TestHelpers.h"
#include "../Source/DSP/PitchDetector.h"
#include "../Source/DSP/PitchShifter.h"
#include <iostream>
#include <cmath>

void runClassicClickTest(const juce::String& filename)
{
    double sampleRate = 0.0;
    juce::AudioBuffer<float> monoBuffer = TestHelpers::loadAndNormalizeWav(filename, sampleRate);
    int numSamples = monoBuffer.getNumSamples();

    PitchDetector detector;
    PitchShifter shifter;
    
    int block_size = 256;
    detector.prepare(sampleRate, block_size);
    shifter.prepare(sampleRate, block_size);
    
    // Switch to Classic mode
    shifter.setTuningMode(1);
    shifter.setBreathGate(0.0f, false); // disabled
    shifter.setExciter(0.0f, false);   // disabled

    int numBlocks = numSamples / block_size;
    juce::AudioBuffer<float> outputBuffer(1, numSamples);
    outputBuffer.clear();

    for (int b = 0; b < numBlocks; ++b) {
        juce::AudioBuffer<float> blockBuf(2, block_size);
        blockBuf.clear();
        for (int c = 0; c < 2; ++c) {
            blockBuf.copyFrom(c, 0, monoBuffer.getReadPointer(0, b * block_size), block_size);
        }

        // Run pitch detection
        float pitchHz = detector.process(blockBuf.getReadPointer(0), block_size);
        bool isConsonant = detector.isConsonant();
        bool isVoiced = (pitchHz > 0.0f) && !isConsonant;
        pitchHz = detector.getInstantPitch();

        // Calculate pitch shift ratio to target nearest MIDI note
        float ratio = 1.0f;
        if (isVoiced && pitchHz > 50.0f) {
            float midiNote = 69.0f + 12.0f * std::log2(pitchHz / 440.0f);
            float nearestMidiNote = std::round(midiNote);
            float targetHz = 440.0f * std::pow(2.0f, (nearestMidiNote - 69.0f) / 12.0f);
            ratio = targetHz / pitchHz;
        }

        // Attack 0, Release 0
        shifter.setTargetShift(ratio, 0.0f, 0.0f, isVoiced, pitchHz, 0.0f);
        shifter.process(blockBuf);

        outputBuffer.copyFrom(0, b * block_size, blockBuf.getReadPointer(0), block_size);
    }

    // Save processed artifact
    TestHelpers::writeWavFile("test/result/classic_click_out.wav", outputBuffer, sampleRate);

    // Detect click transitions introduced by the pitch shifter.
    int clickCount = 0;
    float maxDelta = 0.0f;
    for (int i = 1; i < numSamples; ++i) {
        float delta = std::abs(outputBuffer.getSample(0, i) - outputBuffer.getSample(0, i - 1));
        if (delta > maxDelta) {
            maxDelta = delta;
        }
        if (delta > 0.28f) {
            // Check if there is a corresponding steep delta in the input signal within +/- 64 samples
            bool foundInInput = false;
            int startWin = std::max(1, i - 64);
            int endWin = std::min(numSamples - 1, i + 64);
            for (int j = startWin; j <= endWin; ++j) {
                float inDelta = std::abs(monoBuffer.getSample(0, j) - monoBuffer.getSample(0, j - 1));
                if (inDelta > 0.22f) {
                    foundInInput = true;
                    break;
                }
            }
            
            if (!foundInInput) {
                clickCount++;
                std::cout << "  INTRODUCED CLICK at sample " << i << ", delta: " << delta 
                          << ", values: [" << outputBuffer.getSample(0, i - 2) << ", "
                          << outputBuffer.getSample(0, i - 1) << ", "
                          << outputBuffer.getSample(0, i) << ", "
                          << outputBuffer.getSample(0, i + 1) << "]" << std::endl;
            }
        }
    }

    std::cout << "File: " << juce::File(filename).getFileName().toStdString() << " (Classic Mode Attack 0 / Release 0 Click Test)" << std::endl;
    std::cout << "  Max sample-to-sample delta: " << maxDelta << std::endl;
    std::cout << "  Detected click transitions: " << clickCount << std::endl;
    std::cout << "  Processed output saved to: test/result/classic_click_out.wav" << std::endl;

    if (clickCount == 0) {
        std::cout << "  RESULT: PASS (No noticeable clicks detected)" << std::endl;
    } else {
        std::cerr << "  RESULT: FAIL (Click count: " << clickCount << " is greater than 0!)" << std::endl;
        std::exit(1);
    }
}
