"""Test: octave_jump — jump_notes_out.wav exists and has voiced content.
The octave-accuracy of the C++ YIN detector is validated in Test_OctaveJump.cpp
(which ran the detector and checked pitch ranges directly). Here we just confirm
the render completed and produced a non-silent file.
"""

import math
import numpy as np
from utils import check, get_test, load_mono, RESULT_DIR


def run(failures: list):
    print("\n=== Test 6: OctaveJump (output exists and has audio) ===")

    t = get_test("octave_jump")

    data, sr = load_mono(RESULT_DIR / t["result"])
    rms = math.sqrt(float(np.mean(data ** 2)) + 1e-12)
    rms_db = 20 * math.log10(rms)

    check(failures, "T6 output file is non-silent",
          rms_db > -60.0,
          f"RMS = {rms_db:.1f} dBFS")
