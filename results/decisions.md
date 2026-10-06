# Offload decisions

One row per loop nest, produced by `./nemoc <benchmark> --report-decisions`.
AI is arithmetic intensity (flops per byte moved) against a machine balance of 0.50.
Reuse lists the arrays with significant self-temporal reuse.

| benchmark | nest | AI | reuse | class | verdict | reason |
|---|---|---|---|---|---|---|
| vecadd | vecadd | 0.08 | none | ELEMENTWISE | OFFLOAD | AI=0.08 <  0.50; no temporal reuse; parallel(dir=[=]); class=ELEMENTWISE |
| saxpy | saxpy | 0.17 | none | ELEMENTWISE | OFFLOAD | AI=0.17 <  0.50; no temporal reuse; parallel(dir=[=]); class=ELEMENTWISE |
| dotprod | dotprod | 0.25 | none | REDUCTION | OFFLOAD | AI=0.25 <  0.50; no temporal reuse; reduction-parallel(dir=[R]); class=REDUCTION |
| maxreduce | maxreduce | 0.00 | none | REDUCTION | OFFLOAD | AI=0.00 <  0.50; no temporal reuse; reduction-parallel(dir=[R]); class=REDUCTION |
| histogram | histogram | 0.24 | none | REDUCTION | OFFLOAD | AI=0.24 <  0.50; no temporal reuse; reduction-parallel(dir=[R]); class=REDUCTION |
| gemv | gemv | 0.49 | negligible | ELEMENTWISE | OFFLOAD | AI=0.49 <  0.50; negligible temporal reuse (y,x); parallel(dir=[=,R]); class=ELEMENTWISE |
| stencil1d | stencil1d | 0.37 | none (+ neighbour reuse) | STENCIL | OFFLOAD | AI=0.37 <  0.50; no temporal reuse; parallel(dir=[=]); class=STENCIL |
| stencil2d | stencil2d | 0.49 | none (+ neighbour reuse) | STENCIL | OFFLOAD | AI=0.49 <  0.50; no temporal reuse; parallel(dir=[=,=]); class=STENCIL |
| filter | filter | 0.00 | none | ELEMENTWISE | OFFLOAD | AI=0.00 <  0.50; no temporal reuse; parallel(dir=[=]); class=ELEMENTWISE |
| fusion_chain | chain | 0.12 | none | ELEMENTWISE | OFFLOAD | AI=0.12 <  0.50; no temporal reuse; parallel(dir=[=]); class=ELEMENTWISE |
| mixed_loop | mixed#1 | 0.12 | none | ELEMENTWISE | OFFLOAD | AI=0.12 <  0.50; no temporal reuse; parallel(dir=[=]); class=ELEMENTWISE |
| mixed_loop | mixed#2 | 0.08 | none (+ neighbour reuse) | IRREGULAR | REJECT | AI=0.08 <  0.50; no temporal reuse; NOT parallel(dir=[<]); class=IRREGULAR |
| colmajor | colmajor | 0.12 | none | ELEMENTWISE | OFFLOAD | AI=0.12 <  0.50; no temporal reuse; parallel(dir=[=,=]); class=ELEMENTWISE |
| matmul | matmul | 2.00 | C,A,B | COMPUTE_BOUND | REJECT | AI=2.00 >= 0.50; self-temporal reuse on C,A,B; parallel(dir=[=,=,R]); class=COMPUTE_BOUND |
| transpose | transpose | 0.00 | none | ELEMENTWISE | REJECT | AI=0.00 <  0.50; no temporal reuse; parallel(dir=[=,=]); class=ELEMENTWISE; partitioning impossible: write B[j,i]: its row subscript does not follow loop i, so one iteration writes rows owned by other DPUs |
| indirect | gather | n/a | n/a | IRREGULAR | REJECT | reference to A has a non-affine (data-dependent) subscript; class=IRREGULAR |
