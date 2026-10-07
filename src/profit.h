/* ============================================================================
 * profit.h  --  the offload decision record (Phase E)
 * ==========================================================================*/
#ifndef NEMOC_PROFIT_H
#define NEMOC_PROFIT_H

#include <string>
#include <vector>

#include "ir.h"
#include "analysis.h"

enum ParClass {
	PC_ELEMENTWISE,
	PC_REDUCTION,
	PC_STENCIL,
	PC_COMPUTE_BOUND,
	PC_IRREGULAR
};

/* memory traffic and reuse of one stream of references (same array, same
 * access matrix, same direction) */
struct RefGroupCost {
	std::string array;
	bool        is_write;
	bool        nonaffine;
	long        footprint;        /* distinct elements touched                  */
	long        stride_bytes;     /* byte distance between consecutive accesses
	                               * of the innermost loop the reference varies in */
	double      bytes;            /* bytes moved over the memory bus            */
	bool        temporal;         /* some loop never changes the element        */
	long        reuse_factor;     /* how many times each element is revisited   */
	bool        significant;      /* carries >= REUSE_SIGNIFICANCE of the traffic */
	bool        spatial;          /* innermost index only in the last dim, coeff 1 */
	std::string text;             /* e.g. "A[i, k]"                             */
};

struct Decision {
	std::string  name;            /* kernel name, or "kernel#n" after fission   */
	double       flops;
	double       bytes;
	double       ai;              /* arithmetic intensity, flops per byte       */
	double       host_est;        /* estimated host cycles: line fills + flops  */
	double       pim_fixed;       /* PIM cost that exists even for no work:
	                               * launch + moving the bytes over the bus     */
	std::vector<RefGroupCost> groups;
	std::vector<std::string>  temporal;        /* any self-temporal reuse       */
	std::vector<std::string>  temporal_sig;    /* ... that actually matters     */
	std::vector<std::string>  spatial;
	std::vector<std::string>  group_temporal;  /* neighbouring-element reuse    */
	bool         analysable;
	std::string  why_not;
	int          pclass;
	LoopNest    *par_loop;       /* outermost parallel loop (0 if none)         */
	int          par_kind;       /* PAR_YES / PAR_REDUCTION                     */
	bool         par_outermost;  /* is par_loop the root of the nest?           */
	std::string  par_dirs;       /* e.g. "=,=,R" -- one char per loop           */
	bool         float_reduction;
	bool         offload;        /* the decision                                */
	std::vector<std::string> fails;   /* every clause of the rule that failed    */
	std::string  reason;         /* one-line explanation                        */
};

const char *class_text(int c);

void profitability(IRProgram *p, NestAnalysis *na, const std::string &name, Decision *d);

/* "matmul    REJECT   AI=2.00 >= 0.50; self-temporal reuse on A,B; class=COMPUTE_BOUND" */
std::string decision_line(const Decision *d);

/* the complete step-by-step trace printed by --explain <kernel> */
void explain_decision(IRProgram *p, NestAnalysis *na, const Decision *d);

#endif /* NEMOC_PROFIT_H */
