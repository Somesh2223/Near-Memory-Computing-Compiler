# The machine model: what the simulator measures and why

Every constant is in [`src/config.h`](../src/config.h). The first block of that
file is the course-specified model; the second block lists the constants this
implementation had to *add* to make the model complete, each with a one-line
justification. This page explains how they combine, because the viva will ask
"why does NAIVE_OFFLOAD lose on matmul?" and the answer has to come from the
model, not from tuning.

## What is simulated

The simulator does two jobs at once.

1. **Functional execution.** An interpreter runs the IR. Host code runs against
   the global arrays. An offloaded nest runs the way the generated code would:
   the iteration space is cut across the DPUs by the partition plan, and each
   DPU gets a *private image* of the arrays in which only the rows the compiler
   said it needs are resident (its rows, the halo, broadcast arrays). Reading
   any other element is an error, so a wrong partition or halo is caught.
   Writes are tracked per element and copied back; two DPUs writing the same
   element is reported as a race; reduction accumulators are private and merged.
   The result is compared **bit for bit** with a pure host execution. A fourth
   run executes the *generated DPU programs* (after register allocation, with
   24 physical registers and spills) and must agree as well.

2. **Cost accounting.** Cycles, bytes crossing the memory bus, and energy.

## Host cost

A set-associative LRU cache (`HOST_CACHE_BYTES` = 32 KiB, 8-way, 64-byte lines)
sees every array access. A hit costs `HOST_CACHE_HIT` (4) cycles, a miss
`HOST_DRAM_ACCESS` (100). Each line fill and each write-back of a dirty line puts
`CACHE_LINE` bytes on the bus and costs `DRAM_ACCESS_PJ` (640 pJ). Arithmetic
costs `HOST_ALU` (1) per operation; scalars and temporaries live in registers.
Loop control costs one ALU operation per iteration.

## PIM cost of one offloaded nest

```
cycles = LAUNCH_OVERHEAD                       fixed: launch + synchronise
       + DMA_in  + DMA_out                     bytes * DMA_PER_BYTE
                                              + runs  * DMA_RUN_SETUP
                                              + sets  * DMA_SETUP
       + max over DPUs of DPU_cycles           the slowest DPU
       + merge                                 host-side combining of private copies
```

* **DMA** moves each array's rows over the memory bus (so the bytes count as bus
  traffic). A *run* is a maximal contiguous segment of a transfer: block
  partitions of row-major arrays are one run per DPU per array, a transposed
  write is one run per row per DPU. A *set* is one parallel transfer of one array
  to all DPUs.
* **DPU cycles** = `alu * PIM_ALU + mul * PIM_MUL_CYCLES + div * PIM_DIV_CYCLES`
  plus the local-memory term. DPUs have no hardware multiplier, so a multiply is
  a shift-add sequence (`PIM_MUL_CYCLES`). A local access takes
  `PIM_LOCAL_ACCESS` cycles, but the DPU's tasklets overlap those latencies: the
  effective cost is `PIM_LOCAL_ACCESS / min(TASKLETS_PER_DPU, TASKLET_SATURATION)`
  per access (the pipeline saturates at 11 tasklets). Tasklets contending for the
  same bank serialise: an access whose stride across the iterations is a multiple
  of `NUM_BANKS` words is multiplied by `gcd(stride, NUM_BANKS)`.
* **Energy** = each 64-byte DMA burst costs a DRAM access plus a bank access
  (`DRAM_ACCESS_PJ + PIM_ACCESS_PJ`), and the DPU's scratchpad accesses and
  arithmetic cost `ALU_PJ` each.

## Why the three rejections come out as they do

* **matmul** (16 x 16). The data fits the host cache, so the host runs at cache
  speed: about 87 000 cycles. Offloading pays `LAUNCH_OVERHEAD` (80 000), the DMA
  of `A`, `C` and **B broadcast to every DPU**, and the DPUs' slow multiplies.
  It loses because there is almost nothing for the memory system to save: the
  working set is cached, and PIM only helps when data movement dominates.
* **transpose** (128 x 128). Partitioning on the row loop makes every DPU write
  one *column block* of `B`. Its output is 128 separate segments per DPU, 8 192
  runs in total at `DMA_RUN_SETUP` each. That is the "all-to-all shuffle" of the
  analysis, priced.
* **indirect** (`C[i] = A[B[i]]`). `A` is read at data-dependent positions, so
  every DPU needs all of it: 64 copies of a 64 KiB array cross the bus
  (4.4 MB, against the host's 0.7 MB).

NEMO_C declines all three, so it matches HOST_ONLY exactly; the NAIVE_OFFLOAD
column in `results/` is what offloading them costs.

## Honest limits of the model

* **Data is copied in and out on every launch** (the UPMEM-style explicit-DMA
  model the brief describes). A single streaming pass therefore moves about the
  same number of bytes as the host would, so energy for streaming kernels is
  roughly a wash; the energy wins in `results/energy.png` come from avoiding
  *low-locality* host access (colmajor, where the host drags a full cache line
  per element) and from fusion (the intermediate never crosses the bus).
* **The cost model optimises time, not energy.** The three rejected nests are slower
  under NAIVE_OFFLOAD, but `transpose` under NAIVE_OFFLOAD uses *less energy* than the
  host (a strided host transpose drags a 64-byte line, 640 pJ, per element; the DPUs
  move the same bytes in bursts). The energy chart shows this as measured rather than
  hiding it.
* The host is a single in-order core with a blocking cache; there is no
  memory-level parallelism or prefetching. This flatters PIM somewhat on
  streaming kernels and is the same simplification for every benchmark.
* `LAUNCH_OVERHEAD` and `DMA_RUN_SETUP` are the two constants with the largest
  influence on the small kernels. They are in `config.h`, and the unit test
  `tests/test_equiv.cpp` states which orderings must hold.
