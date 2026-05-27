#include "Test_BreathGate.h"
#include "TestHelpers.h"
#include "../Source/DSP/PitchDetector.h"
#include "../Source/DSP/PitchShifter.h"
#include <iostream>
#include <cmath>

void runBreathGateTests(const juce::String& breathPath, const juce::String& noBreathPath)
{
    auto runSingleBreathGateTest = [](const juce::String& filename, bool shouldDetectBreath, const juce::String& outFilename) {
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
        int detectedCount = 0;
        bool detectedBreath = false;

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
            if (isBreath) {
                detectedBreath = true;
                detectedCount++;
            }

            shifter.setBreathGate(-48.0f, isBreath);
            shifter.process(blockBuf);

            outputBuffer.copyFrom(0, b * block_size, blockBuf.getReadPointer(0), block_size);
        }

        // Save processed artifact
        TestHelpers::writeWavFile(outFilename, outputBuffer, sampleRate);

        int latency = shifter.getLatencySamples();
        std::cout << "File: " << juce::File(filename).getFileName().toStdString() << std::endl;
        std::cout << "  Sample Rate: " << sampleRate << " Hz" << std::endl;
        std::cout << "  Duration: " << (double)numSamples / sampleRate << " seconds" << std::endl;
        std::cout << "  Latency: " << latency << " samples (" << (double)latency / sampleRate * 1000.0 << " ms)" << std::endl;
        std::cout << "  Detected Breath blocks: " << detectedCount << " of " << numBlocks << " total blocks" << std::endl;
        std::cout << "  Processed output saved to: " << outFilename.toStdString() << std::endl;

        double sumSqInput = 0.0;
        double sumSqOutput = 0.0;
        int count = 0;
        
        for (int i = 0; i < numSamples - latency; ++i) {
            float inSample = monoBuffer.getSample(0, i);
            float outSample = outputBuffer.getSample(0, i + latency);
            
            sumSqInput += inSample * inSample;
            sumSqOutput += outSample * outSample;
            count++;
        }
        
        float alignedInputRms = (count > 0) ? std::sqrt(sumSqInput / count) : 0.0f;
        float alignedOutputRms = (count > 0) ? std::sqrt(sumSqOutput / count) : 0.0f;
        float attenuationDb = (alignedInputRms > 1e-9f) ? 20.0f * std::log10(alignedOutputRms / alignedInputRms) : 0.0f;

        std::cout << "  Aligned Input RMS:  " << alignedInputRms << std::endl;
        std::cout << "  Aligned Output RMS: " << alignedOutputRms << std::endl;
        std::cout << "  Overall Attenuation: " << attenuationDb << " dB" << std::endl;

        if (shouldDetectBreath) {
            bool passesRms = (attenuationDb < -3.5f) && (attenuationDb > -7.0f);
            if (detectedBreath && passesRms) {
                std::cout << "  RESULT: PASS" << std::endl;
            } else {
                std::cerr << "  RESULT: FAIL" << std::endl;
                if (!detectedBreath) {
                    std::cerr << "    Expected breath to be detected, but it was NOT." << std::endl;
                }
                if (!passesRms) {
                    std::cerr << "    Expected attenuation to be around -6 dB, but got " << attenuationDb << " dB." << std::endl;
                }
                std::exit(1);
            }
        } else {
            bool passesRms = std::abs(attenuationDb) < 0.5f;
            if (!detectedBreath && passesRms) {
                std::cout << "  RESULT: PASS" << std::endl;
            } else {
                std::cerr << "  RESULT: FAIL" << std::endl;
                if (detectedBreath) {
                    std::cerr << "    Expected NO breath, but it was falsely detected." << std::endl;
                }
                if (!passesRms) {
                    std::cerr << "    Expected 0 dB attenuation (unaffected), but got " << attenuationDb << " dB." << std::endl;
                }
                std::exit(1);
            }
        }
    };

    std::cout << "\nTest 1: Detecting breath in breath.wav & verifying ~-6dB reduction" << std::endl;
    runSingleBreathGateTest(breathPath, true, "test/result/breath_gate_breath_out.wav");

    std::cout << "\nTest 2: Expecting NO breath in no_breath.wav & verifying 0dB change" << std::endl;
    runSingleBreathGateTest(noBreathPath, false, "test/result/breath_gate_no_breath_out.wav");
}
