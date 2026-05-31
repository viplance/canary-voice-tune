#include "TestHelpers.h"
#include <iostream>

namespace TestHelpers
{
    juce::AudioBuffer<float> loadAndNormalizeWav(const juce::String& filepath, double& outSampleRate)
    {
        juce::File file(filepath);
        if (!file.exists()) {
            std::cerr << "Error: File does not exist: " << file.getFullPathName().toStdString() << std::endl;
            std::exit(1);
        }

        juce::AudioFormatManager formatManager;
        formatManager.registerBasicFormats();

        std::unique_ptr<juce::AudioFormatReader> reader(formatManager.createReaderFor(file));
        if (reader == nullptr) {
            std::cerr << "Error: Could not read format for file: " << file.getFullPathName().toStdString() << std::endl;
            std::exit(1);
        }

        outSampleRate = reader->sampleRate;
        int numSamples = (int)reader->lengthInSamples;
        int numChannels = reader->numChannels;

        juce::AudioBuffer<float> buffer(numChannels, numSamples);
        reader->read(&buffer, 0, numSamples, 0, true, true);

        // Mix down to mono if it was somehow stereo
        juce::AudioBuffer<float> monoBuffer(1, numSamples);
        monoBuffer.clear();
        for (int c = 0; c < numChannels; ++c) {
            monoBuffer.addFrom(0, 0, buffer.getReadPointer(c), numSamples, 1.0f / (float)numChannels);
        }

        // Normalize monoBuffer to peak = 0.5 (about -6 dBFS)
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

        return monoBuffer;
    }

    void ensureDirectoryExists(const juce::File& dir)
    {
        if (!dir.exists()) {
            if (!dir.createDirectory()) {
                std::cerr << "Error: Could not create directory: " << dir.getFullPathName().toStdString() << std::endl;
                std::exit(1);
            }
        }
    }

    void writeWavFile(const juce::String& filepath, const juce::AudioBuffer<float>& buffer, double sampleRate)
    {
        juce::File file(filepath);
        ensureDirectoryExists(file.getParentDirectory());

        if (file.exists()) {
            file.deleteFile();
        }

        juce::WavAudioFormat wavFormat;
        std::unique_ptr<juce::FileOutputStream> outStream(file.createOutputStream());
        if (outStream == nullptr) {
            std::cerr << "Error: Could not create output stream for: " << filepath.toStdString() << std::endl;
            return;
        }

        std::unique_ptr<juce::AudioFormatWriter> writer(
            wavFormat.createWriterFor(outStream.get(), sampleRate, (unsigned int)buffer.getNumChannels(), 16, {}, 0)
        );

        if (writer != nullptr) {
            outStream.release(); // Writer took ownership
            writer->writeFromAudioSampleBuffer(buffer, 0, buffer.getNumSamples());
        } else {
            std::cerr << "Error: Could not create WAV writer for: " << filepath.toStdString() << std::endl;
        }
    }
}
