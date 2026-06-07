"""Tests: breath_gate_with_breath, breath_gate_no_breath"""

import math
import numpy as np
from utils import check, get_test, load_mono, rms_db, RESULT_DIR, SAMPLE_DIR


def run(failures: list):
    print("\n=== Test 1 & 2: Breath Gate ===")

    t1 = get_test("breath_gate_with_breath")
    t2 = get_test("breath_gate_no_breath")

    breath_db    = rms_db(RESULT_DIR / t1["result"])
    no_breath_db = rms_db(RESULT_DIR / t2["result"])

    c1 = t1["checks"][0]
    check(failures, "T1 breath attenuated",
          breath_db < no_breath_db + c1["max_db"],
          f"breath={breath_db:.1f} dBFS  no_breath={no_breath_db:.1f} dBFS")

    c2 = t2["checks"][0]
    data_in, _ = load_mono(SAMPLE_DIR / t2["source"])
    rms_in = math.sqrt(float(np.mean(data_in ** 2)) + 1e-12)
    input_db = 20 * math.log10(rms_in)
    attn = no_breath_db - input_db
    check(failures, "T2 no-breath unaffected",
          c2["min_db"] <= attn <= c2["max_db"],
          f"attenuation={attn:.2f} dB (expected {c2['min_db']}..{c2['max_db']})")
