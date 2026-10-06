#!/bin/bash
# run_all.sh -- compile and simulate every benchmark, then write results/
#
#   results/results.csv          every benchmark x config x metric
#   results/decisions.md         the offload decision for every nest
#   results/transformations.md   fission / interchange / fusion log
#   results/cycles.png bytes.png energy.png   (via plot.py, if matplotlib is found)
#   docs/graphs/*.dot            Graphviz output: CFG, dependence graph, interference graph
set -u
cd "$(dirname "$0")/.."

BENCH="vecadd saxpy dotprod maxreduce histogram gemv stencil1d stencil2d filter fusion_chain mixed_loop colmajor matmul transpose indirect"
mkdir -p results build/dot docs/graphs
fail=0

echo "benchmark,config,metric,value" > results/results.csv
for b in $BENCH; do
    ./nemoc benchmarks/$b.nmc --simulate --csv results/results.csv > results/sim_$b.txt 2>&1 || { fail=1; echo "FAILED: $b"; cat results/sim_$b.txt; }
    grep -E "^  speedup|^[a-z_0-9]+ +(HOST_ONLY|NAIVE_OFFLOAD|NEMO_C)" results/sim_$b.txt | sed 's/^/  /' | head -4
done

{
  echo "# Offload decisions"
  echo
  echo "One row per loop nest, produced by \`./nemoc <benchmark> --report-decisions\`."
  echo "AI is arithmetic intensity (flops per byte moved) against a machine balance of 0.50."
  echo "Reuse lists the arrays with significant self-temporal reuse."
  echo
  echo "| benchmark | nest | AI | reuse | class | verdict | reason |"
  echo "|---|---|---|---|---|---|---|"
  for b in $BENCH; do ./nemoc benchmarks/$b.nmc --report-decisions; done
} > results/decisions.md

{
  echo "# Enabling transformations"
  echo
  echo "Loop distribution (fission), interchange and fusion applied by the cost-model-guided"
  echo "compiler, each re-checked by the dependence and profitability analyses."
  echo
  for b in $BENCH; do ./nemoc benchmarks/$b.nmc --report-transforms; done
} > results/transformations.md

# Graphviz output for the docs
./nemoc benchmarks/saxpy.nmc --emit-dot cfg > /dev/null && cp build/dot/main.cfg.dot docs/graphs/saxpy_main.cfg.dot && cp build/dot/saxpy.cfg.dot docs/graphs/saxpy.cfg.dot
./nemoc benchmarks/stencil1d.nmc --emit-dot deps > /dev/null && cp build/dot/stencil1d.deps.dot docs/graphs/stencil1d.deps.dot
./nemoc benchmarks/mixed_loop.nmc --emit-dot deps > /dev/null && cp build/dot/mixed.deps.dot docs/graphs/mixed_loop.deps.dot
./nemoc benchmarks/gemv.nmc --emit-dot deps > /dev/null && cp build/dot/gemv.deps.dot docs/graphs/gemv.deps.dot
./nemoc benchmarks/stencil2d.nmc --emit-dot interference > /dev/null && cp build/dot/stencil2d.interference.dot docs/graphs/stencil2d.interference.dot

# charts: python3 with matplotlib, else the Windows interpreter through WSL interop
if python3 -c "import matplotlib" 2>/dev/null; then
    python3 plot.py results/results.csv results
elif command -v python.exe >/dev/null 2>&1 && python.exe -c "import matplotlib" 2>/dev/null; then
    python.exe plot.py results/results.csv results
else
    echo "matplotlib not found: run  python plot.py results/results.csv results  where it is installed"
fi

if [ $fail -ne 0 ]; then echo "SOME BENCHMARKS FAILED VERIFICATION"; exit 1; fi
echo "run-all complete: every configuration of every benchmark verified bit-identical."
