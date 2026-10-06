#!/usr/bin/env python3
"""
plot.py -- renders the simulator's CSV as three grouped bar charts.

Standalone and entirely outside the compiler: it only reads results/results.csv
(columns: benchmark,config,metric,value) and writes
    results/cycles.png   total cycles of the whole program
    results/bytes.png    bytes that crossed the memory bus
    results/energy.png   energy, in nanojoules

The three benchmarks the cost model must decline (matmul, transpose, indirect)
are marked with a dagger on the axis: there NAIVE_OFFLOAD is slower than
HOST_ONLY and NEMO_C matches HOST_ONLY because it correctly refuses to offload.

usage:  python plot.py [results.csv] [output-directory]
"""
import csv
import os
import sys

import matplotlib
matplotlib.use("Agg")
import matplotlib.pyplot as plt

CONFIGS = ["HOST_ONLY", "NAIVE_OFFLOAD", "NEMO_C"]
COLORS = {"HOST_ONLY": "#8c8c8c", "NAIVE_OFFLOAD": "#d55e00", "NEMO_C": "#0072b2"}
REJECTED = {"matmul", "transpose", "indirect"}
ORDER = ["vecadd", "saxpy", "dotprod", "maxreduce", "histogram", "gemv", "stencil1d",
         "stencil2d", "filter", "fusion_chain", "mixed_loop", "colmajor",
         "matmul", "transpose", "indirect"]

CHARTS = [
    ("cycles",    "Total cycles (lower is better)",              "cycles.png", 1.0),
    ("bus_bytes", "Bytes crossing the memory bus (lower is better)", "bytes.png", 1.0),
    ("energy_pj", "Energy in nJ (lower is better)",               "energy.png", 1e-3),
]


def load(path):
    data = {}
    with open(path, newline="") as f:
        for row in csv.DictReader(f):
            data.setdefault(row["benchmark"], {}).setdefault(row["config"], {})[row["metric"]] = float(row["value"])
    return data


def render(data, metric, title, filename, scale, outdir):
    benches = [b for b in ORDER if b in data] + sorted(b for b in data if b not in ORDER)
    n = len(benches)
    width = 0.27
    fig, ax = plt.subplots(figsize=(max(9, 0.95 * n), 5.2))
    for i, cfg in enumerate(CONFIGS):
        xs = [k + (i - 1) * width for k in range(n)]
        ys = [data[b].get(cfg, {}).get(metric, 0.0) * scale for b in benches]
        ax.bar(xs, ys, width, label=cfg, color=COLORS[cfg])
    ax.set_yscale("log")
    ax.set_ylabel(title)
    ax.set_xticks(range(n))
    ax.set_xticklabels([b + (" †" if b in REJECTED else "") for b in benches],
                       rotation=40, ha="right")
    ax.set_title(title + "\n† = nest the cost model rejects: NEMO_C declines, NAIVE_OFFLOAD pays for it")
    ax.grid(axis="y", which="major", alpha=0.3)
    ax.legend()
    fig.tight_layout()
    path = os.path.join(outdir, filename)
    fig.savefig(path, dpi=140)
    plt.close(fig)
    print("wrote", path)


def main():
    csv_path = sys.argv[1] if len(sys.argv) > 1 else os.path.join("results", "results.csv")
    outdir = sys.argv[2] if len(sys.argv) > 2 else os.path.dirname(csv_path) or "."
    data = load(csv_path)
    for metric, title, filename, scale in CHARTS:
        render(data, metric, title, filename, scale, outdir)


if __name__ == "__main__":
    main()
