#!/usr/bin/env python3
"""
run_speed.py - Benchmark XenoScript VM performance.
Runs both .xeno (compile+run) and .xbc (run only) multiple times
and prints average execution times.

Usage:
    python3 run_speed.py bin/xenoc bin/xenovm test/01_primitives.xeno 20
"""

import os, sys, subprocess, statistics

def run(cmd):
    r = subprocess.run(cmd, capture_output=True, text=True)
    return r.stdout, r.stderr

def extract_time(output):
    """
    Extracts the trailing 'Execution time: X ms' line.
    Returns float milliseconds or None.
    """
    for line in output.splitlines():
        if line.startswith("Execution time:"):
            try:
                return float(line.split(":")[1].strip().split()[0])
            except:
                return None
    return None

def main():
    if len(sys.argv) < 5:
        print(f"Usage: {sys.argv[0]} <xenoc> <xenovm> <file.xeno> <iterations>")
        sys.exit(1)

    xenoc, xenovm, xeno_file, iters = sys.argv[1], sys.argv[2], sys.argv[3], int(sys.argv[4])
    xbc_file = xeno_file.replace(".xeno", ".xbc")

    print("\n==========================================")
    print("  XenoScript Speed Benchmark")
    print("==========================================")

    compile_run_times = []
    vm_only_times = []

    # --- Benchmark .xeno (compile + run) ---
    print(f"\nBenchmarking full pipeline (.xeno) for {iters} iterations...")
    for _ in range(iters):
        # Run VM
        out, _ = run([xenovm, xeno_file])
        t = extract_time(out)
        if t is not None:
            compile_run_times.append(t)

    # Compile
    _, _ = run([xenoc, xeno_file, "-o", xbc_file])

    # --- Benchmark .xbc (VM only) ---
    print(f"\nBenchmarking VM only (.xbc) for {iters} iterations...")
    for _ in range(iters):
        out, _ = run([xenovm, xbc_file])
        t = extract_time(out)
        if t is not None:
            vm_only_times.append(t)

    # Cleanup
    if os.path.exists(xbc_file):
        os.remove(xbc_file)

    # --- Print results ---
    print("\n==========================================")
    print("  Results")
    print("==========================================")

    if compile_run_times:
        print(f"Full pipeline (.xeno):")
        print(f"  Avg: {statistics.mean(compile_run_times):.3f} ms")
        print(f"  Min: {min(compile_run_times):.3f} ms")
        print(f"  Max: {max(compile_run_times):.3f} ms")

    if vm_only_times:
        print(f"\nVM only (.xbc):")
        print(f"  Avg: {statistics.mean(vm_only_times):.3f} ms")
        print(f"  Min: {min(vm_only_times):.3f} ms")
        print(f"  Max: {max(vm_only_times):.3f} ms")

    print("==========================================")

if __name__ == "__main__":
    main()
