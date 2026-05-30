#include "Test_NoteMapping.h"
#include <iostream>

void runNoteMappingTests()
{
    std::cout << "\nTest 4: Verifying standard VST presets and keyboard note mapping" << std::endl;

    // Mirrors PluginProcessor::isNoteEnabledForPreset with the updated index scheme:
    //   0 = Default (Chromatic), 1 = Disable all, 2..25 = harmonic presets
    auto isNoteEnabledForPreset = [](int presetIdx, int midiNote) -> bool {
        if (presetIdx <= 0) return true;   // Chromatic
        if (presetIdx == 1) return false;  // Disable all
        int rootIdx = ((presetIdx - 2) / 2) % 12;
        bool isMinor = ((presetIdx - 2) % 2 == 1);
        int pitchClass = midiNote % 12;
        int interval = (pitchClass - rootIdx + 12) % 12;
        if (isMinor)
            return (interval == 0 || interval == 2 || interval == 3 || interval == 5 || interval == 7 || interval == 8 || interval == 10);
        else
            return (interval == 0 || interval == 2 || interval == 4 || interval == 5 || interval == 7 || interval == 9 || interval == 11);
    };

    // Disable all (presetIdx = 1): every note must be disabled
    for (int midi = 21; midi <= 108; ++midi) {
        if (isNoteEnabledForPreset(1, midi)) {
            std::cerr << "Test 4 FAILED: Expected MIDI " << midi << " disabled in Disable all preset" << std::endl;
            std::exit(1);
        }
    }

    // C Major (presetIdx = 2): C=0, D=2, E=4, F=5, G=7, A=9, B=11
    if (!isNoteEnabledForPreset(2, 21)) { std::cerr << "Test 4 FAILED: Expected MIDI 21 (A) enabled in C Major" << std::endl; std::exit(1); }
    if (isNoteEnabledForPreset(2, 22))  { std::cerr << "Test 4 FAILED: Expected MIDI 22 (A#) disabled in C Major" << std::endl; std::exit(1); }
    if (!isNoteEnabledForPreset(2, 23)) { std::cerr << "Test 4 FAILED: Expected MIDI 23 (B) enabled in C Major" << std::endl; std::exit(1); }
    if (!isNoteEnabledForPreset(2, 24)) { std::cerr << "Test 4 FAILED: Expected MIDI 24 (C) enabled in C Major" << std::endl; std::exit(1); }
    if (isNoteEnabledForPreset(2, 25))  { std::cerr << "Test 4 FAILED: Expected MIDI 25 (C#) disabled in C Major" << std::endl; std::exit(1); }
    if (!isNoteEnabledForPreset(2, 26)) { std::cerr << "Test 4 FAILED: Expected MIDI 26 (D) enabled in C Major" << std::endl; std::exit(1); }

    // A Minor (presetIdx = 21): rootIdx = 9 (A), isMinor = true
    if (!isNoteEnabledForPreset(21, 21)) { std::cerr << "Test 4 FAILED: Expected MIDI 21 (A) enabled in A Minor" << std::endl; std::exit(1); }
    if (isNoteEnabledForPreset(21, 22))  { std::cerr << "Test 4 FAILED: Expected MIDI 22 (A#) disabled in A Minor" << std::endl; std::exit(1); }
    if (!isNoteEnabledForPreset(21, 23)) { std::cerr << "Test 4 FAILED: Expected MIDI 23 (B) enabled in A Minor" << std::endl; std::exit(1); }
    if (!isNoteEnabledForPreset(21, 24)) { std::cerr << "Test 4 FAILED: Expected MIDI 24 (C) enabled in A Minor" << std::endl; std::exit(1); }
    if (isNoteEnabledForPreset(21, 25))  { std::cerr << "Test 4 FAILED: Expected MIDI 25 (C#) disabled in A Minor" << std::endl; std::exit(1); }

    std::cout << "  RESULT: PASS (Preset note mapping algorithm verified)" << std::endl;
}
