#include "Test_BreathGate.h"
#include "TestHelpers.h"
#include "../../Source/DSP/PitchDetector.h"
#include "../../Source/DSP/PitchShifter.h"
#include <iostream>
#include <cmath>

void runBreathGateTests(const juce::String& breathPath, const juce::String& noBreathPath)
{
    auto runSingleBreathGateTest = [](const juce::String& filename, bool /*shouldDetectBreath*/, const juce::String& outFilename) {
        double sampleRate = 0.0;
        juce::AudioBuffer<float> monoBuffer = TestHelpers::loadAndNormalizeWav(filename, sampleRate);
        int numSamples = monoBuffer.getNumSamples();

        PitchDetector detector;
        PitchShifter shifter;
        
        int block_size = 256;
        detector.prepare(sampleRate, block_size);
        shifter.prepare(sampleRate, block_size);
        
        shifter.setBreathGate(-48.0f, false);
        shifter.setExciter(0.0f, false); // Exciter disabled for breath tests

        int numBlocks = numSamples / block_size;
        
        

        juce::AudioBuffer<float> outputBuffer(1, numSamples);
        outputBuffer.clear();

        for (int b = 0; b < numBlocks; ++b) {
            juce::AudioBuffer<float> blockBuf(2, block_size);
            blockBuf.clear();
            for (int c = 0; c < 2; ++c) {
                blockBuf.copyFrom(c, 0, monoBuffer.getReadPointer(0, b * block_size), block_size);
            }

            detector.process(blockBuf.getReadPointer(0), block_size);
            
            bool isBreath = detector.isBreath();
            shifter.setBreathGate(-48.0f, isBreath);
            shifter.process(blockBuf);

            outputBuffer.copyFrom(0, b * block_size, blockBuf.getReadPointer(0), block_size);
        }

        // Save processed artifact
        TestHelpers::writeWavFile(outFilename, outputBuffer, sampleRate);

        std::cout << "  Rendered -> " << outFilename.toStdString() << std::endl;
    };

    std::cout << "\nRendering breath gate outputs..." << std::endl;
    runSingleBreathGateTest(breathPath, true, "test/result/breath_gate_breath_out.wav");
    runSingleBreathGateTest(noBreathPath, false, "test/result/breath_gate_no_breath_out.wav");
}
