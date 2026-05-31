#!/usr/bin/env python3
"""
Benchmark three pitch detection algorithms on vocal samples.

Algorithms:
  1. YIN   — Cumulative Mean Normalized Difference Function (de Cheveigné 2002)
  2. AMDF  — Average Magnitude Difference Function with LPF pre-filter
  3. MPM   — McLeod Pitch Method (normalized autocorrelation, Pitchy-style)

For each algorithm, runs on test/samples/long_melody.wav and reports:
  - Octave jump count (at block=8192 to filter OLA artifacts)
  - Segment accuracy on 19 note windows
"""

import sys, math, json
import numpy as np
import soundfile as sf
from pathlib import Path

sys.path.insert(0, str(Path(__file__).parent))
from utils import hz_to_midi

SAMPLE_DIR = Path(__file__).parent.parent / "samples"
RESULT_DIR = Path(__file__).parent.parent / "result"


# ---------------------------------------------------------------------------
# Shared helpers
# ---------------------------------------------------------------------------

def load_mono(path):
    data, sr = sf.read(str(path))
    if data.ndim > 1:
        data = data.mean(axis=1)
    return data.astype(np.float32), sr


def lpf_biquad(data: np.ndarray, sr: int, cutoff: float) -> np.ndarray:
    """Simple 2nd-order Butterworth LPF."""
    from scipy.signal import butter, lfilter
    b, a = butter(2, cutoff / (sr / 2), btype='low')
    return lfilter(b, a, data).astype(np.float32)


def count_jumps(pitch_seq, gap_max_s=0.20, threshold_st=8.0):
    """Count octave jumps in a (time, hz) list."""
    voiced = [(t, h) for t, h in pitch_seq if h > 0]
    count = 0
    for i in range(1, len(voiced)):
        t0, h0 = voiced[i-1]; t1, h1 = voiced[i]
        if t1 - t0 > gap_max_s: continue
        if abs(hz_to_midi(h1) - hz_to_midi(h0)) >= threshold_st:
            count += 1
    return count


NOTE_SEGMENTS = [
    (0.15,  1.30, 59, "B3 (1)"),   (1.34,  1.46, 61, "C#4(2)"),
    (1.47,  1.79, 63, "D#4(3)"),   (1.86,  2.35, 61, "C#4(4)"),
    (2.58,  2.76, 61, "C#4(5)"),   (2.84,  3.24, 59, "B3 (6)"),
    (3.24,  3.62, 58, "A#3(7)"),   (3.80,  4.40, 59, "B3 (8)"),
    (4.92,  5.15, 61, "C#4(9)"),   (5.27,  5.52, 63, "D#4(10)"),
    (5.72,  6.05, 61, "C#4(11)"),  (6.24,  6.43, 63, "D#4(12)"),
    (6.93,  7.15, 59, "B3 (13)"),  (7.15,  7.33, 58, "A#3(14)"),
    (7.40,  7.62, 59, "B3 (15)"),  (8.76,  9.00, 61, "C#4(16)"),
    (9.14,  9.26, 63, "D#4(17)"),  (9.50,  9.93, 61, "C#4(18)"),
    (10.00, 10.50, 61, "C#4(19)"),
]
ONSET_SKIP = 0.08   # seconds
TOL_ST     = 1.0


def segment_accuracy(pitch_seq, segments=NOTE_SEGMENTS,
                     onset_skip=ONSET_SKIP, tol=TOL_ST):
    """Returns (ok_segs, total_segs, ok_frames, total_frames)."""
    ok_segs = total_segs = ok_frames = total_frames = 0
    for ws, we, midi, name in segments:
        ws += onset_skip
        if ws >= we: continue
        total_segs += 1
        frames = [(t, h) for t, h in pitch_seq if ws <= t <= we and h > 0]
        if not frames:
            continue
        ok = sum(1 for _, h in frames if abs(hz_to_midi(h) - midi) <= tol)
        ok_frames += ok; total_frames += len(frames)
        if ok / len(frames) >= 0.60:
            ok_segs += 1
    return ok_segs, total_segs, ok_frames, total_frames


# ---------------------------------------------------------------------------
# Algorithm 1: YIN
# ---------------------------------------------------------------------------

def yin_pitch(data: np.ndarray, sr: int, block=2048, tolerance=0.15,
              fmin=60.0, fmax=1200.0):
    half = block // 2
    lag_min = max(1, int(sr / fmax))
    lag_max = min(half - 1, int(sr / fmin))
    results = []
    for start in range(0, len(data) - block, block // 2):
        x = data[start:start + block]
        # Difference function
        d = np.array([
            np.sum((x[:half] - x[lag:lag + half])**2)
            for lag in range(lag_max + 1)
        ], dtype=np.float32)
        d[0] = 1.0
        # CMNDF
        running = 0.0
        for tau in range(1, lag_max + 1):
            running += d[tau]
            d[tau] = d[tau] * tau / running if running > 0 else 1.0
        # Threshold search
        tau_est = -1
        for tau in range(lag_min, lag_max):
            if d[tau] < tolerance:
                while tau + 1 < lag_max and d[tau + 1] < d[tau]:
                    tau += 1
                tau_est = tau
                break
        if tau_est < 0:
            tau_est = int(np.argmin(d[lag_min:lag_max])) + lag_min
            if d[tau_est] > 0.45:
                results.append((start / sr, 0.0))
                continue
        hz = sr / tau_est if tau_est > 0 else 0.0
        results.append((start / sr, hz if fmin <= hz <= fmax else 0.0))
    return results


# ---------------------------------------------------------------------------
# Algorithm 2: AMDF with LPF
# ---------------------------------------------------------------------------

def amdf_pitch(data: np.ndarray, sr: int, block=2048, lpf_cutoff=600.0,
               fmin=60.0, fmax=900.0, clarity_thresh=0.55):
    filtered = lpf_biquad(data, sr, lpf_cutoff)
    half = block // 2
    lag_min = max(1, int(sr / fmax))
    lag_max = min(half - 1, int(sr / fmin))
    results = []
    for start in range(0, len(filtered) - block, block // 2):
        x = filtered[start:start + block]
        amdf = np.array([
            np.mean(np.abs(x[:half] - x[lag:lag + half]))
            for lag in range(lag_max + 1)
        ], dtype=np.float32)
        d_mean = amdf[lag_min:lag_max + 1].mean()
        tau_est = int(np.argmin(amdf[lag_min:lag_max])) + lag_min
        clarity = amdf[tau_est] / d_mean if d_mean > 1e-9 else 1.0
        if clarity > clarity_thresh:
            results.append((start / sr, 0.0))
            continue
        hz = sr / tau_est if tau_est > 0 else 0.0
        results.append((start / sr, hz if fmin <= hz <= fmax else 0.0))
    return results


# ---------------------------------------------------------------------------
# Algorithm 3: MPM (normalised autocorrelation via FFT — Pitchy-style)
# ---------------------------------------------------------------------------

def mpm_pitch(data: np.ndarray, sr: int, block=2048,
              fmin=60.0, fmax=1200.0, k=0.85, clarity_thresh=0.45):
    lag_min = max(1, int(sr / fmax))
    lag_max = min(block - 1, int(sr / fmin))
    results = []
    for start in range(0, len(data) - block, block // 2):
        x = data[start:start + block]
        # NSDF via FFT (efficient autocorrelation)
        n2 = 2 * block
        X = np.fft.rfft(x, n=n2)
        nsdf_raw = np.fft.irfft(X * np.conj(X))[:block].real
        # Normalise: divide by (sum_x² + sum_x[tau:]²) at each lag
        xx = np.cumsum(x[::-1]**2)[::-1]   # suffix energy
        xx_lag = np.concatenate([xx[1:], [0]])
        denom = xx + xx_lag
        with np.errstate(divide='ignore', invalid='ignore'):
            nsdf = np.where(denom > 1e-12, 2.0 * nsdf_raw / denom, 0.0)

        # Peak picking: first peak >= k * global_max in [lag_min, lag_max]
        n_max = nsdf[lag_min:lag_max].max()
        if n_max < clarity_thresh:
            results.append((start / sr, 0.0))
            continue
        threshold = k * n_max
        tau_est = -1
        for tau in range(lag_min, lag_max - 1):
            if nsdf[tau] >= threshold and nsdf[tau] >= nsdf[tau + 1]:
                tau_est = tau
                break
        if tau_est < 0:
            results.append((start / sr, 0.0))
            continue
        hz = sr / tau_est
        results.append((start / sr, hz if fmin <= hz <= fmax else 0.0))
    return results


# ---------------------------------------------------------------------------
# Run benchmark
# ---------------------------------------------------------------------------

def run():
    # ---- 1. Detector accuracy on INPUT signal --------------------------------
    wav_input = SAMPLE_DIR / "long_melody.wav"
    data_in, sr_in = load_mono(wav_input)

    print(f"=== Detector accuracy on INPUT: {wav_input.name} ({len(data_in)/sr_in:.1f}s @ {sr_in}Hz) ===")
    print(f"{'Algorithm':<8}  {'Segs OK':>8}  {'Frames OK':>12}  {'Notes accuracy'}")
    print("-" * 60)

    algos = [
        ("YIN",  lambda d, s: yin_pitch(d, s)),
        ("AMDF", lambda d, s: amdf_pitch(d, s)),
        ("MPM",  lambda d, s: mpm_pitch(d, s)),
    ]

    for name, fn in algos:
        pitch_seq = fn(data_in, sr_in)
        ok_segs, total_segs, ok_frames, total_frames = segment_accuracy(pitch_seq)
        pct = int(100 * ok_frames / max(1, total_frames))
        seg_details = []
        for ws, we, midi, label in NOTE_SEGMENTS:
            ws2 = ws + ONSET_SKIP
            if ws2 >= we: continue
            frames = [(t, h) for t, h in pitch_seq if ws2 <= t <= we and h > 0]
            if not frames: continue
            ok = sum(1 for _, h in frames if abs(hz_to_midi(h) - midi) <= TOL_ST)
            if ok / len(frames) < 0.60:
                seg_details.append(label.strip())
        failed_str = ", ".join(seg_details) if seg_details else "none"
        print(f"{name:<8}  {ok_segs}/{total_segs:>2}       {ok_frames}/{total_frames} ({pct}%)    failed: {failed_str}")

    # ---- 2. Octave jump count on OUTPUT WAV (AMDF detector used for plugin) --
    wav_out = RESULT_DIR / "long_melody_classic.wav"
    if wav_out.exists():
        data_out, sr_out = load_mono(wav_out)
        print(f"\n=== Octave jumps in OUTPUT: {wav_out.name} (block=8192) ===")
        print(f"{'Algorithm':<8}  {'Jumps ≥8st':>11}")
        print("-" * 22)
        for name, fn in algos:
            pitch_8192 = []
            block8 = min(8192, len(data_out) // 4)
            for start in range(0, len(data_out) - block8, block8 // 2):
                chunk = data_out[start:start + block8]
                hz = _detect_single(name, chunk, sr_out)
                pitch_8192.append((start / sr_out, hz))
            jumps = count_jumps(pitch_8192)
            print(f"{name:<8}  {jumps:>11}")
    print()


def _detect_single(name, chunk, sr):
    """Detect a single pitch from one chunk for jump analysis."""
    if name == "YIN":
        r = yin_pitch(chunk, sr, block=len(chunk)); return r[0][1] if r else 0
    if name == "AMDF":
        r = amdf_pitch(chunk, sr, block=len(chunk)); return r[0][1] if r else 0
    if name == "MPM":
        r = mpm_pitch(chunk, sr, block=len(chunk)); return r[0][1] if r else 0
    return 0


if __name__ == "__main__":
    run()
