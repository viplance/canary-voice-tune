#include <JuceHeader.h>
#include "Test_BreathGate.h"
#include "Test_Exciter.h"
#include "Test_NoteMapping.h"
#include "Test_ClassicClick.h"
#include "TestHelpers.h"
#include "../Source/DSP/PitchDetector.h"
#include <iostream>
#include <cmath>

void analyzeFile(const juce::String& filename) {
    double sampleRate = 0.0;
    juce::AudioBuffer<float> buffer = TestHelpers::loadAndNormalizeWav(filename, sampleRate);
    int numSamples = buffer.getNumSamples();
    
    PitchDetector detector;
    int block_size = 256;
    detector.prepare(sampleRate, block_size);
    
    int numBlocks = numSamples / block_size;
    std::cout << "\n===========================================" << std::endl;
    std::cout << "Analyzing Consonant/Sibilant Tracking for: " << juce::File(filename).getFileName().toStdString() << std::endl;
    std::cout << "===========================================" << std::endl;
    int activeBlocks = 0;
    int consonantBlocks = 0;
    
    for (int b = 0; b < numBlocks; ++b) {
        const float* blockData = buffer.getReadPointer(0, b * block_size);
        
        float sumSq = 0.0f;
        for (int i = 0; i < block_size; ++i) sumSq += blockData[i] * blockData[i];
        float rms = std::sqrt(sumSq / block_size);
        
        detector.process(blockData, block_size);
        bool isConsonant = detector.isConsonant();
        bool isBreath = detector.isBreath();
        
        if (rms > 0.005f) { // Active audio threshold
            activeBlocks++;
            std::cout << "Block " << b << ": RMS=" << rms 
                      << ", isConsonant=" << (isConsonant ? "YES" : "no")
                      << ", isBreath=" << (isBreath ? "YES" : "no") 
                      << ", Pitch=" << detector.getInstantPitch() << " Hz"
                      << ", ZCR=" << detector.getZcr()
                      << ", hfRatio=" << detector.getHfRatio()
                      << ", yinMin=" << detector.getYinMinValue() << std::endl;
            if (isConsonant) consonantBlocks++;
        }
    }
    std::cout << "Analysis Complete." << std::endl;
    std::cout << "  Total Active Blocks: " << activeBlocks << std::endl;
    std::cout << "  Consonant/Sibilant Blocks: " << consonantBlocks << " (" << (activeBlocks > 0 ? (float)consonantBlocks / activeBlocks * 100.0f : 0.0f) << "%)" << std::endl;
}

int main(int argc, char* argv[]) {
    if (argc < 3) {
        std::cerr << "Usage: CanaryVoiceTune_Tests <path_to_breath.wav> <path_to_no_breath.wav> [path_to_analyze.wav]" << std::endl;
        return 1;
    }

    if (argc >= 4) {
        analyzeFile(argv[3]);
        return 0;
    }

    std::cout << "===========================================" << std::endl;
    std::cout << "Running CanaryVoiceTune Smart Breath Gate & Exciter Tests..." << std::endl;
    std::cout << "===========================================" << std::endl;

    // Run Breath Gate Tests (Test 1 & Test 2)
    runBreathGateTests(argv[1], argv[2]);

    // Run Exciter Test (Test 3)
    std::cout << "\nTest 3: Verifying Vocal Exciter Tube Even Harmonics generation on no_breath.wav" << std::endl;
    runExciterTest(argv[2]);

    // Run Note Mapping Test (Test 4)
    runNoteMappingTests();

    // Run Classic Click Test (Test 5)
    std::cout << "\nTest 5: Verifying Classic Mode Attack 0 / Release 0 Click-Free Onset on dynamic_solo.wav" << std::endl;
    runClassicClickTest("test/samples/dynamic_solo.wav");

    std::cout << "\nAll tests PASSED successfully!" << std::endl;
    return 0;
}
