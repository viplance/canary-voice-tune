#include "Test_NoteSelector.h"
#include "../Source/DSP/NoteSelector.h"
#include <iostream>
#include <cmath>
#include <cstdlib>

namespace {

constexpr int kLowMidi = 21;

// Build an 88-key mask from a list of enabled MIDI notes.
struct KeyMask {
    bool keys[88] = { false };
    KeyMask(std::initializer_list<int> enabled) {
        for (int m : enabled) if (m >= 21 && m <= 108) keys[m - kLowMidi] = true;
    }
    const bool* data() const { return keys; }
};

int failures = 0;

void expectNote(const char* what, int got, int want) {
    if (got != want) {
        std::cerr << "  FAIL: " << what << " -> got MIDI " << got
                  << ", expected " << want << std::endl;
        failures++;
    } else {
        std::cout << "  ok: " << what << " -> MIDI " << got << std::endl;
    }
}

float midiOf(int note, float centsOffsetInSemitones) {
    return (float)note + centsOffsetInSemitones;
}

} // namespace

void runNoteSelectorTests()
{
    std::cout << "\nTest 8: Verifying active-note selection (closest-in-pitch, symmetric)" << std::endl;

    // Scale with only C (60) and E (64) enabled; D (62) is the midpoint.
    KeyMask ce({60, 64});
    const float hyst = 0.35f; // representative symmetric stick-band

    // --- Core requirement: choose the mathematically closer of the two -------
    // A pitch around C# (61) is closer to C than to E -> C.
    expectNote("C# pitch, {C,E}, no incumbent",
               NoteSelector::chooseActiveNote(ce.data(), midiOf(61, 0.0f), -1, hyst), 60);
    // A pitch around D# (63) is closer to E than to C -> E.
    expectNote("D# pitch, {C,E}, no incumbent",
               NoteSelector::chooseActiveNote(ce.data(), midiOf(63, 0.0f), -1, hyst), 64);
    // Just below the midpoint D (61.9) -> C; just above (62.1) -> E.
    expectNote("61.9 (below midpoint), no incumbent",
               NoteSelector::chooseActiveNote(ce.data(), 61.9f, -1, hyst), 60);
    expectNote("62.1 (above midpoint), no incumbent",
               NoteSelector::chooseActiveNote(ce.data(), 62.1f, -1, hyst), 64);

    // --- Symmetry: result must not depend on which note is the incumbent -----
    // With NO hysteresis, incumbent must not bias a clearly-closer pick.
    expectNote("D# pitch, incumbent C, hyst=0 (E clearly closer)",
               NoteSelector::chooseActiveNote(ce.data(), midiOf(63, 0.0f), 60, 0.0f), 64);
    expectNote("C# pitch, incumbent E, hyst=0 (C clearly closer)",
               NoteSelector::chooseActiveNote(ce.data(), midiOf(61, 0.0f), 64, 0.0f), 60);

    // --- Anti-flicker: jitter around the midpoint keeps the incumbent --------
    // Incumbent C, pitch wobbles just past the midpoint by less than `hyst`:
    // must HOLD C (no flicker to E).
    expectNote("62.2 jitter, incumbent C (within hyst) holds C",
               NoteSelector::chooseActiveNote(ce.data(), 62.2f, 60, hyst), 60);
    // Symmetric: incumbent E, pitch dips just below midpoint by < hyst: HOLD E.
    expectNote("61.8 jitter, incumbent E (within hyst) holds E",
               NoteSelector::chooseActiveNote(ce.data(), 61.8f, 64, hyst), 64);

    // --- Real move past the band switches -----------------------------------
    // Incumbent C, pitch goes well past midpoint (62 + hyst + margin): -> E.
    expectNote("63.0, incumbent C (past band) switches to E",
               NoteSelector::chooseActiveNote(ce.data(), 63.0f, 60, hyst), 64);
    expectNote("61.0, incumbent E (past band) switches to C",
               NoteSelector::chooseActiveNote(ce.data(), 61.0f, 64, hyst), 60);

    // --- Wide disabled gap on one side only ---------------------------------
    // Only C (60) and G (67) enabled; midpoint is 63.5. A D#/E-ish pitch (63)
    // is still closer to C; an F (65) is closer to G. Confirms there is no
    // fixed ±1 window — the boundary tracks the actual spacing.
    KeyMask cg({60, 67});
    expectNote("63 pitch, {C,G} -> C (closer)",
               NoteSelector::chooseActiveNote(cg.data(), 63.0f, -1, hyst), 60);
    expectNote("65 pitch, {C,G} -> G (closer)",
               NoteSelector::chooseActiveNote(cg.data(), 65.0f, -1, hyst), 67);

    // --- Disabled incumbent / no keys edge cases ----------------------------
    // Incumbent points at a now-disabled key: ignored, pick closest enabled.
    expectNote("incumbent disabled, pick closest",
               NoteSelector::chooseActiveNote(ce.data(), midiOf(61, 0.0f), 62, hyst), 60);
    KeyMask none({});
    expectNote("no enabled keys -> -1",
               NoteSelector::chooseActiveNote(none.data(), 62.0f, -1, hyst), -1);

    // --- Chromatic regression (the Range=100 bug) ---------------------------
    // Every key enabled. A pitch sitting exactly ON a note must select THAT
    // note, even with a large hysteresis — the incumbent must never stick a
    // whole semitone away. (Old code with hyst=0.6 returned C for a C# pitch.)
    KeyMask chrom({});
    for (int m = 21; m <= 108; ++m) chrom.keys[m - kLowMidi] = true;
    const float bigHyst = 0.6f; // the value the old Range=100 produced
    expectNote("chromatic: C# pitch on the note, incumbent C, big hyst -> C#",
               NoteSelector::chooseActiveNote(chrom.data(), 61.0f, 60, bigHyst), 61);
    expectNote("chromatic: B pitch on the note, incumbent C, big hyst -> B",
               NoteSelector::chooseActiveNote(chrom.data(), 59.0f, 60, bigHyst), 59);

    // Pitch that has skipped PAST the incumbent's neighbour must release the
    // incumbent immediately (no drag-back across an intermediate note).
    expectNote("chromatic: pitch at D, incumbent C, big hyst -> D (not C)",
               NoteSelector::chooseActiveNote(chrom.data(), 62.0f, 60, bigHyst), 62);

    // Within a single semitone step, the small stick-band still suppresses
    // flicker right at the midpoint but releases just past it.
    expectNote("chromatic: 60.6 incumbent C (within 0.15 band) holds C",
               NoteSelector::chooseActiveNote(chrom.data(), 60.6f, 60, 0.15f), 60);
    expectNote("chromatic: 60.7 incumbent C (past 0.15 band) -> C#",
               NoteSelector::chooseActiveNote(chrom.data(), 60.7f, 60, 0.15f), 61);

    if (failures == 0) {
        std::cout << "  RESULT: PASS (Closest-in-pitch selection is symmetric and flicker-free)" << std::endl;
    } else {
        std::cerr << "  RESULT: FAIL (" << failures << " selection assertions failed)" << std::endl;
        std::exit(1);
    }
}
