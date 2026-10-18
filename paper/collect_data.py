#!/usr/bin/env python3
# Collects the extra measurements for the manuscript (runs inside WSL, in the nemoc folder).
import re, subprocess, time, json, statistics, os

BENCH = "vecadd saxpy dotprod maxreduce histogram gemv stencil1d stencil2d filter fusion_chain mixed_loop colmajor matmul transpose indirect".split()
out = {"spills": {}, "compile_ms": {}, "dpu_instr": {}, "tile": {}}

def run(args):
    return subprocess.run(["./nemoc"] + args, capture_output=True, text=True)

# register spills against K for two kernels
for b in ("stencil2d", "stencil1d", "gemv"):
    out["spills"][b] = {}
    for k in range(3, 25):
        r = run(["benchmarks/%s.nmc" % b, "--dump", "regalloc", "--regs", str(k)])
        sp = re.search(r"registers spilled to scratchpad\s*:\s*(\d+)", r.stdout)
        used = re.search(r"physical registers used\s*:\s*(\d+)", r.stdout)
        ok = "colouring verified              : yes" in r.stdout
        out["spills"][b][k] = [int(sp.group(1)) if sp else None, int(used.group(1)) if used else None, ok]

# compile time (front end to code generation), median of 7 runs
for b in BENCH:
    ts = []
    for _ in range(7):
        t0 = time.perf_counter()
        r = run(["benchmarks/%s.nmc" % b, "--codegen"])
        ts.append((time.perf_counter() - t0) * 1000)
    out["compile_ms"][b] = round(statistics.median(ts), 1)
    for m in re.finditer(r"wrote build/(\S+)_dpu\.pim\s+\((\d+) instructions, tile (\d+) iterations\)", r.stdout):
        out["dpu_instr"][m.group(1)] = int(m.group(2))
        out["tile"][m.group(1)] = int(m.group(3))

print(json.dumps(out, indent=1))
