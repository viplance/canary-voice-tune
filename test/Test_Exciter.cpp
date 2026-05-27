#include "Test_Exciter.h"
#include "TestHelpers.h"
#include "../Source/DSP/PitchDetector.h"
#include "../Source/DSP/PitchShifter.h"
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
    shifterOn.setExciter(6.0f, false);
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
    
    // High-pass filter both outputs above 4.0 kHz to isolate the excited harmonic band
    juce::dsp::ProcessSpec spec;
    spec.sampleRate = sampleRate;
    spec.maximumBlockSize = (juce::uint32)numSamples;
    spec.numChannels = 1;
    
    juce::dsp::IIR::Filter<float> hpfOff;
    juce::dsp::IIR::Filter<float> hpfOn;
    hpfOff.prepare(spec);
    hpfOn.prepare(spec);
    
    auto coeffs = juce::dsp::IIR::Coefficients<float>::makeHighPass(sampleRate, 4000.0f);
    *hpfOff.coefficients = *coeffs;
    *hpfOn.coefficients = *coeffs;
    
    hpfOff.reset();
    hpfOn.reset();
    
    juce::AudioBuffer<float> filteredOff(1, numSamples);
    juce::AudioBuffer<float> filteredOn(1, numSamples);
    filteredOff.copyFrom(0, 0, outOff.getReadPointer(0), numSamples);
    filteredOn.copyFrom(0, 0, outOn.getReadPointer(0), numSamples);
    
    float* ptrOff = filteredOff.getWritePointer(0);
    float* ptrOn = filteredOn.getWritePointer(0);
    juce::dsp::AudioBlock<float> blockOff(&ptrOff, 1, (size_t)numSamples);
    juce::dsp::AudioBlock<float> blockOn(&ptrOn, 1, (size_t)numSamples);
    juce::dsp::ProcessContextReplacing<float> ctxOff(blockOff);
    juce::dsp::ProcessContextReplacing<float> ctxOn(blockOn);
    
    hpfOff.process(ctxOff);
    hpfOn.process(ctxOn);
    
    // Aligned comparison (accounting for latency)
    int latency = shifterOff.getLatencySamples();
    double sumSqOff = 0.0;
    double sumSqOn = 0.0;
    int count = 0;
    
    for (int i = latency; i < numSamples; ++i) {
        float sampleOff = filteredOff.getSample(0, i);
        float sampleOn = filteredOn.getSample(0, i);
        sumSqOff += sampleOff * sampleOff;
        sumSqOn += sampleOn * sampleOn;
        count++;
    }
    
    float rmsOff = (count > 0) ? std::sqrt(sumSqOff / count) : 0.0f;
    float rmsOn = (count > 0) ? std::sqrt(sumSqOn / count) : 0.0f;
    
    std::cout << "File: " << juce::File(filename).getFileName().toStdString() << " (Exciter Verification)" << std::endl;
    std::cout << "  High-Frequency RMS (Exciter OFF): " << rmsOff << std::endl;
    std::cout << "  High-Frequency RMS (Exciter ON):  " << rmsOn << std::endl;
    std::cout << "  Processed outputs saved to: test/result/exciter_off_out.wav & exciter_on_out.wav" << std::endl;
    
    if (rmsOff > 1e-6f) {
        float ratio = rmsOn / rmsOff;
        std::cout << "  Harmonics Energy Ratio: " << ratio << " (Exciter ON vs OFF)" << std::endl;
        
        if (ratio > 1.03f) {
            std::cout << "  RESULT: PASS (Harmonics successfully generated and blended)" << std::endl;
        } else {
            std::cerr << "  RESULT: FAIL (Harmonics were not generated or too weak, ratio: " << ratio << ")" << std::endl;
            std::exit(1);
        }
    } else {
        std::cerr << "  RESULT: FAIL (Audio contains no high-frequency energy to excite)" << std::endl;
        std::exit(1);
    }
}
