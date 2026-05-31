#pragma once
#include <JuceHeader.h>

namespace TestHelpers
{
    // Loads a mono WAV file and normalizes its peak to 0.5 (about -6 dBFS).
    // Verifies that the loaded audio is indeed mono, exits with error if reading fails.
    juce::AudioBuffer<float> loadAndNormalizeWav(const juce::String& filepath, double& outSampleRate);

    // Ensures that the target directory exists, creating it if necessary.
    void ensureDirectoryExists(const juce::File& dir);

    // Writes a mono AudioBuffer as a 16-bit PCM WAV file.
    void writeWavFile(const juce::String& filepath, const juce::AudioBuffer<float>& buffer, double sampleRate);
}
