#!/usr/bin/env python3
"""
make_figures.py -- draws every results figure of the NEMO-C manuscript from the
project's own output, so no number in the paper is typed by hand.

Inputs : ../results/results.csv     (make run-all)
         ../results/decisions.md    (make run-all)
         paperdata.json             (register spills, compile time, DPU code size)
Outputs: figures/fig_*.pdf, and the derived statistics printed to stdout.

    python make_figures.py
"""
import csv, json, math, os, re
import matplotlib
matplotlib.use("Agg")
import matplotlib.pyplot as plt
import matplotlib.ticker

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)
FIG = os.path.join(HERE, "figures")
os.makedirs(os.path.join(FIG, "preview"), exist_ok=True)

ORDER = ["vecadd", "saxpy", "dotprod", "maxreduce", "histogram", "gemv", "stencil1d", "stencil2d",
         "filter", "fusion_chain", "mixed_loop", "colmajor", "matmul", "transpose", "indirect"]
SHORT = {"fusion_chain": "fusion", "mixed_loop": "mixed", "maxreduce": "maxred", "histogram": "histo",
         "stencil1d": "sten1d", "stencil2d": "sten2d", "transpose": "transp", "indirect": "indir",
         "colmajor": "colmaj"}

plt.rcParams.update({"font.size": 8, "font.family": "serif", "axes.linewidth": 0.6,
                     "xtick.major.width": 0.6, "ytick.major.width": 0.6, "legend.fontsize": 7,
                     "legend.frameon": False, "savefig.bbox": "tight", "savefig.pad_inches": 0.02})
C_NAIVE, C_NEMO, C_HOST, C_AI = "#9a9a9a", "#1f4fc4", "#4d4d4d", "#e08a2e"

# ---- data -------------------------------------------------------------------------------
data = {}
with open(os.path.join(ROOT, "results", "results.csv")) as f:
    for row in csv.DictReader(f):
        data.setdefault(row["benchmark"], {}).setdefault(row["config"], {})[row["metric"]] = float(row["value"])

ai = {}
with open(os.path.join(ROOT, "results", "decisions.md")) as f:
    for line in f:
        c = [x.strip() for x in line.strip().strip("|").split("|")]
        if len(c) >= 7 and c[0] in ORDER:
            # the first nest of a benchmark decides its AI for the single-metric policy
            ai.setdefault(c[0], float(c[2]) if c[2] != "n/a" else 0.0)

with open(os.path.join(HERE, "paperdata.json"), encoding="utf-8-sig") as f:
    extra = json.load(f)

def sp(b, cfg):
    return data[b]["HOST_ONLY"]["cycles"] / data[b][cfg]["cycles"]

def gmean(xs):
    return math.exp(sum(math.log(x) for x in xs) / len(xs))

def save(fig, name):
    fig.savefig(os.path.join(FIG, name + ".pdf"))
    fig.savefig(os.path.join(FIG, "preview", name + ".png"), dpi=200)
    plt.close(fig)

labels = [SHORT.get(b, b) for b in ORDER]
x = range(len(ORDER))

def bars(ax, a, b, la, lb):
    w = 0.38
    ax.bar([i - w / 2 for i in x], a, w, color=C_NAIVE, label=la, edgecolor="none")
    ax.bar([i + w / 2 for i in x], b, w, color=C_NEMO, label=lb, edgecolor="none")
    ax.set_xticks(list(x))
    ax.set_xticklabels(labels, rotation=55, ha="right")
    ax.axhline(1.0, color="black", lw=0.6, ls="--")
    ax.set_xlim(-0.6, len(ORDER) - 0.4)

# ---- Fig: speedup ------------------------------------------------------------------------
naive = [sp(b, "NAIVE_OFFLOAD") for b in ORDER]
nemo = [sp(b, "NEMO_C") for b in ORDER]
fig, ax = plt.subplots(figsize=(3.5, 2.1))
bars(ax, naive, nemo, "NAIVE_OFFLOAD", "NEMO-C")
ax.set_yscale("log")
ax.yaxis.set_minor_formatter(matplotlib.ticker.NullFormatter())
ax.set_yticks([0.4, 0.6, 1, 2, 4, 8])
ax.set_yticklabels(["0.4", "0.6", "1", "2", "4", "8"])
ax.set_ylabel("Speedup over HOST_ONLY (log)")
ax.legend(loc="lower left", bbox_to_anchor=(0, 1.0), ncol=2)
save(fig, "fig_speedup")

# ---- Fig: memory-bus traffic -----------------------------------------------------------
tn = [data[b]["NAIVE_OFFLOAD"]["bus_bytes"] / data[b]["HOST_ONLY"]["bus_bytes"] for b in ORDER]
tm = [data[b]["NEMO_C"]["bus_bytes"] / data[b]["HOST_ONLY"]["bus_bytes"] for b in ORDER]
fig, ax = plt.subplots(figsize=(3.5, 2.1))
bars(ax, tn, tm, "NAIVE_OFFLOAD", "NEMO-C")
ax.set_yscale("log")
ax.yaxis.set_minor_formatter(matplotlib.ticker.NullFormatter())
ax.set_yticks([0.1, 0.25, 0.5, 1, 2, 4, 8])
ax.set_yticklabels(["0.1", "0.25", "0.5", "1", "2", "4", "8"])
ax.set_ylabel("Bus bytes / HOST_ONLY (log)")
ax.legend(loc="lower left", bbox_to_anchor=(0, 1.0), ncol=2)
save(fig, "fig_traffic")

# ---- Fig: energy -------------------------------------------------------------------------
en = [data[b]["NAIVE_OFFLOAD"]["energy_pj"] / data[b]["HOST_ONLY"]["energy_pj"] for b in ORDER]
em = [data[b]["NEMO_C"]["energy_pj"] / data[b]["HOST_ONLY"]["energy_pj"] for b in ORDER]
fig, ax = plt.subplots(figsize=(3.5, 2.1))
bars(ax, en, em, "NAIVE_OFFLOAD", "NEMO-C")
ax.set_yscale("log")
ax.yaxis.set_minor_formatter(matplotlib.ticker.NullFormatter())
ax.set_yticks([0.1, 0.25, 0.5, 1, 2, 4, 8])
ax.set_yticklabels(["0.1", "0.25", "0.5", "1", "2", "4", "8"])
ax.set_ylabel("Energy / HOST_ONLY (log)")
ax.legend(loc="lower left", bbox_to_anchor=(0, 1.0), ncol=2)
save(fig, "fig_energy")

# ---- policy comparison ---------------------------------------------------------------------
# offload-all  : what a PrIM/SimplePIM-style hand port does -- every parallel kernel on PIM (= NAIVE_OFFLOAD)
# AI-only      : a single-metric roofline rule -- offload when AI < 0.5, nothing else checked
# NEMO-C       : the six-check predicate plus the enabling transformations
def ai_only(b):
    return data[b]["NAIVE_OFFLOAD"]["cycles"] if ai.get(b, 0.0) < 0.5 else data[b]["HOST_ONLY"]["cycles"]

pol = {
    "HOST_ONLY": [1.0 for b in ORDER],
    "Offload-all": [sp(b, "NAIVE_OFFLOAD") for b in ORDER],
    "AI-only": [data[b]["HOST_ONLY"]["cycles"] / ai_only(b) for b in ORDER],
    "NEMO-C": [sp(b, "NEMO_C") for b in ORDER],
}
oracle = [max(1.0, sp(b, "NAIVE_OFFLOAD")) for b in ORDER]   # best of the two untransformed choices
stats = {}
for k, v in pol.items():
    stats[k] = {
        "gmean": gmean(v),
        "slowdowns": sum(1 for s in v if s < 0.995),
        "worst": min(v),
        "correct": sum(1 for s, o in zip(v, oracle) if s >= o - 1e-9),
    }
fig, (a1, a2) = plt.subplots(1, 2, figsize=(3.5, 1.8))
names = list(pol)
cols = [C_HOST, C_NAIVE, C_AI, C_NEMO]
a1.bar(names, [stats[n]["gmean"] for n in names], color=cols, edgecolor="none")
a1.axhline(1.0, color="black", lw=0.6, ls="--")
a1.set_ylabel("Geo-mean speedup")
a2.bar(names, [stats[n]["correct"] for n in names], color=cols, edgecolor="none")
a2.set_ylabel("Correct decisions (of 15)")
a2.set_ylim(0, 15.5)
for a in (a1, a2):
    a.set_xticks(range(len(names)))
    a.set_xticklabels(names, rotation=40, ha="right")
fig.tight_layout(w_pad=1.0)
save(fig, "fig_policy")

# ---- Fig: register spills against K --------------------------------------------------------
fig, ax = plt.subplots(figsize=(3.5, 1.9))
marks = {"stencil2d": "o", "gemv": "s", "stencil1d": "^"}
for b in ("stencil2d", "gemv", "stencil1d"):
    ks = sorted(int(k) for k in extra["spills"][b])
    ax.plot(ks, [extra["spills"][b][str(k)][0] for k in ks], marker=marks[b], ms=3, lw=1, label=b)
ax.axvline(24, color="black", lw=0.6, ls="--")
ax.text(23.6, 7, "K = 24\n(DPU budget)", ha="right", fontsize=7)
ax.set_xlabel("Physical registers K")
ax.set_ylabel("Virtual registers spilled")
ax.legend(loc="lower left", bbox_to_anchor=(0, 1.0), ncol=3)
save(fig, "fig_spills")

# ---- Fig: compile time ----------------------------------------------------------------------
fig, ax = plt.subplots(figsize=(3.5, 1.9))
ct = [extra["compile_ms"][b] for b in ORDER]
colors = [C_NEMO if data[b]["NEMO_C"]["pim_nests"] > 0 else C_NAIVE for b in ORDER]
ax.bar(list(x), ct, color=colors, edgecolor="none")
ax.set_xticks(list(x))
ax.set_xticklabels(labels, rotation=55, ha="right")
ax.set_ylabel("Compile time (ms)")
ax.set_xlim(-0.6, len(ORDER) - 0.4)
from matplotlib.patches import Patch
ax.legend(handles=[Patch(color=C_NEMO, label="offloaded: full back end"),
                   Patch(color=C_NAIVE, label="rejected: no DPU code")],
          loc="lower left", bbox_to_anchor=(0, 1.0), ncol=2)
save(fig, "fig_compile")

# ---- numbers quoted in the text -------------------------------------------------------------
tot = {c: sum(data[b][c]["cycles"] for b in ORDER) for c in ("HOST_ONLY", "NAIVE_OFFLOAD", "NEMO_C")}
print("total cycles", tot)
print("aggregate speedup naive %.3f nemo %.3f" % (tot["HOST_ONLY"] / tot["NAIVE_OFFLOAD"], tot["HOST_ONLY"] / tot["NEMO_C"]))
for k, v in stats.items():
    print(k, {kk: (round(vv, 3) if isinstance(vv, float) else vv) for kk, vv in v.items()})
print("AI used by AI-only policy", ai)
print("AI-only total cycles", sum(ai_only(b) for b in ORDER))
etot = {c: sum(data[b][c]["energy_pj"] for b in ORDER) for c in ("HOST_ONLY", "NAIVE_OFFLOAD", "NEMO_C")}
btot = {c: sum(data[b][c]["bus_bytes"] for b in ORDER) for c in ("HOST_ONLY", "NAIVE_OFFLOAD", "NEMO_C")}
print("total energy pJ", etot)
print("total bus bytes", btot)
for b in ORDER:
    print("%-12s naive %.3f nemo %.3f  bytesN %.3f bytesM %.3f  enN %.3f enM %.3f" % (
        b, sp(b, "NAIVE_OFFLOAD"), sp(b, "NEMO_C"), tn[ORDER.index(b)], tm[ORDER.index(b)], en[ORDER.index(b)], em[ORDER.index(b)]))
print("compile ms mean %.1f max %.1f" % (sum(ct) / len(ct), max(ct)))
