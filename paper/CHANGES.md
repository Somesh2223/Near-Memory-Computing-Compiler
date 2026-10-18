# What changed from the earlier draft, and why

The earlier draft (NEMO-C_layout-check_NOT-official-ieeeaccess-class.pdf) was rewritten. These were the reasons.

## Facts that did not match the project

| Earlier draft | Actual project |
|---|---|
| stencil1d 1,245,102 / 782,104 cycles; matmul 1,842,100 / 2,924,012; transpose 1,120,400 / 1,600,576; indirect 1,450,200 / 3,372,558 | 770,204 / 460,263; 87,080 / 138,850; 2,048,456 / 2,909,951; 1,196,328 / 2,752,264 (`results/results.csv`). Only vecadd and saxpy were right; all totals built from the others were wrong. |
| Blind offloading moves 3x, 2x, 2.5x the bytes on the rejected kernels | matmul 8.4x, indirect 6.4x, transpose **0.12x** (fewer bytes, yet slower because of 8,192 transfer segments) |
| Five checks, evaluated until the first failure; reuse = fraction of references | Six checks (the size check was missing), all evaluated; reuse counts only when the stream carries at least 10% of the **bytes** |
| matmul rejected by the reuse test | matmul fails AI, reuse and size |
| "No energy results", "cost parameters not given", "no spill counts", "coherence algorithm not specified", "AI per kernel not reported" | All exist: energy in results.csv, parameters in config.h, spills from `--dump regalloc`, the coherence dataflow in pass_coherence.cpp, AI in decisions.md |
| Register allocation coalesces after colouring | The code coalesces before simplify (standard Chaitin–Briggs order) |
| 6 benchmarks | 15, including the fission, interchange and fusion cases |
| "The author" (singular) | Four authors |

## Course template rules that were not met

- References had to be 30 papers from 2018–2024; most of the 36 were from 1972–2017.
- The introduction needed about 15 recent references.
- Related works had to cover 15 recent papers, each with solution, methodology and tools, results and research gap.
- Section 4 lacked the tool screenshot, the parameter table, a graph per metric and the comparison with 2–3 related works.
- Keywords had to be exactly 6, in alphabetical order.
- The reference format did not follow the template.

All of these are now satisfied; `python check_manuscript.py` prints the counts.
