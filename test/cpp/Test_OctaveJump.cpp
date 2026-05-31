#include "Test_OctaveJump.h"
#include "TestHelpers.h"
#include "../../Source/DSP/PitchDetector.h"
#include "../../Source/DSP/PitchShifter.h"
#include <iostream>
#include <cmath>

void runOctaveJumpTest(const juce::String& filename)
{
    double sampleRate = 0.0;
    juce::AudioBuffer<float> monoBuffer = TestHelpers::loadAndNormalizeWav(filename, sampleRate);
    int numSamples = monoBuffer.getNumSamples();

    PitchDetector detector;
    PitchShifter shifter;
    
    int block_size = 256;
    detector.prepare(sampleRate, block_size);
    shifter.prepare(sampleRate, block_size);
    
    shifter.setBreathGate(0.0f, false);
    shifter.setExciter(0.0f, false);

    int numBlocks = numSamples / block_size;
    juce::AudioBuffer<float> outputBuffer(1, numSamples);
    outputBuffer.clear();

    for (int b = 0; b < numBlocks; ++b) {
        juce::AudioBuffer<float> blockBuf(2, block_size);
        blockBuf.clear();
        for (int c = 0; c < 2; ++c) {
            blockBuf.copyFrom(c, 0, monoBuffer.getReadPointer(0, b * block_size), block_size);
        }

        // Run pitch detection
        detector.process(blockBuf.getReadPointer(0), block_size);
        bool isConsonant = detector.isConsonant();
        float pitchHz = detector.getInstantPitch();
        bool isVoiced = (pitchHz > 0.0f) && !isConsonant;

        // Apply shift.
        //
        // The real plugin tunes toward a SCALE-LOCKED target note (the active
        // key nearest the locked pitch), not the per-block nearest semitone.
        // That distinction matters here: per-block "snap to my own nearest
        // semitone" maps both 160 Hz and its octave-up 320 Hz to a ratio near
        // 1.0, so an octave detection error leaves NO audible trace in the
        // output WAV. To make the artifact representative of the plugin — so
        // the jump is actually audible if the detector regresses — we lock the
        // first note (250-400 ms region) to a fixed D3 (MIDI 50) target. An
        // octave-up misdetection then yields a ratio that audibly drops the
        // output ~an octave, exactly as it would in the plugin.
        float ratio = 1.0f;
        if (isVoiced && pitchHz > 50.0f) {
            float targetHz;
            if (b >= 26 && b <= 75) {
                // First note locked to D3 = 146.83 Hz (MIDI 50).
                targetHz = 440.0f * std::pow(2.0f, (50.0f - 69.0f) / 12.0f);
            } else {
                float midiNote = 69.0f + 12.0f * std::log2(pitchHz / 440.0f);
                float nearestMidiNote = std::round(midiNote);
                targetHz = 440.0f * std::pow(2.0f, (nearestMidiNote - 69.0f) / 12.0f);
            }
            ratio = targetHz / pitchHz;
        }

        shifter.setTargetShift(ratio, 0.0f, 0.0f, isVoiced, pitchHz, 0.0f);
        shifter.process(blockBuf);

        outputBuffer.copyFrom(0, b * block_size, blockBuf.getReadPointer(0), block_size);
    }

    // Save processed artifact
    TestHelpers::writeWavFile("test/result/jump_notes_out.wav", outputBuffer, sampleRate);

    std::cout << "  Rendered -> test/result/jump_notes_out.wav" << std::endl;
}
