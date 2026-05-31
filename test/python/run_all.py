#!/usr/bin/env python3
"""
Stage 2: Independent Python verification of test/result/*.wav

Usage:
    python3 test/python/run_all.py            # run all tests
    python3 test/python/run_all.py t1 t2      # run specific tests by id

Available test ids: breath, exciter, click, octave, melody

Exit code 0 = all checks pass, non-zero = at least one check failed.
"""

import sys
from pathlib import Path

# Allow importing sibling modules when invoked from any working directory
sys.path.insert(0, str(Path(__file__).parent))

import test_breath_gate
import test_exciter
import test_classic_click
import test_octave_jump
import test_long_melody

ALL_TESTS = {
    "breath":  test_breath_gate.run,
    "exciter": test_exciter.run,
    "click":   test_classic_click.run,
    "octave":  test_octave_jump.run,
    "melody":  test_long_melody.run,
}

def main():
    requested = sys.argv[1:] if len(sys.argv) > 1 else list(ALL_TESTS.keys())
    unknown = [t for t in requested if t not in ALL_TESTS]
    if unknown:
        print(f"Unknown test id(s): {unknown}")
        print(f"Available: {list(ALL_TESTS.keys())}")
        sys.exit(2)

    print("===========================================")
    print("Stage 2: Verifying test/result/*.wav (Python)")
    print("===========================================")

    failures = []
    for name in requested:
        ALL_TESTS[name](failures)

    print("\n===========================================")
    if failures:
        print(f"FAILED: {len(failures)} check(s):")
        for f in failures:
            print(f"  - {f}")
        sys.exit(1)
    else:
        print("All checks PASSED.")
        sys.exit(0)

if __name__ == "__main__":
    main()
