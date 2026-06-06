#include <JuceHeader.h>
#include "Test_BreathGate.h"
#include "Test_Exciter.h"
#include "Test_ClassicClick.h"
#include "Test_OctaveJump.h"
#include "Test_LongMelody.h"
#include "Test_NoteMapping.h"
#include "Test_NoteSelector.h"
#include "Test_HighlightOctave.h"
#include "Test_19Notes.h"
#include "TestHelpers.h"
#include "../../Source/DSP/PitchDetector.h"
#include <iostream>
#include <cmath>
#include <cstdlib>

void analyzeFile(const juce::String& filename) {
    double sampleRate = 0.0;
    juce::AudioBuffer<float> buffer = TestHelpers::loadAndNormalizeWav(filename, sampleRate);
    int numSamples = buffer.getNumSamples();
    
    PitchDetector detector;
    int block_size = 256;
    if (const char* bsEnv = std::getenv("ANALYZE_BLOCK_SIZE")) {
        int v = std::atoi(bsEnv);
        if (v > 0) block_size = v;
    }
    detector.prepare(sampleRate, block_size);
    std::cout << "(analysis block_size = " << block_size << ")" << std::endl;
    
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
    // Optional: pass a wav path as the only argument to run the analyzer.
    if (argc == 2) {
        analyzeFile(argv[1]);
        return 0;
    }

    std::cout << "===========================================" << std::endl;
    std::cout << "Stage 1: Rendering test/samples -> test/result (C++)" << std::endl;
    std::cout << "===========================================" << std::endl;

    runBreathGateTests("test/samples/breath.wav", "test/samples/no_breath.wav");
    runExciterTest("test/samples/no_breath.wav");
    runClassicClickTest("test/samples/dynamic_solo.wav");
    runOctaveJumpTest("test/samples/jump_notes.wav");
    runLongMelodyTest("test/samples/long_melody.wav");
    
    // Additional automated C++ validation tests
    runNoteMappingTests();
    runNoteSelectorTests();
    runHighlightOctaveTest("test/samples/long_melody.wav");
    run19NotesTest("test/samples/long_melody.wav");

    std::cout << "\nAll renders and C++ checks complete. Run test/python/run_all.py to verify." << std::endl;
    return 0;
}
