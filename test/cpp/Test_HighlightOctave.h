#pragma once
#include <JuceHeader.h>

// Reproduces the PLUGIN's displayed-note (keyboard highlight) chain on a sample
// and verifies the highlighted note does not jump an octave away from the sung
// pitch. Unlike Test_OctaveJump (which asserts the raw getInstantPitch in Hz),
// this exercises the floatMidi -> smoothedMidi(jump-bypass) -> bestMidi mapping
// that actually drives the on-screen highlight, plus the plugin's
// instant->smoothed pitch fallback and host-realistic block size.
void runHighlightOctaveTest(const juce::String& filename);
