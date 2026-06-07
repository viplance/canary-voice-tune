"""Test: classic_click — no introduced clicks in classic mode output."""

import math
import numpy as np
from utils import check, get_test, load_mono, RESULT_DIR


def run(failures: list):
    print("\n=== Test 5: Classic Click ===")

    t = get_test("classic_click")
    c = t["checks"][0]

    data, sr = load_mono(RESULT_DIR / t["result"])
    win_short = max(1, int(sr * c["window_short_ms"] / 1000))
    win_long  = max(1, int(sr * c["window_long_ms"]  / 1000))
    threshold = c["rms_ratio_threshold"]
    floor     = c["active_floor"]

    click_count = 0
    for i in range(win_long, len(data) - win_long, win_short):
        rms_s = math.sqrt(float(np.mean(data[i:i + win_short] ** 2)) + 1e-12)
        rms_l = math.sqrt(float(np.mean(data[i - win_long:i + win_long] ** 2)) + 1e-12)
        if rms_l > floor and rms_s > threshold * rms_l:
            click_count += 1

    check(failures, "T5 no clicks in classic output",
          click_count == 0,
          f"{click_count} click(s) detected")
