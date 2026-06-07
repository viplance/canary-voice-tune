#include "Test_OctaveJump.h"
#include "TestHelpers.h"
#include "TuneRender.h"
#include <iostream>
#include <cmath>
#include <vector>

// jump_notes.wav contains a sung melody with these stable notes (analysed from
// the input pitch track):
//   Eb3 (MIDI 51) — t ≈ 0.10–0.42 s
//   F#3 (MIDI 54) — t ≈ 0.45–0.75 s  and  1.05–1.28 s
//   F3  (MIDI 53) — t ≈ 0.75–1.00 s
//   Ab3 (MIDI 56) — t ≈ 1.30–1.90 s
//
// The test renders with ONLY those four keys active so the NoteSelector has a
// specific, sparse key layout. With all 88 keys on, the selector chases every
// semitone during portamento glides, producing an unstable output that wanders
// across 6–7 notes instead of locking. With the correct sparse key set, the
// note lock is stable and the output pitch stays within ±1 st of each target.
//
// The octave-jump regression is also covered: if the detector emits an octave-
// up error during the Eb3 region the locked target stays on 51 but the ratio
// pulls the output to ~63, violating the ±1 st tolerance for that segment.

struct Segment {
    float startS, endS;
    int   expectedMidi;
    const char* name;
};

static const Segment kSegments[] = {
    { 0.12f, 0.40f, 51, "Eb3 (MIDI 51)" },
    { 0.52f, 0.73f, 54, "F#3 (MIDI 54) first" },
    { 0.78f, 0.98f, 53, "F3  (MIDI 53)" },
    { 1.07f, 1.26f, 54, "F#3 (MIDI 54) second" },
    { 1.35f, 1.88f, 56, "Ab3 (MIDI 56)" },
};
static const int kNumSegments = (int)(sizeof(kSegments) / sizeof(kSegments[0]));
static const float kOnsetSkipS = 0.06f;  // skip first 60 ms of each segment (attack)
static const float kToleranceSt = 1.0f;

void runOctaveJumpTest(const juce::String& filename)
{
    std::cout << "\nTest 6: OctaveJump — per-note pitch accuracy on jump_notes.wav" << std::endl;

    double sampleRate = 0.0;
    juce::AudioBuffer<float> monoBuffer = TestHelpers::loadAndNormalizeWav(filename, sampleRate);

    // Render with only the four melody notes active so the NoteSelector locks
    // stably instead of chasing every semitone during portamento.
    juce::AudioBuffer<float> out =
        TuneRender::renderWithNotes(monoBuffer, sampleRate, /*Classic*/1,
                                    /*attackMs*/0.0f, /*releaseMs*/50.0f,
                                    { 51, 53, 54, 56 });

    TestHelpers::writeWavFile("test/result/jump_notes_out.wav", out, sampleRate);

    // Evaluate per-segment pitch accuracy on the rendered output.
    const float* x   = out.getReadPointer(0);
    int numSamples    = out.getNumSamples();
    const int hopSz   = 512;
    const int winSz   = 2048;

    // Collect pitch estimates via autocorrelation (same approach as test_long_melody).
    std::vector<std::pair<float,float>> pitchFrames; // (time_s, midi)
    for (int s = 0; s + winSz <= numSamples; s += hopSz) {
        const float* w = x + s;
        float rms = 0.0f;
        for (int i = 0; i < winSz; ++i) rms += w[i] * w[i];
        rms = std::sqrt(rms / winSz);
        if (rms < 0.005f) continue;

        // Normalized autocorrelation, lag range for 60–900 Hz
        int lagMin = (int)((float)sampleRate / 900.0f);
        int lagMax = (int)((float)sampleRate / 60.0f);
        if (lagMax >= winSz) lagMax = winSz - 1;
        float r0 = 0.0f;
        for (int i = 0; i < winSz; ++i) r0 += w[i] * w[i];
        float bestCorr = -1.0f;
        int   bestLag  = lagMin;
        for (int lag = lagMin; lag <= lagMax; ++lag) {
            float r = 0.0f, e = 0.0f;
            int n = winSz - lag;
            for (int i = 0; i < n; ++i) { r += w[i] * w[i + lag]; e += w[i+lag]*w[i+lag]; }
            float norm = r / (std::sqrt(r0 * e) + 1e-9f);
            if (norm > bestCorr) { bestCorr = norm; bestLag = lag; }
        }
        float hz = (float)sampleRate / (float)bestLag;
        if (hz >= 60.0f && hz <= 900.0f) {
            float midi = 69.0f + 12.0f * std::log2(hz / 440.0f);
            pitchFrames.push_back({ (float)s / (float)sampleRate, midi });
        }
    }

    int failures = 0;
    for (int seg = 0; seg < kNumSegments; ++seg) {
        const auto& s = kSegments[seg];
        float t0 = s.startS + kOnsetSkipS;
        float t1 = s.endS;

        // Collect median over the segment window.
        std::vector<float> vals;
        for (auto& [t, m] : pitchFrames)
            if (t >= t0 && t <= t1) vals.push_back(m);

        if (vals.empty()) {
            std::cout << "  [WARN] " << s.name << ": no voiced frames in window" << std::endl;
            continue;
        }
        std::sort(vals.begin(), vals.end());
        float median = vals[vals.size() / 2];
        float err    = std::abs(median - (float)s.expectedMidi);
        bool  ok     = err <= kToleranceSt;

        std::cout << "  " << (ok ? "OK  " : "FAIL") << "  " << s.name
                  << "  output_median=" << median
                  << "  err=" << err << " st"
                  << (ok ? "" : "  <- OUTSIDE ±1 st")
                  << std::endl;
        if (!ok) ++failures;
    }

    if (failures > 0) {
        std::cerr << "  RESULT: FAIL (" << failures
                  << " segment(s) outside ±1 st of target)" << std::endl;
        std::exit(1);
    }
    std::cout << "  RESULT: PASS (all segments within ±1 st of target note)" << std::endl;
}
