/* ============================================================================
 * config.h  --  every tunable constant of the compiler's cost model and of the
 * simulator, in one place so each can be defended (or changed) in the viva.
 *
 * The first block is the course-specified machine model.  The second block is
 * constants this implementation had to add to make the model complete; each
 * carries a one-line justification.
 * ==========================================================================*/
#ifndef NEMOC_CONFIG_H
#define NEMOC_CONFIG_H

/* ---- course-specified constants ------------------------------------------ */
#define HOST_DRAM_ACCESS    100     /* host cycles for a cache miss (DRAM access)  */
#define HOST_CACHE_HIT      4       /* host cycles for a cache hit                 */
#define HOST_ALU            1       /* host cycles per arithmetic operation        */
#define PIM_LOCAL_ACCESS    10      /* DPU cycles, one access to its local bank    */
#define PIM_ALU             2       /* DPU cycles per arithmetic operation         */
#define DMA_PER_BYTE        0.5     /* cycles per byte moved over the memory bus   */
#define NUM_DPUS            64
#define TASKLETS_PER_DPU    16
#define WRAM_BYTES          65536   /* per-DPU scratchpad                          */
#define CACHE_LINE          64
#define MACHINE_BALANCE     0.5     /* flops per byte below which PIM wins         */
#define DRAM_ACCESS_PJ      640     /* energy of one DRAM line access              */
#define PIM_ACCESS_PJ       60      /* energy of one DPU-local access              */
#define ALU_PJ              1       /* energy of one arithmetic operation          */

#define NUM_REGS            24      /* DPU registers available to the allocator    */

/* ---- constants this implementation adds ----------------------------------- */
#define ELEM_BYTES          4       /* int and float are both 32-bit               */
#define HOST_CACHE_BYTES    32768   /* host last-level cache capacity              */
#define HOST_CACHE_WAYS     8       /* associativity of the simulated host cache   */
#define LAUNCH_OVERHEAD     80000   /* host cycles to launch + synchronise a DPU
                                     * program (~27 us at 3 GHz; real systems are
                                     * tens of microseconds)                       */
#define DMA_SETUP           2000    /* host cycles of fixed cost per transfer set  */
#define DMA_RUN_SETUP       300     /* host cycles per non-contiguous run in a
                                     * transfer: each segment is a separate host-side
                                     * transfer call (real systems: microseconds)   */
#define TASKLET_SATURATION  11      /* tasklets needed to fill a DPU pipeline      */
#define NUM_BANKS           8       /* local-memory banks a DPU's tasklets contend
                                     * for                                         */
#define PIM_MUL_CYCLES      16      /* DPUs have no hardware multiplier: a 32-bit
                                     * multiply is a shift-add sequence            */
#define PIM_DIV_CYCLES      32      /* ... and division is worse                   */
#define COHERENCE_OP_CYCLES 50      /* fixed host cost of one flush / invalidate     */
#define INVALIDATE_PER_LINE 2       /* host cycles per cache line an invalidate drops */
#define LOOP_OVERHEAD_OPS   1       /* ALU operations of loop control per iteration  */
#define REUSE_SIGNIFICANCE  0.10    /* a self-temporal reference only blocks
                                     * offload if it carries at least this share of
                                     * the nest's memory traffic                   */

#endif /* NEMOC_CONFIG_H */
