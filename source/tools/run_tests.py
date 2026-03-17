#!/usr/bin/env python3
"""
run_tests.py - XenoScript language test runner.
Works on both Linux and Windows.

Usage:
    python3 source/tools/run_tests.py bin/xenoc bin/xenovm test/
"""
import os, sys, subprocess, glob

def run(cmd):
    r = subprocess.run(cmd, capture_output=True)
    return r.stdout, r.stderr

def read(path):
    try:
        return open(path, "rb").read().replace(b"\r\n", b"\n").replace(b"\r", b"\n")
    except FileNotFoundError:
        return b""

def strip_timing(b):
    lines = b.splitlines(keepends=True)
    if lines and lines[-1].startswith(b"Execution time:"):
        lines = lines[:-1]
    return b"".join(lines)

def main():
    if len(sys.argv) < 4:
        print(f"Usage: {sys.argv[0]} <xenoc> <xenovm> <test_dir>")
        sys.exit(1)

    xenoc, xenovm, test_dir = sys.argv[1], sys.argv[2], sys.argv[3]
    tests = sorted(glob.glob(os.path.join(test_dir, "*.xeno")))

    if not tests:
        print(f"No tests found in {test_dir}")
        sys.exit(1)

    print()
    print("==========================================")
    print("  Running XenoScript language tests")
    print("==========================================")

    passed = failed = 0

    for xeno in tests:
        base = xeno[:-5]  # strip .xeno
        xbc  = base + ".xbc"

        # Compile
        cout, cerr = run([xenoc, xeno, "-o", xbc])

        # Run if .xbc was produced
        if os.path.exists(xbc):
            rout, rerr = run([xenovm, xbc])
            os.remove(xbc)
        else:
            rout, rerr = b"", b""

        # Normalize line endings
        norm = lambda b: b.replace(b"\r\n", b"\n").replace(b"\r", b"\n")
        actual_out = strip_timing(norm(rout))
        actual_err = norm(cerr + cout + rerr)

        expected_out_path = base + ".out"
        expected_err_path = base + ".err"

        # Auto-generate expected files if missing
        if not os.path.exists(expected_out_path):
            with open(expected_out_path, "wb") as f:
                f.write(actual_out)
            print(f"  [GEN] {expected_out_path}")

        if not os.path.exists(expected_err_path):
            with open(expected_err_path, "wb") as f:
                f.write(actual_err)
            print(f"  [GEN] {expected_err_path}")

        expected_out = strip_timing(read(expected_out_path))
        expected_err = read(expected_err_path)

        if actual_out == expected_out and actual_err == expected_err:
            passed += 1
            print(f"  [PASS] {xeno}")
        else:
            failed += 1
            print(f"  [FAIL] {xeno}")
            if actual_out != expected_out:
                print(f"    stdout expected: {expected_out!r}")
                print(f"    stdout actual:   {actual_out!r}")
            if actual_err != expected_err:
                print(f"    stderr expected: {expected_err!r}")
                print(f"    stderr actual:   {actual_err!r}")

    print()
    print(f"  {passed} passed, {failed} failed.")
    print("==========================================")
    sys.exit(0 if failed == 0 else 1)

if __name__ == "__main__":
    main()
