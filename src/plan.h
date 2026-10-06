/* ============================================================================
 * plan.h  --  the compiler's decisions, gathered per kernel and per nest
 *
 * A NestPlan says, for one top-level loop nest of one kernel: what the
 * analyses found (NestAnalysis), what the cost model decided (Decision), how
 * the iteration space is cut across the DPUs (PartitionPlan), and whether the
 * nest finally runs on the host or on PIM.  A ProgramPlan collects them for the
 * whole program together with the log of enabling transformations.
 * ==========================================================================*/
#ifndef NEMOC_PLAN_H
#define NEMOC_PLAN_H

#include <set>
#include <string>
#include <vector>

#include "ir.h"
#include "analysis.h"
#include "profit.h"

enum DistKind { DIST_BLOCK, DIST_CYCLIC, DIST_BLOCK_CYCLIC };

enum ArrayMode {
	AM_PARTITIONED,   /* rows follow the partitioned loop (plus a halo)        */
	AM_BROADCAST,     /* every DPU gets the whole array (read-only)            */
	AM_PRIVATE,       /* every DPU has a private accumulator, merged after     */
	AM_SCATTER        /* (naive offload only) written rows follow no partition */
};

struct ArrayRole {
	std::string name;
	bool        read;
	bool        written;
	int         mode;
	long        row_off;         /* element row = coeff * iteration + row_off    */
	long        row_coeff;       /*   (AM_PARTITIONED; 1 in practice)            */
	long        halo_lo;         /* extra rows needed below the owned rows       */
	long        halo_hi;         /* extra rows needed above them                 */
	bool        local_only;      /* dead outside a fused nest: never leaves PIM  */
	int         red_kind;        /* AM_PRIVATE: RED_SUM / RED_PROD / RED_MAX ... */
};

struct DistCost {
	int    kind;
	long   block;                /* chunk size for DIST_BLOCK_CYCLIC             */
	double halo_bytes;           /* communication beyond the owned rows          */
	long   runs;                 /* non-contiguous transfer segments             */
	double score;                /* halo_bytes + runs * (DMA_RUN_SETUP/DMA_PER_BYTE) */
};

struct PartitionPlan {
	bool        feasible;
	std::string reason;          /* why not, when !feasible; or the summary      */
	LoopNest   *loop;            /* the partitioned loop (the root)              */
	int         dist;            /* chosen distribution                          */
	long        block;           /* chunk size (block-cyclic)                    */
	int         ndpus;           /* DPUs actually used                           */
	long        trips;
	std::vector<ArrayRole> arrays;
	std::vector<DistCost>  considered;
	std::vector<std::string> notes;     /* per-reference classification, human readable */
	bool        privatised;      /* some accumulator is merged across DPUs       */
};

struct NestPlan {
	std::string   name;
	LoopNest     *root;
	NestAnalysis  na;
	Decision      dec;
	PartitionPlan part;
	bool          offload;       /* final: profitable AND partitionable          */
	bool          forced;        /* offloaded by NAIVE even though infeasible    */
	std::vector<std::string> notes;
	int           fused_count;   /* how many original nests were fused into this  */
};

struct KernelPlan {
	Kernel                 *kernel;
	std::vector<NestPlan*>  nests;
};

struct TransformRecord {
	std::string kind;            /* "fission", "interchange", "fusion"           */
	std::string kernel;
	std::string what;            /* what was done                                */
	std::string gain;            /* what it bought                               */
	double      bytes_saved;
};

struct ProgramPlan {
	IRProgram                    *prog;
	std::vector<KernelPlan>       kernels;
	std::vector<TransformRecord>  transforms;
	std::set<std::string>         dead_arrays;   /* intermediates that never leave PIM */
	bool                          nemo;          /* built by the cost-model-guided compiler */
};

/* pass_partition.cpp */
/* naive = true: never fail; replicate what cannot be partitioned (NAIVE_OFFLOAD) */
bool partition_nest(IRProgram *p, NestAnalysis *na, const Decision *d, bool naive,
                    PartitionPlan *out);
void partition_print(const PartitionPlan *pp);
const char *dist_text(int d);
/* iteration counters (0 .. trips-1) that DPU `dpu` executes */
void partition_iterations(const PartitionPlan *pp, int dpu, std::vector<long> *out);
extern int g_force_dist;      /* -1: choose by cost; else a DistKind (tests, --dist) */

/* plan.cpp -- build / transform / partition */
ProgramPlan *plan_program(IRProgram *p, bool nemo);
NestPlan    *make_nest_plan(IRProgram *p, Kernel *k, LoopNest *root, const std::string &name);
void         print_decisions(ProgramPlan *pp);
void         print_transformations(ProgramPlan *pp);
NestPlan    *find_nest_plan(ProgramPlan *pp, const std::string &name);

/* pass_coherence.cpp -- places flush/invalidate statements; verbose prints the
 * dataflow iteration table */
void coherence_run(ProgramPlan *pp, int verbose);

/* pass_fission.cpp / pass_interchange.cpp / pass_fusion.cpp */
int fission_kernel(ProgramPlan *pp, Kernel *k);
int interchange_kernel(ProgramPlan *pp, Kernel *k);
int fusion_kernel(ProgramPlan *pp, Kernel *k);

#endif /* NEMOC_PLAN_H */
