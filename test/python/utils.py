"""Shared utilities for audio analysis tests."""

import json
import math
import numpy as np
import soundfile as sf
from pathlib import Path

TEST_DIR    = Path(__file__).parent.parent
RESULT_DIR  = TEST_DIR / "result"
SAMPLE_DIR  = TEST_DIR / "samples"
MAPPING     = json.loads((SAMPLE_DIR / "mapping.json").read_text())

PASS_TAG = "\033[32mPASS\033[0m"
FAIL_TAG = "\033[31mFAIL\033[0m"


def get_test(test_id: str) -> dict:
    for t in MAPPING["tests"]:
        if t["id"] == test_id:
            return t
    raise KeyError(f"Test '{test_id}' not found in mapping.json")


def check(failures: list, name: str, ok: bool, detail: str = ""):
    tag = PASS_TAG if ok else FAIL_TAG
    msg = f"  [{tag}] {name}"
    if detail:
        msg += f"  ({detail})"
    print(msg)
    if not ok:
        failures.append(name)


def load_mono(wav_path: Path) -> tuple[np.ndarray, int]:
    data, sr = sf.read(str(wav_path))
    if data.ndim > 1:
        data = data.mean(axis=1)
    return data.astype(np.float32), sr


def rms(samples: np.ndarray) -> float:
    return math.sqrt(float(np.mean(samples ** 2)) + 1e-12)


def rms_db(wav_path: Path) -> float:
    data, _ = load_mono(wav_path)
    return 20 * math.log10(rms(data))


def hz_to_midi(hz: float) -> float:
    if hz <= 0:
        return -1.0
    return 69.0 + 12.0 * math.log2(hz / 440.0)


def detect_pitch_ac(samples: np.ndarray, sr: int,
                    fmin: float = 60.0, fmax: float = 1200.0) -> float:
    """Autocorrelation-based pitch, independent from C++ YIN. Returns Hz or 0."""
    n = len(samples)
    if n < 512 or rms(samples) < 0.005:
        return 0.0
    lag_min = max(1, int(sr / fmax))
    lag_max = min(n // 2, int(sr / fmin))
    if lag_min >= lag_max:
        return 0.0
    r0 = float(np.dot(samples, samples))
    if r0 < 1e-12:
        return 0.0
    best_corr, best_lag = -1.0, lag_min
    for lag in range(lag_min, lag_max + 1):
        norm = float(np.dot(samples[:n - lag], samples[lag:])) / r0
        if norm > best_corr:
            best_corr, best_lag = norm, lag
    if best_corr < 0.45:
        return 0.0
    return sr / best_lag


def pitch_track(wav_path: Path, block: int = 2048, hop: int = 512,
                fmin: float = 60.0, fmax: float = 1200.0) -> tuple[list, int]:
    """Return list of (time_sec, hz) for every hop, and sample rate."""
    data, sr = load_mono(wav_path)
    results = []
    for start in range(0, len(data) - block, hop):
        hz = detect_pitch_ac(data[start:start + block], sr, fmin, fmax)
        results.append((start / sr, hz))
    return results, sr
