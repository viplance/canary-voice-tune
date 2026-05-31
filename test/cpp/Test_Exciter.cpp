#include "Test_Exciter.h"
#include "TestHelpers.h"
#include "../../Source/DSP/PitchDetector.h"
#include "../../Source/DSP/PitchShifter.h"
#include <iostream>
#include <cmath>

void runExciterTest(const juce::String& filename)
{
    double sampleRate = 0.0;
    juce::AudioBuffer<float> monoBuffer = TestHelpers::loadAndNormalizeWav(filename, sampleRate);
    int numSamples = monoBuffer.getNumSamples();

    PitchDetector detector;
    PitchShifter shifterOff;
    PitchShifter shifterOn;
    
    int block_size = 256;
    detector.prepare(sampleRate, block_size);
    shifterOff.prepare(sampleRate, block_size);
    shifterOn.prepare(sampleRate, block_size);
    
    shifterOff.setExciter(0.0f, false);
    shifterOn.setExciter(12.0f, false);
    shifterOff.setBreathGate(0.0f, false); // disabled
    shifterOn.setBreathGate(0.0f, false);  // disabled
    
    int numBlocks = numSamples / block_size;
    
    juce::AudioBuffer<float> outOff(1, numSamples);
    juce::AudioBuffer<float> outOn(1, numSamples);
    outOff.clear();
    outOn.clear();
    
    for (int b = 0; b < numBlocks; ++b) {
        juce::AudioBuffer<float> blockOff(2, block_size);
        juce::AudioBuffer<float> blockOn(2, block_size);
        blockOff.clear();
        blockOn.clear();
        for (int c = 0; c < 2; ++c) {
            blockOff.copyFrom(c, 0, monoBuffer.getReadPointer(0, b * block_size), block_size);
            blockOn.copyFrom(c, 0, monoBuffer.getReadPointer(0, b * block_size), block_size);
        }
        
        shifterOff.process(blockOff);
        shifterOn.process(blockOn);
        
        outOff.copyFrom(0, b * block_size, blockOff.getReadPointer(0), block_size);
        outOn.copyFrom(0, b * block_size, blockOn.getReadPointer(0), block_size);
    }

    // Save processed artifacts
    TestHelpers::writeWavFile("test/result/exciter_off_out.wav", outOff, sampleRate);
    TestHelpers::writeWavFile("test/result/exciter_on_out.wav", outOn, sampleRate);
    
    std::cout << "  Rendered -> test/result/exciter_off_out.wav & exciter_on_out.wav" << std::endl;
}
