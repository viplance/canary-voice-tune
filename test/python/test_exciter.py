"""Test: exciter_on — HF energy must exceed exciter_off baseline."""

import math
import numpy as np
from utils import check, get_test, load_mono, RESULT_DIR


def _hf_rms(wav_path, cutoff_hz: float) -> float:
    data, sr = load_mono(wav_path)
    alpha = 1.0 - math.exp(-2 * math.pi * cutoff_hz / sr)
    hp, prev = np.zeros_like(data), 0.0
    for i, x in enumerate(data):
        prev = alpha * x + (1 - alpha) * prev
        hp[i] = x - prev
    return math.sqrt(float(np.mean(hp ** 2)) + 1e-12)


def run(failures: list):
    print("\n=== Test 3: Exciter ===")

    t = get_test("exciter_on")
    c = t["checks"][0]

    hf_off = _hf_rms(RESULT_DIR / get_test("exciter_off")["result"], c["hf_cutoff_hz"])
    hf_on  = _hf_rms(RESULT_DIR / t["result"], c["hf_cutoff_hz"])
    ratio  = hf_on / (hf_off + 1e-12)

    check(failures, "T3 exciter adds HF energy",
          ratio >= c["min_ratio"],
          f"HF ratio on/off = {ratio:.3f} (need ≥{c['min_ratio']})")
