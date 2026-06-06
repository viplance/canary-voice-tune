#include "Test_ClassicClick.h"
#include "TestHelpers.h"
#include "../../Source/DSP/PitchDetector.h"
#include "../../Source/DSP/PitchShifter.h"
#include <iostream>
#include <cmath>
#include <vector>

// Renders `mono` through the Classic pitch shifter (auto-tune to nearest note,
// attack/release 0).
static juce::AudioBuffer<float> renderClassic(const juce::AudioBuffer<float>& mono,
                                              double sampleRate)
{
    int numSamples = mono.getNumSamples();

    PitchDetector detector;
    PitchShifter shifter;

    int block_size = 256;
    detector.prepare(sampleRate, block_size);
    shifter.prepare(sampleRate, block_size);

    shifter.setTuningMode(1);          // Classic
    shifter.setBreathGate(0.0f, false);
    shifter.setExciter(0.0f, false);

    int numBlocks = numSamples / block_size;
    juce::AudioBuffer<float> outputBuffer(1, numSamples);
    outputBuffer.clear();

    for (int b = 0; b < numBlocks; ++b) {
        juce::AudioBuffer<float> blockBuf(2, block_size);
        blockBuf.clear();
        for (int c = 0; c < 2; ++c)
            blockBuf.copyFrom(c, 0, mono.getReadPointer(0, b * block_size), block_size);

        float pitchHz = detector.process(blockBuf.getReadPointer(0), block_size);
        bool isConsonant = detector.isConsonant();
        bool isVoiced = (pitchHz > 0.0f) && !isConsonant;
        pitchHz = detector.getInstantPitch();

        float ratio = 1.0f;
        if (isVoiced && pitchHz > 50.0f) {
            float midiNote = 69.0f + 12.0f * std::log2(pitchHz / 440.0f);
            float nearestMidiNote = std::round(midiNote);
            float targetHz = 440.0f * std::pow(2.0f, (nearestMidiNote - 69.0f) / 12.0f);
            ratio = targetHz / pitchHz;
        }

        shifter.setTargetShift(ratio, 0.0f, 0.0f, isVoiced, pitchHz, 0.0f);
        shifter.process(blockBuf);

        outputBuffer.copyFrom(0, b * block_size, blockBuf.getReadPointer(0), block_size);
    }

    return outputBuffer;
}

// A click is an isolated hard waveform discontinuity. The second difference
// |x[n] - 2x[n-1] + x[n-2]| spikes at a discontinuity; an absolute threshold of
// ~0.12 (on peak-0.5-normalised audio) cleanly separates real clicks from the
// per-sample curvature of normal voiced harmonics. Counts are de-duplicated
// with a 5 ms refractory so one event isn't tallied multiple times.
struct ClickReport {
    int    count = 0;
    double worstTimeS = 0.0;
    float  worstScore = 0.0f;
};

static ClickReport detectClicks(const float* x, int n, double sampleRate)
{
    ClickReport rep;
    if (n < 16) return rep;

    // Ignore the final block: the offline render truncates to whole blocks, so
    // the last voiced sample steps to the trailing zero pad — a render boundary,
    // not a shifter click.
    int scanEnd = n - 256;
    if (scanEnd <= 2) return rep;

    const float kThresh    = 0.12f;
    const int   refractory = (int)(sampleRate * 0.005);

    int lastClick = -refractory;
    for (int i = 2; i < scanEnd; ++i) {
        float d2 = std::abs(x[i] - 2.0f * x[i - 1] + x[i - 2]);
        if (d2 >= kThresh && (i - lastClick) >= refractory) {
            ++rep.count;
            lastClick = i;
            if (d2 > rep.worstScore) {
                rep.worstScore = d2;
                rep.worstTimeS = (double)i / sampleRate;
            }
        }
    }
    return rep;
}

void runClassicClickTest(const juce::String& filename)
{
    std::cout << "\nTest 5: Classic-mode click detection" << std::endl;

    // dynamic_solo is the original regression fixture; zaberi reproduces the
    // transition click around 3.5 s reported by the user. Both inputs contain
    // their own incoming clicks (rough vocal fry, recording glitches), so the
    // test is input-relative: it fails only if the Classic shifter *adds*
    // clicks beyond those already present in the source audio.
    struct Case { const char* in; const char* out; };
    static const Case cases[] = {
        { "test/samples/dynamic_solo.wav", "test/result/classic_click_out.wav" },
        { "test/samples/zaberi_in.wav",    "test/result/zaberi_classic.wav"    },
    };

    int failures = 0;
    for (const auto& c : cases) {
        juce::File inFile(juce::File::getCurrentWorkingDirectory().getChildFile(c.in));
        if (! inFile.existsAsFile()) {
            std::cout << "  [skip] " << c.in << " not present" << std::endl;
            continue;
        }

        double sr = 0.0;
        juce::AudioBuffer<float> mono = TestHelpers::loadAndNormalizeWav(c.in, sr);
        juce::AudioBuffer<float> out  = renderClassic(mono, sr);
        TestHelpers::writeWavFile(c.out, out, sr);

        ClickReport in  = detectClicks(mono.getReadPointer(0), mono.getNumSamples(), sr);
        ClickReport rep = detectClicks(out.getReadPointer(0),  out.getNumSamples(),  sr);

        // Allow the input's own clicks to pass through; flag only added ones.
        int added = rep.count - in.count;

        std::cout << "  " << c.in << " -> " << c.out
                  << "  | input clicks=" << in.count
                  << "  output clicks=" << rep.count
                  << "  added=" << juce::jmax(0, added);
        if (rep.count > 0)
            std::cout << "  worst=" << rep.worstScore << " @ " << rep.worstTimeS << "s";
        std::cout << std::endl;

        if (added > 0) ++failures;
    }

    if (failures > 0) {
        std::cerr << "  RESULT: FAIL (" << failures
                  << " file(s) gained Classic-mode clicks vs. their input)" << std::endl;
        std::exit(1);
    }
    std::cout << "  RESULT: PASS (Classic shifter added no clicks over the source audio)" << std::endl;
}
