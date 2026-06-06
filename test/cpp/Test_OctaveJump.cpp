#include "Test_OctaveJump.h"
#include "TestHelpers.h"
#include "TuneRender.h"
#include <iostream>

void runOctaveJumpTest(const juce::String& filename)
{
    double sampleRate = 0.0;
    juce::AudioBuffer<float> monoBuffer = TestHelpers::loadAndNormalizeWav(filename, sampleRate);

    // Render through the full plugin path (PitchDetector -> scale-locked note
    // selection / lock -> tuning ratio -> Classic shifter), so jump_notes_out.wav
    // behaves exactly like the plugin in a DAW. The note lock (NoteSelector +
    // release inertia) is what makes an octave misdetection audible: the locked
    // target stays on the true note, so a glitched octave yields a ratio that
    // audibly jumps the output — per-block "snap to own nearest semitone" would
    // hide it. Aggressive settings (Attack 0, Release 50) match the click test.
    juce::AudioBuffer<float> out =
        TuneRender::render(monoBuffer, sampleRate, /*Classic*/1,
                           /*attackMs*/0.0f, /*releaseMs*/50.0f);

    TestHelpers::writeWavFile("test/result/jump_notes_out.wav", out, sampleRate);
    std::cout << "  Rendered -> test/result/jump_notes_out.wav" << std::endl;
}
