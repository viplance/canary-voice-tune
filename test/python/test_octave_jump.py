"""Test: octave_jump — per-segment pitch accuracy on jump_notes_out.wav.

The C++ renderer (Test_OctaveJump.cpp) renders with only Eb3/F3/F#3/Ab3
active so NoteSelector locks stably instead of chasing every semitone.
This Python stage reads the rendered WAV and verifies pitch accuracy per
segment using independent autocorrelation, mirroring the C++ median check.
"""

import statistics
from utils import check, get_test, pitch_track, hz_to_midi, RESULT_DIR, PASS_TAG, FAIL_TAG


def run(failures: list):
    print("\n=== Test 6: OctaveJump — per-segment pitch accuracy ===")

    t = get_test("octave_jump")
    c = t["checks"][0]   # type: pitch_segments
    tol          = c["tolerance_st"]
    segments     = c["segments"]
    onset_skip_s = c.get("onset_skip_ms", 0) / 1000.0

    pitch_data, _ = pitch_track(RESULT_DIR / t["result"])

    all_ok = True
    for seg in segments:
        seg_s = seg["start"] + onset_skip_s
        seg_e = seg["end"]
        target_midi = seg["midi"]
        name = seg["name"]

        midi_vals = []
        for ts, hz in pitch_data:
            if ts < seg_s or ts > seg_e or hz <= 0:
                continue
            midi_vals.append(hz_to_midi(hz))

        if not midi_vals:
            print(f"  [WARN] {name}: no voiced frames in [{seg_s:.2f}–{seg_e:.2f}s]")
            continue

        median = statistics.median(midi_vals)
        err = abs(median - target_midi)
        seg_ok = err <= tol

        tag = PASS_TAG if seg_ok else FAIL_TAG
        print(f"  [{tag}] {name}  median={median:.2f} target={target_midi}  err={err:.2f} st")
        if not seg_ok:
            failures.append(f"T6 {name}")
            all_ok = False

    if all_ok:
        print("  RESULT: PASS (all segments within ±1 st of target note)")
    else:
        print("  RESULT: FAIL")
