#pragma once

// Pure, dependency-free selection of the active keyboard note that a detected
// pitch should snap to. Extracted from the plugin processor so the policy can
// be unit-tested directly (the processor itself drags in the full JUCE
// AudioProcessor and cannot be instantiated in the console test runner).
//
// Policy (see chooseActiveNote): pick the ENABLED key whose pitch is
// mathematically closest to the detected pitch, with NO bias toward the note
// being above or below. When several disabled keys sit between two enabled
// ones, the switch happens exactly at the midpoint between those two enabled
// keys — e.g. with only C and E enabled, a pitch at C# maps to C and a pitch
// at D# maps to E, because each is the closer of the two in semitone distance.
//
// To stop a pitch that merely jitters around that midpoint from flickering
// between the two notes, a SYMMETRIC hysteresis band is applied around the
// incumbent: the previously chosen note is retained until the pitch crosses
// the midpoint by more than `hysteresisSemitones`. The band is symmetric, so
// it does not favour higher or lower notes — it only adds stickiness.

namespace NoteSelector
{
    // activeKeys: 88-entry bool array indexed [midi - 21], midi in [21,108]
    //             (standard piano range). true = key enabled.
    // pitchMidi:  the detected pitch expressed in (possibly fractional) MIDI
    //             semitones (69 + 12*log2(hz/440)).
    // incumbentMidi: the currently locked/displayed MIDI note, or -1 if none.
    // hysteresisSemitones: half-width of the symmetric stick band at the
    //             midpoint between two enabled notes (0 disables hysteresis).
    //
    // Returns the chosen enabled MIDI note, or -1 if no key is enabled.
    int chooseActiveNote(const bool* activeKeys,
                         float pitchMidi,
                         int incumbentMidi,
                         float hysteresisSemitones);
}
