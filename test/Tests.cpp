#include <JuceHeader.h>
#include "../Source/DSP/PitchDetector.h"
#include "../Source/DSP/PitchShifter.h"
#include <iostream>
#include <cmath>

void runTest(const juce::String& filename, bool shouldDetectBreath) {
    juce::File file(filename);
    if (!file.exists()) {
        std::cerr << "Error: File does not exist: " << file.getFullPathName() << std::endl;
        std::exit(1);
    }

    juce::AudioFormatManager formatManager;
    formatManager.registerBasicFormats();
    
    std::unique_ptr<juce::AudioFormatReader> reader(formatManager.createReaderFor(file));
    if (reader == nullptr) {
        std::cerr << "Error: Could not read format for file: " << file.getFullPathName() << std::endl;
        std::exit(1);
    }

    double sampleRate = reader->sampleRate;
    int numSamples = (int)reader->lengthInSamples;
    int numChannels = reader->numChannels;

    juce::AudioBuffer<float> buffer(numChannels, numSamples);
    reader->read(&buffer, 0, numSamples, 0, true, true);

    // Mix down to mono
    juce::AudioBuffer<float> monoBuffer(1, numSamples);
    monoBuffer.clear();
    for (int c = 0; c < numChannels; ++c) {
        monoBuffer.addFrom(0, 0, buffer.getReadPointer(c), numSamples, 1.0f / (float)numChannels);
    }

    // Normalize monoBuffer to a healthy peak of 0.5 (about -6 dBFS)
    float maxVal = 0.0f;
    for (int i = 0; i < numSamples; ++i) {
        float absVal = std::abs(monoBuffer.getSample(0, i));
        if (absVal > maxVal) {
            maxVal = absVal;
        }
    }
    if (maxVal > 1e-6f) {
        float gain = 0.5f / maxVal;
        for (int i = 0; i < numSamples; ++i) {
            monoBuffer.setSample(0, i, monoBuffer.getSample(0, i) * gain);
        }
    }

    PitchDetector detector;
    PitchShifter shifter;
    
    int block_size = 256;
    detector.prepare(sampleRate, block_size);
    shifter.prepare(sampleRate, block_size);
    
    // Set the breath gate threshold to -48 dB to make sure it triggers on breath.wav
    shifter.setBreathGate(-48.0f, false);
    shifter.setExciter(0.0f, false); // Exciter disabled for breath tests

    int numBlocks = numSamples / block_size;
    bool detectedBreath = false;
    int detectedCount = 0;

    // Output buffer to hold output of pitch shifter
    juce::AudioBuffer<float> outputBuffer(1, numSamples);
    outputBuffer.clear();

    for (int b = 0; b < numBlocks; ++b) {
        // Copy block to temp block buffer
        juce::AudioBuffer<float> blockBuf(2, block_size); // PitchShifter processes 2 channels internally
        blockBuf.clear();
        for (int c = 0; c < 2; ++c) {
            blockBuf.copyFrom(c, 0, monoBuffer.getReadPointer(0, b * block_size), block_size);
        }

        // Run detection on mono signal
        detector.process(blockBuf.getReadPointer(0), block_size);
        
        bool isBreath = detector.isBreath();
        if (isBreath) {
            detectedBreath = true;
            detectedCount++;
        }

        // Run shifter
        shifter.setBreathGate(-48.0f, isBreath);
        shifter.process(blockBuf);

        // Copy channel 0 to output buffer
        outputBuffer.copyFrom(0, b * block_size, blockBuf.getReadPointer(0), block_size);
    }

    int latency = shifter.getLatencySamples();
    std::cout << "File: " << file.getFileName() << std::endl;
    std::cout << "  Sample Rate: " << sampleRate << " Hz" << std::endl;
    std::cout << "  Duration: " << (double)numSamples / sampleRate << " seconds" << std::endl;
    std::cout << "  Latency: " << latency << " samples (" << (double)latency / sampleRate * 1000.0 << " ms)" << std::endl;
    std::cout << "  Detected Breath blocks: " << detectedCount << " of " << numBlocks << " total blocks" << std::endl;

    // Calculate latency-aligned RMS
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
        // Expecting breath detection and ~-6 dB overall attenuation
        // Since the breath file might contain silence or fade-in/out, the overall attenuation
        // should be at least -3.5 dB (close to -6 dB).
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
        // Expecting no breath detection and 0 dB overall attenuation (within 0.5 dB)
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
}

void runExciterTest(const juce::String& filename) {
    juce::File file(filename);
    if (!file.exists()) {
        std::cerr << "Error: File does not exist: " << file.getFullPathName() << std::endl;
        std::exit(1);
    }

    juce::AudioFormatManager formatManager;
    formatManager.registerBasicFormats();
    
    std::unique_ptr<juce::AudioFormatReader> reader(formatManager.createReaderFor(file));
    if (reader == nullptr) {
        std::cerr << "Error: Could not read format for file: " << file.getFullPathName() << std::endl;
        std::exit(1);
    }

    double sampleRate = reader->sampleRate;
    int numSamples = (int)reader->lengthInSamples;
    int numChannels = reader->numChannels;

    juce::AudioBuffer<float> buffer(numChannels, numSamples);
    reader->read(&buffer, 0, numSamples, 0, true, true);

    // Mix down to mono
    juce::AudioBuffer<float> monoBuffer(1, numSamples);
    monoBuffer.clear();
    for (int c = 0; c < numChannels; ++c) {
        monoBuffer.addFrom(0, 0, buffer.getReadPointer(c), numSamples, 1.0f / (float)numChannels);
    }

    // Normalize monoBuffer to a healthy peak of 0.5 (about -6 dBFS)
    float maxVal = 0.0f;
    for (int i = 0; i < numSamples; ++i) {
        float absVal = std::abs(monoBuffer.getSample(0, i));
        if (absVal > maxVal) {
            maxVal = absVal;
        }
    }
    if (maxVal > 1e-6f) {
        float gain = 0.5f / maxVal;
        for (int i = 0; i < numSamples; ++i) {
            monoBuffer.setSample(0, i, monoBuffer.getSample(0, i) * gain);
        }
    }

    // We will run the audio through the PitchShifter twice:
    // Run 1: Exciter OFF (0.0 dB)
    // Run 2: Exciter ON (6.0 dB)
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
    
    // Now, we high-pass filter both outputs above 4.0 kHz to isolate the excited harmonic band
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
    
    std::cout << "File: " << file.getFileName() << " (Exciter Verification)" << std::endl;
    std::cout << "  High-Frequency RMS (Exciter OFF): " << rmsOff << std::endl;
    std::cout << "  High-Frequency RMS (Exciter ON):  " << rmsOn << std::endl;
    
    if (rmsOff > 1e-6f) {
        float ratio = rmsOn / rmsOff;
        std::cout << "  Harmonics Energy Ratio: " << ratio << " (Exciter ON vs OFF)" << std::endl;
        
        // We expect the Exciter-active high frequency RMS to be strictly greater
        // due to the newly generated harmonics. Specifically, at +6 dB exciter boost,
        // it should be at least 3% higher (ratio > 1.03).
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

void analyzeFile(const juce::String& filename) {
    juce::File file(filename);
    if (!file.exists()) {
        std::cerr << "Analysis Error: File does not exist: " << file.getFullPathName() << std::endl;
        return;
    }
    
    juce::AudioFormatManager formatManager;
    formatManager.registerBasicFormats();
    std::unique_ptr<juce::AudioFormatReader> reader(formatManager.createReaderFor(file));
    if (reader == nullptr) {
        std::cerr << "Analysis Error: Could not read format." << std::endl;
        return;
    }
    
    double sampleRate = reader->sampleRate;
    int numSamples = (int)reader->lengthInSamples;
    
    juce::AudioBuffer<float> buffer(1, numSamples);
    reader->read(&buffer, 0, numSamples, 0, true, false);
    
    PitchDetector detector;
    int block_size = 256;
    detector.prepare(sampleRate, block_size);
    
    int numBlocks = numSamples / block_size;
    std::cout << "\n===========================================" << std::endl;
    std::cout << "Analyzing Consonant/Sibilant Tracking for: " << file.getFileName() << std::endl;
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

    std::cout << "\nTest 1: Detecting breath in breath.wav & verifying ~-6dB reduction" << std::endl;
    runTest(argv[1], true);

    std::cout << "\nTest 2: Expecting NO breath in no_breath.wav & verifying 0dB change" << std::endl;
    runTest(argv[2], false);

    std::cout << "\nTest 3: Verifying Vocal Exciter Tube Even Harmonics generation on no_breath.wav" << std::endl;
    runExciterTest(argv[2]);

    std::cout << "\nAll tests PASSED successfully!" << std::endl;
    return 0;
}
