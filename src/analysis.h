/* ============================================================================
 * analysis.h  --  results of the loop analyses (Phases D and E)
 *
 * One NestAnalysis describes one top-level loop nest of a kernel: every array
 * and scalar reference in it (with its affine access matrix), every dependence
 * between references, the reductions it contains, and, per loop, whether that
 * loop may run its iterations in parallel.
 * ==========================================================================*/
#ifndef NEMOC_ANALYSIS_H
#define NEMOC_ANALYSIS_H

#include <cstdio>
#include <map>
#include <string>
#include <vector>

#include "ir.h"

enum DepKind { DEP_FLOW, DEP_ANTI, DEP_OUTPUT };
enum Dir     { DIR_LT, DIR_EQ, DIR_GT };            /* source iteration <, =, > sink */
enum ParKind { PAR_NO, PAR_REDUCTION, PAR_YES };
enum RedKind { RED_SUM, RED_PROD, RED_MAX, RED_MIN, RED_HIST };

/* ---- one memory reference (array element or global scalar) --------------- */
struct RefInfo {
	IRStmt                  *stmt;
	bool                     is_write;
	bool                     is_scalar;
	std::string              name;          /* array or scalar name                   */
	std::vector<LoopNest*>   loops;         /* enclosing loops in the nest, outermost first */
	std::vector<std::vector<long> > F;      /* access matrix: F[dim][loop column]     */
	std::vector<long>        c;             /* offset vector, one entry per dimension */
	bool                     affine;        /* every subscript affine in the loop indices */
	int                      order;         /* execution order within one iteration   */
	int                      red_group;     /* index into reductions, or -1           */
};

/* ---- a recognised reduction ---------------------------------------------- */
struct Reduction {
	RedKind     kind;
	std::string var;                        /* the scalar / array being reduced       */
	bool        is_array;
	bool        is_float;                   /* a float sum/product is not associative */
	IRStmt     *anchor;
};

/* ---- a dependence between two references --------------------------------- */
struct Dep {
	int                     src, dst;       /* indices into NestAnalysis::refs        */
	IRStmt                 *src_stmt;
	IRStmt                 *dst_stmt;
	DepKind                 kind;
	std::string             var;
	std::vector<LoopNest*>  common;         /* loops enclosing both references        */
	std::vector<int>        dir;            /* one Dir per common loop                */
	int                     level;          /* 0 = loop independent; k = carried by common[k-1] */
	bool                    reduction;      /* both ends belong to the same reduction */
};

/* the two tests' separate verdicts on one pair of references ------------------ */
struct PairVerdict {
	int         a, b;                       /* ref indices                            */
	bool        gcd_maybe;                  /* false: GCD proves independence         */
	bool        banerjee_maybe;             /* false: Banerjee proves independence    */
	bool        nonaffine;                  /* tests not applicable: assumed dependent*/
};

struct NestAnalysis {
	Kernel                      *kernel;
	LoopNest                    *root;
	std::vector<LoopNest*>       loops;     /* every loop of the nest, pre-order      */
	std::vector<RefInfo>         refs;
	std::vector<Reduction>       reductions;
	std::vector<Dep>             deps;
	std::vector<PairVerdict>     pairs;
	std::map<LoopNest*, int>     par;       /* ParKind per loop                       */
	std::map<std::string, IRStmt*> defs;    /* single-definition temps of the kernel  */
	bool                         analysable;
	std::string                  why_not;   /* set when !analysable                   */
};

/* trip count of a loop (0 if it never runs) */
long loop_trips(LoopNest *l);

/* pass_affine.cpp: build refs[] with F and c; fills Subscript::affine in the IR */
void affine_analyse(IRProgram *p, Kernel *k, LoopNest *root, NestAnalysis *out);

/* pass_dependence.cpp */
void dependence_analyse(IRProgram *p, NestAnalysis *na);
void dependence_print(NestAnalysis *na, int verbose_pairs);
void dependence_emit_dot(NestAnalysis *na, FILE *out);
int  gcd_long(long a, long b);
/* the two tests, exposed for unit testing: single-variable form
 * A[a*i + c1] (write) vs A[b*i + c2] (read) over i in [lo, lo+step*(n-1)] */
bool gcd_test_single(long a, long b, long c1, long c2);
bool banerjee_test_single(long a, long b, long c1, long c2, long lo, long step, long trips);

/* one call: affine analysis + dependence analysis for a nest */
void analyse_nest(IRProgram *p, Kernel *k, LoopNest *root, NestAnalysis *out);

const char *dir_char(int d);
const char *par_text(int p);
const char *red_text(int k);

#endif /* NEMOC_ANALYSIS_H */
