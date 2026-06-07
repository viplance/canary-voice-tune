#pragma once
#include <JuceHeader.h>

// Verifies that PitchDetector tracks long_melody.wav without spurious
// octave-UP jumps. The melody is a slow, mostly-stepwise line; every note
// is sustained for tens of blocks. Any voiced block whose detected pitch
// briefly leaps ~an octave above the surrounding stable pitch (and then
// falls back) is an octave-tracking defect, not real musical motion.
//
// Known failing regions in the current detector (48 kHz, 256-sample blocks):
//   * first note  (~0.27 s): stable ~250-269 Hz -> ~542-575 Hz (C#5)
//   * ~3.85 s    : same note again, ~261 Hz -> ~555 Hz (C#5)
//   * 5 s -> end : many short jumps an octave up (E5 ~671 Hz, D5, C5, A#4 ...)
void runLongMelodyTest(const juce::String& filename);
