"""Test: octave_jump — D3 region in jump_notes_out.wav must not jump octaves."""

from utils import check, get_test, pitch_track, hz_to_midi, RESULT_DIR


def run(failures: list):
    print("\n=== Test 6: OctaveJump ===")

    t = get_test("octave_jump")
    c = t["checks"][0]

    region = c["region_sec"]
    target = c["target_midi"]
    tol    = c["tolerance_st"]

    pitch_data, _ = pitch_track(RESULT_DIR / t["result"])

    errors = 0
    for ts, hz in pitch_data:
        if hz <= 0 or ts < region[0] or ts > region[1]:
            continue
        if abs(hz_to_midi(hz) - target) > tol:
            errors += 1

    check(failures, "T6 D3 note stays in correct octave",
          errors == 0,
          f"{errors} off-pitch frame(s) in [{region[0]}-{region[1]}s], "
          f"target MIDI {target} ±{tol} st")
