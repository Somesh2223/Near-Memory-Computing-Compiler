# Enabling transformations

Loop distribution (fission), interchange and fusion applied by the cost-model-guided
compiler, each re-checked by the dependence and profitability analyses.

- **fusion_chain** (`chain`) -- fusion: fused two adjacent loops over `i` (legal: no dependence from loop 2 back to loop 1)
  - saves 131072 bytes of DMA traffic and 1 DPU launch (80000 cycles); intermediate: T (never leaves PIM)
- **mixed_loop** (`mixed`) -- fission: split loop `i` (2 statement groups) into 2 loops by SCC of the dependence graph
  - loop 1 {B[i] = %t1}: OFFLOADABLE; loop 2 {C[i] = %t5}: stays on host; the whole nest was previously REJECTED (AI=0.10 <  0.50; no temporal reuse; NOT parallel(dir=[<]); class=IRREGULAR)
- **colmajor** (`colmajor`) -- interchange: loops (j,i) -> (i,j) (legal: every dependence direction vector stays lexicographically non-negative)
  - memory traffic 2097152 B -> 131072 B; nest becomes offloadable
