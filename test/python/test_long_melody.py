"""Tests: long_melody_classic, long_melody_modern — all 19 segments on-pitch."""

from utils import check, get_test, pitch_track, hz_to_midi, RESULT_DIR, PASS_TAG, FAIL_TAG


def _audit(failures: list, test_id: str, label: str):
    t = get_test(test_id)
    c = t["checks"][0]          # type: pitch_segments
    tol      = c["tolerance_st"]
    segments = c["segments"]

    pitch_data, _ = pitch_track(RESULT_DIR / t["result"])

    for seg in segments:
        seg_s, seg_e = seg["start"], seg["end"]
        target_midi  = seg["midi"]
        name         = seg["name"]

        voiced, ok_frames, bad = 0, 0, []
        for ts, hz in pitch_data:
            if ts < seg_s or ts > seg_e or hz <= 0:
                continue
            voiced += 1
            err = abs(hz_to_midi(hz) - target_midi)
            if err <= tol:
                ok_frames += 1
            else:
                bad.append((ts, hz, err))

        if voiced == 0:
            print(f"  [WARN] {label} {name}: no voiced frames in [{seg_s}-{seg_e}s]")
            continue

        seg_ok = (len(bad) == 0)
        if seg_ok:
            detail = f"{ok_frames}/{voiced} frames on-pitch"
        else:
            worst  = max(bad, key=lambda x: x[2])
            detail = (f"{len(bad)}/{voiced} frames off, "
                      f"worst {worst[2]:.2f} st = {worst[1]:.1f} Hz at {worst[0]:.2f}s")

        tag = PASS_TAG if seg_ok else FAIL_TAG
        print(f"  [{tag}] {label} {name}  {detail}")
        if not seg_ok:
            failures.append(f"{label} {name}")


def run(failures: list):
    print("\n=== Test 9: Long Melody Classic ===")
    _audit(failures, "long_melody_classic", "CLASSIC")

    print("\n=== Test 9b: Long Melody Modern ===")
    _audit(failures, "long_melody_modern", "MODERN")
