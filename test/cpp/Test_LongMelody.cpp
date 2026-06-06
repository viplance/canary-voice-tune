#include "Test_LongMelody.h"
#include "TestHelpers.h"
#include "TuneRender.h"
#include <iostream>
#include <cstdio>
#include <string>

// Renders long_melody.wav through the full plugin signal path (see TuneRender.h)
// once per shift engine, writing test/result/long_melody_modern.wav (engine 0)
// and test/result/long_melody_classic.wav (engine 1) so the tuned result can be
// auditioned. All 88 keys enabled (plugin default), no Range control.

namespace {

void renderEngine(const juce::AudioBuffer<float>& input, double sampleRate,
                  int tuningMode, const char* label, const char* wavPath)
{
    const float attackMs  = 0.1f;
    const float releaseMs = 10.0f;

    juce::AudioBuffer<float> out =
        TuneRender::render(input, sampleRate, tuningMode, attackMs, releaseMs);

    TestHelpers::writeWavFile(wavPath, out, sampleRate);

    // Latency metadata for the Python verifier — re-derive from a prepared engine.
    PitchShifter shifter;
    shifter.prepare(sampleRate, 256);
    shifter.setTuningMode(tuningMode);
    int latSamples = shifter.getLatencySamples();
    {
        std::string metaPath = std::string(wavPath) + ".lat";
        if (FILE* f = std::fopen(metaPath.c_str(), "w")) {
            std::fprintf(f, "%d\n", latSamples);
            std::fclose(f);
        }
    }
    std::cout << "  [" << label << "] rendered -> " << wavPath
              << "  latency=" << latSamples
              << " samples (" << (1000.0 * latSamples / sampleRate) << " ms)"
              << std::endl;
}

} // namespace

void runLongMelodyTest(const juce::String& filename)
{
    double sampleRate = 0.0;
    juce::AudioBuffer<float> monoBuffer = TestHelpers::loadAndNormalizeWav(filename, sampleRate);

    std::cout << "File: " << juce::File(filename).getFileName().toStdString()
              << " (Long Melody — render both engines)" << std::endl;

    renderEngine(monoBuffer, sampleRate, 0, "MODERN",  "test/result/long_melody_modern.wav");
    renderEngine(monoBuffer, sampleRate, 1, "CLASSIC", "test/result/long_melody_classic.wav");
}
