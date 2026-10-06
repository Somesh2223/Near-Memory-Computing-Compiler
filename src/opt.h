/* ============================================================================
 * opt.h  --  the classical optimisations (Phase C)
 *
 * Each pass is its own file and is individually callable.  `verbose` makes a
 * pass print the analysis it computed (lattice values, live sets, induction
 * variable table) -- the `--dump <pass>` flag in main.cpp prints the IR before
 * and after and turns this on.  Every pass returns the number of changes it
 * made, so the pipeline (and the unit tests) can tell whether it did anything.
 * ==========================================================================*/
#ifndef NEMOC_OPT_H
#define NEMOC_OPT_H

#include <string>
#include <vector>
#include "ir.h"

/* opt_constfold.cpp -- forward dataflow: constant propagation + folding */
int opt_constfold(IRProgram *p, int verbose);

/* opt_cse.cpp -- local available-expressions CSE (+ block-local copy propagation) */
int opt_cse(IRProgram *p, int verbose);

/* opt_dce.cpp -- backward liveness analysis, dead-code elimination */
int opt_dce(IRProgram *p, int verbose);

/* opt_indvar.cpp -- basic and derived induction variables */
struct DerivedIV {
	std::string name;     /* the temp, e.g. "%t3"                     */
	long        coeff;    /* name == coeff * basic + offset           */
	long        offset;
	IRStmt     *stmt;     /* the defining statement                   */
	bool        via_mul;  /* defined with a multiplication            */
};

struct LoopIVs {
	LoopNest               *loop;
	std::string             basic;      /* the loop index                      */
	long                    init;
	long                    step;
	std::vector<DerivedIV>  derived;
};

void indvar_analyse_loop(LoopNest *l, LoopIVs *out);      /* one loop */
void opt_indvar_analyse(IRProgram *p, std::vector<LoopIVs> *out);   /* every loop */
int  opt_indvar(IRProgram *p, int verbose);       /* prints the table; changes nothing */

/* opt_strength.cpp -- multiplication by an induction variable -> additive update */
int opt_strength(IRProgram *p, int verbose);

/* pipeline.cpp -- run the whole classical sequence; `dump` names one pass whose
 * before/after IR is printed ("" = none). Returns total changes. */
int opt_run_all(IRProgram *p, const std::string &dump);

#endif /* NEMOC_OPT_H */
