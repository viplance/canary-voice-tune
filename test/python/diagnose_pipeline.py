"""
Full pipeline diagnosis: detector → selector → ratio → pitch output.
Produces a per-note diagnostic table covering all 3 stages.

Usage:
    python3 test/python/diagnose_pipeline.py
"""

import math
import numpy as np
import soundfile as sf
from pathlib import Path
from utils import RESULT_DIR, SAMPLE_DIR, hz_to_midi, load_mono, pitch_track

# ── test parameters ──────────────────────────────────────────────────────────
SAMPLE_RATE  = 44100
BLOCK        = 256
ONSET_SKIP_S = 0.080

SEGMENTS = [
    {"start": 0.15,  "end": 1.22,  "midi": 59, "name": "B3  ( 1)"},
    {"start": 1.34,  "end": 1.45,  "midi": 61, "name": "C#4 ( 2)"},
    {"start": 1.60,  "end": 1.72,  "midi": 63, "name": "D#4 ( 3)"},
    {"start": 1.86,  "end": 2.28,  "midi": 61, "name": "C#4 ( 4)"},
    {"start": 2.58,  "end": 2.76,  "midi": 61, "name": "C#4 ( 5)"},
    {"start": 2.84,  "end": 3.24,  "midi": 59, "name": "B3  ( 6)"},
    {"start": 3.24,  "end": 3.62,  "midi": 58, "name": "A#3 ( 7)"},
    {"start": 3.80,  "end": 4.40,  "midi": 59, "name": "B3  ( 8)"},
    {"start": 5.03,  "end": 5.16,  "midi": 61, "name": "C#4 ( 9)"},
    {"start": 5.27,  "end": 5.52,  "midi": 63, "name": "D#4 (10)"},
    {"start": 5.72,  "end": 6.05,  "midi": 61, "name": "C#4 (11)"},
    {"start": 6.24,  "end": 6.43,  "midi": 63, "name": "D#4 (12)"},
    {"start": 6.93,  "end": 7.15,  "midi": 59, "name": "B3  (13)"},
    {"start": 7.20,  "end": 7.33,  "midi": 58, "name": "A#3 (14)"},
    {"start": 7.42,  "end": 7.48,  "midi": 59, "name": "B3  (15)"},
    {"start": 8.76,  "end": 9.00,  "midi": 61, "name": "C#4 (16)"},
    {"start": 9.14,  "end": 9.26,  "midi": 63, "name": "D#4 (17)"},
    {"start": 9.62,  "end": 9.75,  "midi": 61, "name": "C#4 (18)"},
    {"start":10.00,  "end":10.19,  "midi": 61, "name": "C#4 (19)"},
]

# ── Python AMDF detector (mirrors C++ AMDF) ──────────────────────────────────
FMIN, FMAX = 60.0, 900.0
CLARITY_THRESH = 0.55
LPF_CUTOFF = 600.0
WIN = 2048

def _make_biquad_lpf(sr, fc):
    """1st-order IIR approximation of a low-pass biquad (2 cascaded = 4th order)."""
    wc = 2.0 * math.pi * fc / sr
    alpha = math.exp(-wc)
    b0 = 1.0 - alpha
    return b0, alpha  # y[n] = b0*x[n] + alpha*y[n-1]

def _apply_lpf(x, b0, alpha):
    y = np.zeros_like(x)
    state = 0.0
    for i in range(len(x)):
        state = b0 * x[i] + alpha * state
        y[i] = state
    return y

def amdf_pitch(buf_lpf, sr):
    """AMDF on LPF buffer, returns (hz, clarity)."""
    N = len(buf_lpf)
    half = N // 2
    lag_min = max(1, int(sr / FMAX))
    lag_max = min(half - 1, int(sr / FMIN))
    if lag_min >= lag_max:
        return 0.0, 1.0

    d_vals = np.array([
        np.mean(np.abs(buf_lpf[:half] - buf_lpf[tau:half+tau]))
        for tau in range(lag_min, lag_max + 1)
    ])
    d_min  = d_vals.min()
    d_mean = d_vals.mean()
    clarity = (d_min / d_mean) if d_mean > 1e-9 else 1.0
    if clarity > CLARITY_THRESH:
        return 0.0, clarity

    tau_rel = int(np.argmin(d_vals))
    tau_min = lag_min + tau_rel

    # subharmonic check (octave-up guard)
    tau_dbl = tau_min * 2
    if tau_dbl <= lag_max:
        d_dbl = np.mean(np.abs(buf_lpf[:half] - buf_lpf[tau_dbl:half+tau_dbl]))
        if d_dbl < d_min * 0.90:
            tau_min = tau_dbl

    hz = sr / tau_min
    if FMIN <= hz <= FMAX:
        return hz, clarity
    return 0.0, clarity


def run_detector_on_file(wav_path):
    """Run block-by-block AMDF detector on file, return list of (t, hz, clarity)."""
    data, sr = load_mono(wav_path)
    sr = float(sr)

    b0, alpha = _make_biquad_lpf(sr, LPF_CUTOFF)

    # Pre-filter entire file (cascade twice = 4th order)
    lpf1 = _apply_lpf(data, b0, alpha)
    lpf2 = _apply_lpf(lpf1, b0, alpha)

    circ = np.zeros(WIN)
    write_idx = 0

    results = []
    total_samples = len(data)
    MEDIAN_HIST = 5
    med_hist = [0.0] * MEDIAN_HIST
    med_i = 0
    med_filled = 0
    slow_anchor = 0.0
    out_of_range_count = 0
    last_out_of_range_pitch = 0.0
    last_valid = 0.0
    hold = 0
    HOLD_FRAMES = 8
    voiced_since_onset = 0
    ONSET_GUARD = 2
    silent_blocks = 0
    FLUSH_BLOCKS = 8
    last_known_good = 0.0

    def fold(hz):
        if slow_anchor <= 0:
            return hz
        r = hz / slow_anchor
        if   1.60 < r < 2.50: return hz * 0.5
        elif 0.40 < r < 0.63: return hz * 2.0
        elif 2.50 < r < 3.50: return hz / 3.0
        elif 0.29 < r < 0.40: return hz * 3.0
        elif 3.50 < r < 4.60: return hz / 4.0
        elif 0.22 < r < 0.29: return hz * 4.0
        return hz

    for start in range(0, total_samples - BLOCK, BLOCK):
        block_lpf = lpf2[start:start + BLOCK]
        block_raw = data[start:start + BLOCK]
        rms = math.sqrt(float(np.mean(block_raw ** 2)) + 1e-12)

        # fill circular buffer
        for s in block_lpf:
            circ[write_idx] = s
            write_idx = (write_idx + 1) % WIN

        raw_hz = 0.0
        in_onset = False
        if rms > 0.01:
            voiced_since_onset += 1
            if voiced_since_onset > ONSET_GUARD:
                # build linear read buf
                idx = write_idx
                buf = np.roll(circ, -idx)  # oldest first
                raw_hz, clarity = amdf_pitch(buf, sr)
            else:
                in_onset = True
        else:
            voiced_since_onset = 0

        t = start / sr

        if in_onset:
            results.append((t, last_valid, 'ONSET'))
            continue

        if raw_hz > 0:
            silent_blocks = 0
            nonlocal_slow = slow_anchor  # capture for fold

            folded = fold(raw_hz)
            med_hist[med_i] = folded
            med_i = (med_i + 1) % MEDIAN_HIST
            if med_filled < MEDIAN_HIST:
                med_filled += 1
            pitch_est = sorted(med_hist[:med_filled])[med_filled // 2] if med_filled >= MEDIAN_HIST else folded

            if slow_anchor <= 0:
                if last_known_good > 0:
                    slow_anchor = last_known_good
                    pitch_est = fold(pitch_est)
                    slow_anchor = pitch_est
                    med_hist = [pitch_est] * MEDIAN_HIST
                    med_filled = MEDIAN_HIST
                else:
                    slow_anchor = pitch_est
                out_of_range_count = 0
                last_out_of_range_pitch = 0.0
            else:
                r = folded / slow_anchor
                if r < 0.749 or r > 1.334:
                    is_stable = False
                    if last_out_of_range_pitch > 0.0:
                        r_diff = folded / last_out_of_range_pitch
                        if 0.917 < r_diff < 1.091:
                            is_stable = True

                    if is_stable:
                        out_of_range_count += 1
                        if out_of_range_count >= 5:
                            slow_anchor = raw_hz
                            pitch_est = fold(raw_hz)
                            out_of_range_count = 0
                            last_out_of_range_pitch = 0.0
                            med_hist = [pitch_est] * MEDIAN_HIST
                            med_filled = MEDIAN_HIST
                    else:
                        out_of_range_count = 1
                        last_out_of_range_pitch = folded
                else:
                    out_of_range_count = 0
                    last_out_of_range_pitch = 0.0
                    r_est = pitch_est / slow_anchor
                    if 0.749 < r_est < 1.334:
                        slow_anchor = slow_anchor * 0.70 + pitch_est * 0.30

            last_known_good = pitch_est
            r2 = last_valid / pitch_est if last_valid > 0 else 0
            if 0.85 < (pitch_est / last_valid if last_valid > 0 else 0) < 1.18:
                last_valid = last_valid * 0.7 + pitch_est * 0.3
            else:
                last_valid = pitch_est
            hold = 0
            results.append((t, last_valid, 'VOICED'))
        else:
            silent_blocks += 1
            if silent_blocks >= 2:
                med_hist = [0.0] * MEDIAN_HIST
                med_i = 0
                med_filled = 0
            if silent_blocks >= FLUSH_BLOCKS:
                slow_anchor = 0.0
                out_of_range_count = 0
                last_out_of_range_pitch = 0.0
            if hold < HOLD_FRAMES and last_valid > 0:
                hold += 1
                results.append((t, last_valid, 'HOLD'))
            else:
                last_valid = 0.0
                results.append((t, 0.0, 'SILENT'))

    return results


# ── Python note selector (mirrors C++ NoteSelector) ───────────────────────────
def choose_note(active_keys, pitch_midi, incumbent, hysteresis_st=0.1):
    closest = -1
    closest_dist = 1e9
    for midi in range(21, 109):
        if not active_keys[midi - 21]:
            continue
        d = abs(pitch_midi - midi)
        if d < closest_dist:
            closest_dist = d
            closest = midi
    if closest < 0:
        return -1
    if (incumbent >= 21 and incumbent <= 108
            and active_keys[incumbent - 21]
            and closest != incumbent):
        lo, hi = min(incumbent, closest), max(incumbent, closest)
        adjacent = all(not active_keys[m - 21] for m in range(lo+1, hi))
        if adjacent:
            gap  = float(hi - lo)
            band = min(hysteresis_st, 0.45 * gap)
            mid  = 0.5 * (incumbent + closest)
            past = (pitch_midi - mid) if closest > incumbent else (mid - pitch_midi)
            if past < band:
                return incumbent
    return closest


# ── Python TuneState (mirrors C++ chooseTargetNoteAndRatio) ───────────────────
class TuneState:
    def __init__(self):
        self.smoothed_midi       = -1.0
        self.smoothed_target     = -1.0
        self.release_midi        = -1
        self.attack_samples      = 0
        self.note_held_samples   = 0
        self.candidate_midi      = -1
        self.candidate_stable    = 0
        self.voiced_sample_count = 0

    def reset_note_lock(self):
        self.release_midi      = -1
        self.attack_samples    = 0
        self.note_held_samples = 0
        self.smoothed_target   = -1.0
        self.voiced_sample_count = 0
        self.candidate_midi    = -1
        self.candidate_stable  = 0

    def compute_ratio(self, detected_hz, active_keys, block_size, sr,
                      attack_ms, release_ms, vibrato_amount=0.0):
        self.voiced_sample_count += block_size
        float_midi = hz_to_midi(detected_hz)

        block_dt = block_size / sr
        vib_time_ms = 201.0
        if self.smoothed_midi < 0:
            self.smoothed_midi = float_midi
        else:
            delta = abs(float_midi - self.smoothed_midi)
            jump_lo = vibrato_amount + 0.5
            jump_hi = vibrato_amount + 1.2
            if delta < jump_lo:   jump_factor = 0.0
            elif delta > jump_hi: jump_factor = 1.0
            else:                 jump_factor = (delta - jump_lo) / (jump_hi - jump_lo)
            eff_time_ms = vib_time_ms * (1 - jump_factor) + 1.0 * jump_factor
            alpha = 1.0 - math.exp(-block_dt / (eff_time_ms / 1000.0))
            self.smoothed_midi += alpha * (float_midi - self.smoothed_midi)

        dev = float_midi - self.smoothed_midi
        clamped_dev = max(-vibrato_amount, min(vibrato_amount, dev))
        eff_midi = self.smoothed_midi + clamped_dev

        hysteresis_st = 0.1
        lock_pitch = self.smoothed_midi if self.smoothed_midi > 0 else eff_midi
        best_midi = choose_note(active_keys, lock_pitch, self.release_midi, hysteresis_st)
        if best_midi < 0:
            best_midi = int(round(lock_pitch))

        grace_samples         = int(sr * 0.050)
        attack_stable_samples = int(sr * max(attack_ms, 1.0) / 1000.0)
        release_hold_samples  = int(sr * 0.030)

        if self.voiced_sample_count >= grace_samples and self.release_midi < 0:
            self.release_midi    = best_midi
            self.attack_samples  = 0
            self.note_held_samples = 0
            self.candidate_midi  = -1
            self.candidate_stable = 0

        if self.release_midi >= 0:
            self.attack_samples    += block_size
            self.note_held_samples += block_size
            if best_midi != self.release_midi:
                if self.note_held_samples >= release_hold_samples:
                    if best_midi == self.candidate_midi:
                        self.candidate_stable += block_size
                    else:
                        self.candidate_midi   = best_midi
                        self.candidate_stable = block_size
                    if self.candidate_stable >= attack_stable_samples:
                        self.release_midi    = self.candidate_midi
                        self.attack_samples  = 0
                        self.note_held_samples = 0
                        self.candidate_midi  = -1
                        self.candidate_stable = 0
            else:
                self.candidate_midi  = -1
                self.candidate_stable = 0

        engage_fade = max(0.0, min(1.0, 1000.0 * self.attack_samples / sr / max(attack_ms, 1.0)))
        lock_bypass = 1.0 - engage_fade
        selected_midi = self.release_midi if self.release_midi >= 0 else best_midi
        raw_target = selected_midi + clamped_dev
        raw_target = raw_target * (1 - lock_bypass) + eff_midi * lock_bypass

        block_dt_ms = 1000.0 * block_size / sr
        port_time_ms = 30.0
        t_alpha = 1.0 - math.exp(-block_dt_ms / port_time_ms)
        if self.smoothed_target < 0:
            self.smoothed_target = raw_target
        else:
            self.smoothed_target += t_alpha * (raw_target - self.smoothed_target)

        target_hz = 440.0 * (2.0 ** ((self.smoothed_target - 69.0) / 12.0))
        ratio = max(0.5, min(2.0, target_hz / detected_hz))
        return ratio, best_midi, selected_midi, self.smoothed_midi


# ── per-segment stats ─────────────────────────────────────────────────────────
def seg_stats(frames, t_start, t_end):
    """frames = list of (t, hz, tag); return midi values in window."""
    vals = [hz_to_midi(hz) for t, hz, _ in frames
            if t_start <= t <= t_end and hz > 0]
    if not vals:
        return None, None, None, 0
    arr = np.array(vals)
    return float(np.median(arr)), float(np.mean(arr)), float(np.std(arr)), len(arr)


def gap_artifacts(frames, segs, latency_s=0.0):
    """Count frames in gaps whose pitch belongs to neither neighbour."""
    count = 0
    details = []
    for i in range(len(segs) - 1):
        g_s = segs[i]["end"] + latency_s
        g_e = segs[i+1]["start"] + latency_s
        if g_e - g_s < 0.04:
            continue
        midi_a, midi_b = segs[i]["midi"], segs[i+1]["midi"]
        for t, hz, _ in frames:
            if g_s <= t <= g_e and hz > 0:
                m = hz_to_midi(hz)
                if min(abs(m - midi_a), abs(m - midi_b)) > 1.5:
                    count += 1
                    details.append((t, m, midi_a, midi_b))
    return count, details


# ── full pipeline simulation ──────────────────────────────────────────────────
def simulate_pipeline(wav_in_path, attack_ms=0.1, release_ms=10.0):
    """
    Run detector → selector → ratio on INPUT file.
    Returns per-block trace: list of dicts with keys:
      t, det_hz, det_midi, smoothed_midi, best_midi, selected_midi, ratio
    """
    data, sr = load_mono(wav_in_path)
    sr_f = float(sr)

    b0, alpha = _make_biquad_lpf(sr_f, LPF_CUTOFF)
    lpf1 = _apply_lpf(data, b0, alpha)
    lpf2 = _apply_lpf(lpf1, b0, alpha)

    circ = np.zeros(WIN)
    write_idx = 0

    MEDIAN_HIST = 5
    med_hist = [0.0] * MEDIAN_HIST
    med_i = 0
    med_filled = 0
    slow_anchor = 0.0
    out_of_range_count = 0
    last_out_of_range_pitch = 0.0
    last_valid = 0.0
    hold = 0
    HOLD_FRAMES = 8
    voiced_since_onset = 0
    ONSET_GUARD = 2
    silent_blocks = 0
    FLUSH_BLOCKS = 8
    last_known_good = 0.0

    def fold(hz):
        if slow_anchor <= 0: return hz
        r = hz / slow_anchor
        if   1.60 < r < 2.50: return hz * 0.5
        elif 0.40 < r < 0.63: return hz * 2.0
        elif 2.50 < r < 3.50: return hz / 3.0
        elif 0.29 < r < 0.40: return hz * 3.0
        elif 3.50 < r < 4.60: return hz / 4.0
        elif 0.22 < r < 0.29: return hz * 4.0
        return hz

    active_keys = [True] * 88
    tune = TuneState()
    was_voiced = False

    trace = []

    for start in range(0, len(data) - BLOCK, BLOCK):
        block_lpf = lpf2[start:start + BLOCK]
        block_raw = data[start:start + BLOCK]
        rms = math.sqrt(float(np.mean(block_raw ** 2)) + 1e-12)
        t = start / sr_f

        for s in block_lpf:
            circ[write_idx] = s
            write_idx = (write_idx + 1) % WIN

        raw_hz = 0.0
        in_onset = False
        clarity = 1.0
        if rms > 0.01:
            voiced_since_onset += 1
            if voiced_since_onset > ONSET_GUARD:
                idx = write_idx
                buf = np.roll(circ, -idx)
                raw_hz, clarity = amdf_pitch(buf, sr_f)
            else:
                in_onset = True
        else:
            voiced_since_onset = 0

        if in_onset:
            trace.append(dict(t=t, det_hz=last_valid, det_midi=hz_to_midi(last_valid) if last_valid > 0 else 0,
                              smoothed_midi=tune.smoothed_midi, best_midi=-1, selected_midi=-1,
                              ratio=1.0, tag='ONSET'))
            continue

        # detector post-processing (anchor fold + median + EMA)
        if raw_hz > 0:
            silent_blocks = 0
            folded = fold(raw_hz)
            med_hist[med_i] = folded
            med_i = (med_i + 1) % MEDIAN_HIST
            if med_filled < MEDIAN_HIST: med_filled += 1
            pitch_est = sorted(med_hist[:med_filled])[med_filled//2] if med_filled >= MEDIAN_HIST else folded

            if slow_anchor <= 0:
                if last_known_good > 0:
                    slow_anchor = last_known_good
                    pitch_est = fold(pitch_est)
                    slow_anchor = pitch_est
                    med_hist = [pitch_est] * MEDIAN_HIST
                    med_filled = MEDIAN_HIST
                else:
                    slow_anchor = pitch_est
                out_of_range_count = 0
                last_out_of_range_pitch = 0.0
            else:
                r = folded / slow_anchor
                if r < 0.749 or r > 1.334:
                    is_stable = False
                    if last_out_of_range_pitch > 0.0:
                        r_diff = folded / last_out_of_range_pitch
                        if 0.917 < r_diff < 1.091:
                            is_stable = True

                    if is_stable:
                        out_of_range_count += 1
                        if out_of_range_count >= 5:
                            slow_anchor = raw_hz
                            pitch_est = fold(raw_hz)
                            out_of_range_count = 0
                            last_out_of_range_pitch = 0.0
                            med_hist = [pitch_est] * MEDIAN_HIST
                            med_filled = MEDIAN_HIST
                    else:
                        out_of_range_count = 1
                        last_out_of_range_pitch = folded
                else:
                    out_of_range_count = 0
                    last_out_of_range_pitch = 0.0
                    r_est = pitch_est / slow_anchor
                    if 0.749 < r_est < 1.334:
                        slow_anchor = slow_anchor * 0.70 + pitch_est * 0.30

            last_known_good = pitch_est
            if last_valid > 0 and 0.85 < pitch_est / last_valid < 1.18:
                last_valid = last_valid * 0.7 + pitch_est * 0.3
            else:
                last_valid = pitch_est
            hold = 0
            det_hz = last_valid
        else:
            silent_blocks += 1
            if silent_blocks >= 2:
                med_hist = [0.0] * MEDIAN_HIST
                med_i = 0
                med_filled = 0
            if silent_blocks >= FLUSH_BLOCKS:
                slow_anchor = 0.0
                out_of_range_count = 0
                last_out_of_range_pitch = 0.0
            if hold < HOLD_FRAMES and last_valid > 0:
                hold += 1
                det_hz = last_valid
            else:
                last_valid = 0.0
                det_hz = 0.0

        # isVoiced mirror
        is_consonant = (clarity > CLARITY_THRESH and raw_hz <= 0 and rms > 0.015)
        is_voiced = det_hz > 0 and not is_consonant and rms > 0.01

        if is_consonant:
            tune.reset_note_lock()
        elif is_voiced and not was_voiced:
            tune.reset_note_lock()
        elif not is_voiced:
            pass  # release tail not simulated here

        was_voiced = is_voiced

        ratio, best_midi, selected_midi, sm = 1.0, -1, -1, tune.smoothed_midi
        if is_voiced:
            ratio, best_midi, selected_midi, sm = tune.compute_ratio(
                det_hz, active_keys, BLOCK, sr_f, attack_ms, release_ms)
        else:
            if not is_voiced and was_voiced is False:  # true silence
                tune.reset_note_lock()

        trace.append(dict(t=t, det_hz=det_hz,
                          det_midi=hz_to_midi(det_hz) if det_hz > 0 else 0,
                          smoothed_midi=sm,
                          best_midi=best_midi,
                          selected_midi=selected_midi,
                          ratio=ratio,
                          tag='VOICED' if is_voiced else 'SILENT'))

    return trace


# ── per-segment analysis ──────────────────────────────────────────────────────
def analyse_stage(trace, label):
    """For each segment analyse all 3 layers."""
    lines = []
    header = (f"\n{'─'*110}\n"
              f"{'#':>3} │ {'Note':<10} │ {'DET median':>10} │ {'DET std':>7} │ "
              f"{'SEL dominant':>12} │ {'SEL flip%':>9} │ {'ART in gap':>10} │ {'Status'}")
    lines.append(f"\n{'='*110}")
    lines.append(f"  LAYER DIAGNOSIS — {label}")
    lines.append(header)
    lines.append('─' * 110)

    # collect gap artifacts (between consecutive segments)
    gap_art = {}
    for i in range(len(SEGMENTS) - 1):
        g_s, g_e = SEGMENTS[i]["end"], SEGMENTS[i+1]["start"]
        if g_e - g_s < 0.04:
            gap_art[i+1] = 0
            continue
        ma, mb = SEGMENTS[i]["midi"], SEGMENTS[i+1]["midi"]
        cnt = sum(1 for f in trace
                  if g_s <= f['t'] <= g_e
                  and f['det_hz'] > 0
                  and min(abs(hz_to_midi(f['det_hz']) - ma),
                          abs(hz_to_midi(f['det_hz']) - mb)) > 1.5)
        gap_art[i+1] = cnt

    all_ok = True
    for i, seg in enumerate(SEGMENTS):
        t_s = seg["start"] + ONSET_SKIP_S
        t_e = seg["end"]
        target = seg["midi"]
        name   = seg["name"]

        # Detector layer
        det_vals  = [hz_to_midi(f['det_hz']) for f in trace
                     if t_s <= f['t'] <= t_e and f['det_hz'] > 0]
        det_med   = float(np.median(det_vals)) if det_vals else 0.0
        det_std   = float(np.std(det_vals))    if det_vals else 0.0
        det_err   = abs(det_med - target)

        # Selector layer
        sel_vals  = [f['selected_midi'] for f in trace
                     if t_s <= f['t'] <= t_e and f['selected_midi'] >= 0]
        from collections import Counter
        sel_counts = Counter(sel_vals)
        sel_dom   = sel_counts.most_common(1)[0][0] if sel_counts else -1
        sel_wrong = sum(v for k, v in sel_counts.items() if k != target)
        sel_flip  = 100.0 * sel_wrong / len(sel_vals) if sel_vals else 0.0

        # Gap artifact (before this segment)
        art = gap_art.get(i, 0)

        # Status flags
        flags = []
        if det_err > 1.0:  flags.append(f'DET_WRONG({det_err:.1f}st)')
        if det_std  > 1.2: flags.append(f'DET_UNSTABLE({det_std:.1f}st)')
        if sel_dom != target and sel_dom >= 0: flags.append(f'SEL_WRONG({sel_dom}vs{target})')
        if sel_flip > 20:  flags.append(f'SEL_FLIP({sel_flip:.0f}%)')
        if art > 3:        flags.append(f'GAP_ART({art}fr)')
        status = '  '.join(flags) if flags else 'OK'
        if flags: all_ok = False

        det_note = f"{det_med:.2f}" if det_vals else "---"
        sel_note = f"midi{sel_dom}" if sel_dom >= 0 else "---"

        lines.append(
            f"  {i+1:>2} │ {name:<10} │ {det_note:>10} │ {det_std:>6.2f}st │ "
            f"{sel_note:>12} │ {sel_flip:>8.1f}% │ {art:>10} │ {status}")

    lines.append('─' * 110)
    lines.append(f"  Overall: {'ALL OK' if all_ok else 'ISSUES FOUND (see flags above)'}")
    return '\n'.join(lines)


# ── main ──────────────────────────────────────────────────────────────────────
if __name__ == "__main__":
    input_wav = SAMPLE_DIR / "long_melody.wav"
    if not input_wav.exists():
        print(f"ERROR: {input_wav} not found")
        exit(1)

    print(f"\nInput: {input_wav}")
    print("Running full pipeline simulation (attack=0ms, all keys active)...")

    trace = simulate_pipeline(input_wav, attack_ms=0.1, release_ms=10.0)

    # Also read output WAVs to get pitch of the ACTUAL shifted audio
    print("\n" + "="*110)
    print("  OUTPUT PITCH ANALYSIS — comparing rendered WAVs vs expected notes")
    for label, wav_name in [("CLASSIC", "long_melody_classic.wav"), ("MODERN", "long_melody_modern.wav")]:
        out_wav = RESULT_DIR / wav_name
        if not out_wav.exists():
            print(f"  [{label}] {wav_name} not found — run C++ test first")
            continue
        out_frames, out_sr = pitch_track(out_wav, block=2048, hop=512)

        # Read latency if available
        lat_path = Path(str(out_wav) + ".lat")
        lat_s = 0.0
        if lat_path.exists():
            try:
                lat_s = int(lat_path.read_text().strip()) / float(out_sr)
            except:
                pass
        if lat_s > 0.0:
            print(f"  [INFO] {label}: latency compensation {lat_s*1000:.0f} ms")

        print(f"  ── Output pitch: {label} ──")
        for seg in SEGMENTS:
            t_s = seg["start"] + ONSET_SKIP_S + lat_s
            t_e = seg["end"] + lat_s
            vals = [hz_to_midi(hz) for ts, hz in out_frames if t_s <= ts <= t_e and hz > 0]
            if not vals:
                print(f"     {seg['name']}: no frames")
                continue
            med = float(np.median(vals))
            std = float(np.std(vals))
            err = med - seg["midi"]
            flag = " ← WRONG" if abs(err) > 0.6 else ""
            flag += " UNSTABLE" if std > 1.2 else ""
            print(f"     {seg['name']}:  median={med:.2f}  (err={err:+.2f}st)  std={std:.2f}st{flag}")

    print(analyse_stage(trace, "INPUT→DETECTOR→SELECTOR (Python model)"))

    # Count gap artifacts in actual OUTPUT
    print("\n  ── Gap artifacts in OUTPUT (inter-note pitch leakage) ──")
    for label, wav_name in [("CLASSIC", "long_melody_classic.wav"), ("MODERN", "long_melody_modern.wav")]:
        out_wav = RESULT_DIR / wav_name
        if not out_wav.exists():
            continue
        out_frames, out_sr = pitch_track(out_wav, block=2048, hop=512)
        out_list = [(ts, hz, 'V') for ts, hz in out_frames]

        lat_path = Path(str(out_wav) + ".lat")
        lat_s = 0.0
        if lat_path.exists():
            try:
                lat_s = int(lat_path.read_text().strip()) / float(out_sr)
            except:
                pass

        n_art, details = gap_artifacts(out_list, SEGMENTS, lat_s)
        print(f"  [{label}]  {n_art} artifact frames in gaps")
        for t, m, ma, mb in details[:8]:
            print(f"     t={t:.3f}s  midi={m:.1f}  (neighbours {ma},{mb})")
