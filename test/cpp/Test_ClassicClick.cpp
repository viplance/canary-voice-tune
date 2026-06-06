#include "Test_ClassicClick.h"
#include "TestHelpers.h"
#include "TuneRender.h"
#include <iostream>
#include <cmath>

// Aggressive, clearly-audible tuning settings so the rendered artifacts match
// what the plugin does in a DAW (the crude per-block round-to-nearest the test
// used before bypassed note-locking and barely moved the pitch). Attack 0 snaps
// instantly onto the target note; Release 50 ms re-arms note switches quickly.
static constexpr float kAttackMs  = 0.0f;
static constexpr float kReleaseMs = 50.0f;

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
    std::cout << "\nTest 5: Classic-mode click detection (Attack 0, Release 50)" << std::endl;

    // dynamic_solo is the original regression fixture; zaberi reproduces the
    // transition click around 3.5 s reported by the user. Both inputs contain
    // their own incoming clicks (rough vocal fry, recording glitches), so the
    // test is input-relative: it fails only if the Classic shifter *adds*
    // clicks beyond those already present in the source audio.
    //
    // Both engines are rendered for zaberi so the tuned result can be auditioned
    // and compared the same way the plugin sounds in a DAW.
    struct Case { const char* in; const char* out; int mode; bool gate; };
    static const Case cases[] = {
        { "test/samples/dynamic_solo.wav", "test/result/classic_click_out.wav", 1, true  },
        { "test/samples/zaberi_in.wav",    "test/result/zaberi_classic.wav",    1, true  },
        { "test/samples/zaberi_in.wav",    "test/result/zaberi_modern.wav",     0, false },
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
        juce::AudioBuffer<float> out  =
            TuneRender::render(mono, sr, c.mode, kAttackMs, kReleaseMs);
        TestHelpers::writeWavFile(c.out, out, sr);

        ClickReport in  = detectClicks(mono.getReadPointer(0), mono.getNumSamples(), sr);
        ClickReport rep = detectClicks(out.getReadPointer(0),  out.getNumSamples(),  sr);

        const char* engine = (c.mode == 0) ? "MODERN " : "CLASSIC";
        std::cout << "  [" << engine << "] " << c.in << " -> " << c.out
                  << "  | input clicks=" << in.count
                  << "  output clicks=" << rep.count
                  << "  added=" << juce::jmax(0, rep.count - in.count);
        if (rep.count > 0)
            std::cout << "  worst=" << rep.worstScore << " @ " << rep.worstTimeS << "s";
        std::cout << std::endl;

        // Only the click gate (Classic transition clicks) is asserted; Modern is
        // rendered for audition only.
        if (c.gate && (rep.count - in.count) > 0) ++failures;
    }

    if (failures > 0) {
        std::cerr << "  RESULT: FAIL (" << failures
                  << " file(s) gained Classic-mode clicks vs. their input)" << std::endl;
        std::exit(1);
    }
    std::cout << "  RESULT: PASS (Classic shifter added no clicks over the source audio)" << std::endl;
}
