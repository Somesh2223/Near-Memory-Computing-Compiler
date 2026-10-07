# Running and demoing NEMO-C

## 0. One-time setup (do this the day before, not in front of your teacher)

`nemoc` is a Linux program, so everything runs inside **WSL Ubuntu**. Open a Windows
terminal (PowerShell) and type `wsl`, then:

```bash
cd "/mnt/c/Users/somes/OneDrive/Documents/Somesh Docs/Projects/nemoc"
make            # builds ./nemoc  (about a minute the first time)
```

If `make` complains about a missing tool: `sudo apt install g++ make flex bison`.
Optional, only to *draw* the Graphviz files: `sudo apt install graphviz`.

## 1. Is everything working? (a 10-minute self-test)

```bash
make test       # 8 test programs, 359 checks; ends with  ALL TEST PROGRAMS PASSED
make run-all    # simulates all 15 benchmarks; ends with  run-all complete ... verified
```

`make test` prints some `error:` lines -- those are the *negative* tests (bad input
that must be rejected). Only the final line matters. Both take several minutes: run them
once the day before and leave the results in `results/`.

Quick sanity check, 5 seconds:

```bash
./nemoc benchmarks/matmul.nmc --decisions     # must say REJECT
./nemoc benchmarks/saxpy.nmc  --decisions     # must say OFFLOAD
```

## 2. The demo script (about 15 minutes)

Copy-paste each block; the comment says what to point at.

**A. Front end** -- lexer, parser, error recovery, semantic checks
```bash
./nemoc benchmarks/saxpy.nmc --dump tokens | head -25    # tokens with line:col
./nemoc benchmarks/saxpy.nmc --dump ast | head -40       # precedence is visible in the tree
./nemoc tests/bad_syntax.nmc --dump ast                  # two errors, parser recovers after the first
./nemoc tests/bad_semantics.nmc --check                  # nine errors in one run
grep -c conflict build/parser.output                     # 0 -> the grammar has no conflicts
```

**B. IR and control-flow graph**
```bash
./nemoc benchmarks/saxpy.nmc --dump ir                   # three-address code, loops stay nested
mkdir -p build/dot && ./nemoc benchmarks/saxpy.nmc --emit-dot cfg
cat build/dot/main.cfg.dot                               # if/else merge block + loop back edge
```
(With graphviz: `dot -Tpng build/dot/main.cfg.dot -o cfg.png`, or paste the file into
any online Graphviz viewer.)

**C. Classical optimisations** -- each pass shows IR before, analysis, IR after
```bash
./nemoc tests/opt_demo.nmc --dump constfold      # n = 3*4 becomes 12, a branch disappears
./nemoc tests/opt_demo.nmc --dump cse            # A[i] loaded once instead of three times
./nemoc tests/opt_demo.nmc --dump dce            # live-in / live-out sets per block
./nemoc tests/opt_demo.nmc --dump indvar         # induction variable table
./nemoc tests/opt_demo.nmc --dump strength       # i*4 becomes an additive update
```

**D. Dependence analysis** -- the GCD / Banerjee contrast
```bash
./nemoc tests/dep_banerjee.nmc --dump deps
```
Point at the three kernels: *GCD proves independence and Banerjee cannot*, *GCD cannot
and Banerjee proves it* (A[i] vs A[i+100]), and a *real* flow dependence with direction `(<)`.

**E. The offload decision** -- the heart of the project
```bash
for b in vecadd dotprod gemv stencil1d matmul transpose indirect; do ./nemoc benchmarks/$b.nmc --decisions | tail -1; done
./nemoc benchmarks/matmul.nmc    --explain matmul      # plain-English trace, REJECT
./nemoc benchmarks/saxpy.nmc     --explain saxpy       # same steps, OFFLOAD
./nemoc benchmarks/transpose.nmc --explain transpose   # fine on paper, but partitioning needs all-to-all
```

**F. Enabling transformations**
```bash
./nemoc benchmarks/mixed_loop.nmc   --transformations   # fission: splits an offloadable and a serial statement
./nemoc benchmarks/fusion_chain.nmc --transformations   # fusion: intermediate never leaves PIM, bytes saved
./nemoc benchmarks/colmajor.nmc     --transformations   # interchange: 2 MB -> 128 KB of traffic
```

**G. Partitioning and coherence**
```bash
./nemoc benchmarks/stencil1d.nmc --partition         # halo of 1 row each side; block vs cyclic vs block-cyclic costed
./nemoc tests/coherence_demo.nmc --coherence         # dataflow table; note the if.merge block
```
The written explanation is `docs/coherence_analysis.md`.

**H. Code generation and register allocation**
```bash
./nemoc benchmarks/stencil1d.nmc --codegen
cat build/stencil1d_dpu.pim            # the tiling arithmetic is the comment at the top
cat build/stencil1d_host.c             # flush, partition setup, DMA, launch, DMA out
./nemoc benchmarks/stencil2d.nmc --dump regalloc --regs 5     # forces real spills
./nemoc benchmarks/stencil2d.nmc --emit-dot interference
```

**I. The result** -- simulation and charts
```bash
./nemoc benchmarks/matmul.nmc    --simulate     # NAIVE slower, NEMO_C = HOST_ONLY
./nemoc benchmarks/transpose.nmc --simulate
./nemoc benchmarks/indirect.nmc  --simulate
./nemoc benchmarks/saxpy.nmc     --simulate     # offloading pays
```
Then open `results/cycles.png` (Windows Explorer can open it from the project folder) and
`results/decisions.md`. Every `--simulate` ends with *bit-identical* checks: the host run,
the PIM run, **and the generated DPU code actually executed**.

## 3. Show that it is not canned -- edit a program live

Teachers like this. Copy a benchmark and change it:

```bash
cp benchmarks/saxpy.nmc /tmp/mine.nmc
# edit /tmp/mine.nmc: e.g. add a second statement  D[i] = D[i-1] + A[i];  (declare array float D[16384];)
./nemoc /tmp/mine.nmc --decisions        # the nest is now serial -> REJECT with the reason
./nemoc /tmp/mine.nmc --transformations  # ...and fission splits it so saxpy still offloads
```
Other quick experiments: change matmul's size from 16 to 8 or 64 and watch the
arithmetic intensity move; change `LAUNCH_OVERHEAD` in `src/config.h`, `make`, and
re-run `--simulate` to see small kernels stop paying off.

## 4. Questions to expect (and where the answer is)

| question | answer lives in |
|---|---|
| Why is your grammar conflict-free? What about dangling else? | top comment of `src/parser.y` |
| Why does the IR keep loops nested instead of a flat CFG? | top comment of `src/ir.h` |
| GCD vs Banerjee -- what does each one use? | top comment of `src/pass_dependence.cpp` |
| Why is matmul rejected although it is parallel? | `--explain matmul`; `src/pass_profitability.cpp` |
| Why is gemv accepted although x and y are reused? | README, "Deviations", item 2 |
| What is the owner-computes rule? Why does transpose fail it? | `src/pass_partition.cpp` |
| What is the meet at the if/else merge? | `docs/coherence_analysis.md` |
| Walk me through Chaitin-Briggs. | top comment of `src/regalloc.cpp` |
| How do you know the generated code is right? | `src/dpu_interp.cpp`, the check line of `--simulate` |
| Why does NAIVE lose? Is the model tuned? | `docs/simulator_model.md` (also lists its limits honestly) |

## 5. If something goes wrong in front of the teacher

* `./nemoc: cannot execute binary file` -- you are in Git Bash / PowerShell, not WSL. Type `wsl` first.
* `cannot write 'build/dot/...'` -- run `mkdir -p build/dot`.
* A command looks stuck -- `--simulate` takes 5-20 seconds per benchmark. `make test` takes minutes.
* Anything weird after editing code -- `make clean && make`.
* Pre-made output is always in `results/` (CSV, charts, decisions, transformations) and
  `docs/graphs/`, so you can show those even if a live command misbehaves.
