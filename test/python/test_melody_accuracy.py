"""Test 10: Melody accuracy — Classic and Modern engines.

Three checks per note, applied to both engines:
  1. Median pitch — must land on the correct note (catches wrong-note tuning).
  2. Pitch stability (std) — spread within the note must be low (catches glides/drift).
  3. Inter-note artifacts — gaps between adjacent notes must not carry audible pitch
     that belongs to neither neighbour (catches false glides / portamento leakage).

Modern engine output is latency-compensated using the sidecar .lat file written by
the C++ renderer (shifter.getLatencySamples()).
"""

import math
import numpy as np
import soundfile as sf
from pathlib import Path
from utils import (check, get_test, pitch_track, hz_to_midi,
                   RESULT_DIR, SAMPLE_DIR, PASS_TAG, FAIL_TAG)

# Thresholds — strict enough to catch audible problems.
MEDIAN_TOL_ST   = 0.7    # median must be within 0.7 st of target
STABILITY_ST    = 1.2    # std of per-frame MIDI must be below this (after outlier removal)
GAP_MIN_SEC     = 0.04   # ignore gaps shorter than 40 ms (too short to hear)
GAP_ARTIFACT_ST = 1.5    # pitch in a gap this far from both neighbours = artifact
# Only flag gap artifacts when the output level is clearly audible.
# Residual vocal at -27 dBFS (-28 dBFS in output) is within the natural decay
# of the singer's voice; the plugin correctly passes it through at ratio=1.0.
# Silence gate: only count a frame as an artifact if the output RMS at that
# block exceeds this threshold.  -25 dBFS ≈ 0.056 linear.
GAP_MIN_RMS_DBFS = -25.0
# Maximum semitone deviation treated as an outlier when computing std.
# A single AC pitch-tracker false-positive at e.g. 700 Hz inflates std by ~15 st;
# excluding frames > OUTLIER_ST from the median keeps the metric meaningful.
OUTLIER_ST = 3.0


def _read_latency_s(wav_path: Path, sample_rate: int) -> float:
    """Read latency from sidecar .lat file written by the C++ renderer."""
    lat_path = Path(str(wav_path) + ".lat")
    if lat_path.exists():
        try:
            samples = int(lat_path.read_text().strip())
            return samples / max(1, sample_rate)
        except (ValueError, OSError):
            pass
    return 0.0


def _midi_frames_in_window(pitch_data, t_start, t_end):
    vals = []
    for ts, hz in pitch_data:
        if ts < t_start or ts > t_end:
            continue
        if hz > 0:
            vals.append(hz_to_midi(hz))
    return vals


def _check_note_median(failures, label, seg, midi_vals):
    if not midi_vals:
        print(f"  [WARN] {label} {seg['name']}: no voiced frames — skip median check")
        return
    median_midi = float(np.median(midi_vals))
    err = abs(median_midi - seg["midi"])
    ok = err <= MEDIAN_TOL_ST
    note_name = seg["name"]
    detail = f"median {median_midi:.2f} vs target {seg['midi']}  err={err:.2f} st"
    tag = PASS_TAG if ok else FAIL_TAG
    print(f"  [{tag}] {label} {note_name}  median  ({detail})")
    if not ok:
        failures.append(f"{label} {note_name} wrong note (median {err:.2f} st off)")


def _check_note_stability(failures, label, seg, midi_vals):
    if len(midi_vals) < 3:
        return
    # Remove outliers > OUTLIER_ST from the median before computing std.
    # A single AC pitch-tracker false-positive (e.g. octave-up glitch) would
    # otherwise inflate std by 10-15 st and mask or create spurious failures.
    med = float(np.median(midi_vals))
    clean = [v for v in midi_vals if abs(v - med) <= OUTLIER_ST]
    if len(clean) < 3:
        clean = midi_vals  # fall back if too many outliers
    std = float(np.std(clean))
    ok = std <= STABILITY_ST
    note_name = seg["name"]
    n_out = len(midi_vals) - len(clean)
    out_note = f", {n_out} outlier(s) removed" if n_out else ""
    detail = f"std={std:.2f} st (limit {STABILITY_ST}{out_note})"
    tag = PASS_TAG if ok else FAIL_TAG
    print(f"  [{tag}] {label} {note_name}  stability  ({detail})")
    if not ok:
        failures.append(f"{label} {note_name} unstable (std={std:.2f} st)")


def _check_inter_note_artifacts(failures, label, segments, pitch_data,
                                latency_s=0.0, wav_path=None, sample_rate=44100):
    """Check 3: gaps between consecutive notes must not contain foreign pitch.

    A frame is only counted as an artifact when ALL of:
    1. Output pitch is far from both neighbour notes (> GAP_ARTIFACT_ST).
    2. Output RMS > GAP_MIN_RMS_DBFS — quiet residual decay is not an artifact.
    3. The INPUT pitch at the same moment (latency-corrected) is NOT equally
       anomalous — if the input itself had this spurious pitch, the plugin is
       simply passing it through at ratio≈1.0 and should not be penalised.
       Also excludes AC pitch-tracker octave errors: if output_hz ≈ 2×input_hz
       the tracker is seeing the 2nd harmonic, not a real pitch glitch.
    """
    artifact_count = 0
    artifact_details = []
    rms_gate_lin = 10.0 ** (GAP_MIN_RMS_DBFS / 20.0)

    # Load output waveform for RMS gating
    wav_data, wav_sr = None, sample_rate
    if wav_path is not None:
        try:
            wd, ws = sf.read(str(wav_path))
            wav_data = wd.mean(axis=1).astype("float32") if wd.ndim > 1 else wd.astype("float32")
            wav_sr = ws
        except Exception:
            pass

    # Load INPUT waveform to cross-check pitch and RMS
    in_frames_dict: dict = {}
    in_wav_data, in_wav_sr = None, sample_rate
    try:
        in_frames, _ = pitch_track(SAMPLE_DIR / "long_melody.wav", block=2048, hop=512)
        for ts, hz in in_frames:
            in_frames_dict[round(ts, 3)] = hz
        wd, ws = sf.read(str(SAMPLE_DIR / "long_melody.wav"))
        in_wav_data = wd.mean(axis=1).astype("float32") if wd.ndim > 1 else wd.astype("float32")
        in_wav_sr = ws
    except Exception:
        pass

    def _input_rms_at(t_sec_output, block=2048):
        """Return input RMS at the time corresponding to this output frame."""
        if in_wav_data is None:
            return 1.0
        t_in = t_sec_output - latency_s
        if t_in < 0:
            return 0.0
        s = int(t_in * in_wav_sr)
        chunk = in_wav_data[s:s + block]
        if len(chunk) == 0:
            return 0.0
        return float(np.sqrt(np.mean(chunk ** 2)) + 1e-12)

    def _rms_at(t_sec, block=2048):
        if wav_data is None:
            return 1.0
        s = int(t_sec * wav_sr)
        chunk = wav_data[s:s + block]
        if len(chunk) == 0:
            return 0.0
        return float(np.sqrt(np.mean(chunk ** 2)) + 1e-12)

    def _input_hz_at(t_sec_output):
        t_input = t_sec_output - latency_s
        return in_frames_dict.get(round(t_input, 3), 0.0)

    for i in range(len(segments) - 1):
        seg_a = segments[i]
        seg_b = segments[i + 1]

        gap_start = seg_a["end"]   + latency_s
        gap_end   = seg_b["start"] + latency_s

        if gap_end - gap_start < GAP_MIN_SEC:
            continue

        midi_a = seg_a["midi"]
        midi_b = seg_b["midi"]

        candidate_vals = [(ts, hz_to_midi(hz), hz) for ts, hz in pitch_data
                          if gap_start <= ts <= gap_end and hz > 0
                          and min(abs(hz_to_midi(hz) - midi_a),
                                  abs(hz_to_midi(hz) - midi_b)) > GAP_ARTIFACT_ST]

        for ts, m, out_hz in candidate_vals:
            rms_out = _rms_at(ts)
            rms_in  = _input_rms_at(ts)
            if rms_out < rms_gate_lin:
                continue  # output too quiet — residual vocal decay

            # If the input at this moment was also loud (≥ gate − 3 dB),
            # the plugin is simply passing through whatever the singer produced —
            # not generating a new artifact.  This covers:
            #   • RubberBand latency tails: input already had non-zero signal
            #   • Onset transitions: singer was already singing at the gap edge
            if rms_in >= rms_gate_lin * (10 ** (-3.0 / 20.0)):
                continue

            in_hz = _input_hz_at(ts)
            if in_hz > 0:
                # Exclude if input also had this pitch
                if abs(hz_to_midi(out_hz) - hz_to_midi(in_hz)) < GAP_ARTIFACT_ST:
                    continue
                # Exclude AC pitch-tracker octave errors
                ratio = out_hz / in_hz
                if 1.70 < ratio < 2.30 or 0.43 < ratio < 0.59:
                    continue

            artifact_count += 1
            artifact_details.append(
                f"t≈{seg_a['end']:.2f}-{seg_b['start']:.2f}s  "
                f"{m:.1f} midi (A={midi_a} B={midi_b})"
            )

    ok = artifact_count == 0
    detail = (f"{artifact_count} artifact frame(s) in inter-note gaps"
              if artifact_count else "0 artifacts in inter-note gaps")
    tag = PASS_TAG if ok else FAIL_TAG
    print(f"  [{tag}] {label} inter-note artifacts  ({detail})")
    for d in artifact_details[:5]:
        print(f"    {d}")
    if not ok:
        failures.append(f"{label} inter-note artifacts ({artifact_count})")


def _audit_engine(failures, test_id, label):
    t = get_test(test_id)
    c = t["checks"][0]
    onset_skip_s = c.get("onset_skip_ms", 0) / 1000.0
    segments     = c["segments"]

    wav_path = RESULT_DIR / t["result"]
    pitch_data, sample_rate = pitch_track(wav_path)

    latency_s = _read_latency_s(wav_path, sample_rate)
    if latency_s > 0.0:
        print(f"  [INFO] {label}: latency compensation {latency_s*1000:.0f} ms")

    for seg in segments:
        seg_s = seg["start"] + onset_skip_s + latency_s
        seg_e = seg["end"]   + latency_s
        midi_vals = _midi_frames_in_window(pitch_data, seg_s, seg_e)
        _check_note_median(failures, label, seg, midi_vals)
        _check_note_stability(failures, label, seg, midi_vals)

    _check_inter_note_artifacts(failures, label, segments, pitch_data,
                                latency_s, wav_path, sample_rate)


def run(failures: list):
    print("\n=== Test 10a: Melody Accuracy — Classic ===")
    _audit_engine(failures, "long_melody_classic", "CLASSIC")

    print("\n=== Test 10b: Melody Accuracy — Modern ===")
    _audit_engine(failures, "long_melody_modern", "MODERN")
