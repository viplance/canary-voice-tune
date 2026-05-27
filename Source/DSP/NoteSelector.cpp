#include "NoteSelector.h"
#include <cmath>
#include <algorithm>

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
            // Hysteresis must only hold the incumbent across its OWN boundary —
            // i.e. when the incumbent and the challenger are the two enabled
            // notes bracketing the pitch, with nothing enabled between them. If
            // the pitch has already moved past an intermediate enabled note (so
            // the challenger is no longer the incumbent's immediate neighbour),
            // the incumbent must release immediately; otherwise the stick-band
            // would drag the choice back across a note and cause the jumps the
            // user saw at high Range.
            bool adjacent = true;
            int lo = std::min(incumbentMidi, closestMidi);
            int hi = std::max(incumbentMidi, closestMidi);
            for (int m = lo + 1; m < hi; ++m) {
                if (activeKeys[m - kLowMidi]) { adjacent = false; break; }
            }

            if (adjacent) {
                // The natural switch boundary is the midpoint between the
                // incumbent and the challenger. Require the pitch to cross that
                // midpoint by more than the hysteresis half-width before the
                // challenger wins. The test is symmetric — same on either side.
                //
                // Clamp the effective band so it can never reach the challenger
                // note itself: the half-distance to the midpoint is gap/2, so a
                // band >= gap/2 would let the incumbent stick even when the
                // pitch sits exactly ON the challenger. Capping at 0.45*gap
                // keeps a margin (for adjacent semitones, gap=1 -> max 0.45).
                float gap      = (float)(hi - lo);
                float band     = std::min(hysteresisSemitones, 0.45f * gap);
                float midpoint = 0.5f * ((float)incumbentMidi + (float)closestMidi);
                float past     = (closestMidi > incumbentMidi)
                                     ? (pitchMidi - midpoint)   // challenger above
                                     : (midpoint - pitchMidi);  // challenger below
                if (past < band) {
                    // Not far enough past the midpoint — keep the incumbent.
                    return incumbentMidi;
                }
            }
        }

        return closestMidi;
    }
}
