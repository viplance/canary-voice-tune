#include "NoteSelector.h"
#include <cmath>

namespace NoteSelector
{
    static constexpr int kLowMidi  = 21;
    static constexpr int kHighMidi = 108;

    int chooseActiveNote(const bool* activeKeys,
                         float pitchMidi,
                         int incumbentMidi,
                         float hysteresisSemitones)
    {
        // 1. Closest enabled key by pure semitone distance — symmetric, no
        //    above/below preference. This alone already implements "snap to the
        //    nearer of the two surrounding enabled notes": the implicit switch
        //    boundary is the midpoint between consecutive enabled keys.
        int   closestMidi = -1;
        float closestDist = 1e9f;
        for (int midi = kLowMidi; midi <= kHighMidi; ++midi) {
            if (! activeKeys[midi - kLowMidi]) continue;
            float d = std::abs(pitchMidi - (float)midi);
            if (d < closestDist) { closestDist = d; closestMidi = midi; }
        }
        if (closestMidi < 0) return -1; // no enabled keys at all

        // 2. Symmetric hysteresis around the incumbent. Only relevant when an
        //    incumbent is set, still enabled, and the raw closest pick differs.
        if (incumbentMidi >= kLowMidi && incumbentMidi <= kHighMidi
            && activeKeys[incumbentMidi - kLowMidi]
            && closestMidi != incumbentMidi) {
            // The natural switch boundary is the midpoint between the incumbent
            // and the challenger. Require the pitch to cross that midpoint by
            // more than the hysteresis half-width before letting the challenger
            // win. Because the test is on |pitch - midpoint| measured toward the
            // challenger, it is the same on either side — higher or lower.
            float midpoint = 0.5f * ((float)incumbentMidi + (float)closestMidi);
            float past     = (closestMidi > incumbentMidi)
                                 ? (pitchMidi - midpoint)   // challenger above
                                 : (midpoint - pitchMidi);  // challenger below
            if (past < hysteresisSemitones) {
                // Not far enough past the midpoint — keep the incumbent.
                return incumbentMidi;
            }
        }

        return closestMidi;
    }
}
