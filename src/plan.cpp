/* ============================================================================
 * plan.cpp  --  turning analyses into per-nest decisions
 *
 * plan_program(p, nemo = true) is the cost-model-guided compiler (NEMO_C):
 * for every kernel it runs the enabling transformations of Phase F (fission,
 * interchange, then fusion), re-running the profitability analysis after each
 * one, and then decides every remaining nest by the Phase E rule combined with
 * the Phase G partitioning feasibility check.
 *
 * plan_program(p, nemo = false) is the NAIVE_OFFLOAD baseline: no
 * transformations, no cost model -- every nest whose outermost loop is
 * parallel is offloaded, replicating or scattering whatever it cannot
 * partition cleanly.  The simulator uses it to show what ignoring the cost
 * model costs.
 * ==========================================================================*/
#include <cstdio>
#include <string>
#include <vector>

#include "ir.h"
#include "analysis.h"
#include "profit.h"
#include "plan.h"

NestPlan *make_nest_plan(IRProgram *p, Kernel *k, LoopNest *root, const std::string &name)
{
	NestPlan *np = new NestPlan();
	np->name = name;
	np->root = root;
	np->offload = false;
	np->forced = false;
	np->fused_count = 1;
	np->part.feasible = false;

	analyse_nest(p, k, root, &np->na);
	profitability(p, &np->na, name, &np->dec);

	if (np->dec.offload) {
		bool ok = partition_nest(p, &np->na, &np->dec, false, &np->part);
		if (ok) {
			np->offload = true;
		} else {
			np->dec.offload = false;
			np->dec.fails.push_back("partitioning impossible: " + np->part.reason);
			np->dec.reason += "; partitioning impossible: " + np->part.reason;
		}
	}
	return np;
}

static void naive_nest(IRProgram *p, NestPlan *np)
{
	/* NAIVE_OFFLOAD: offload every nest whose outermost loop is parallel */
	if (np->na.par[np->root] != PAR_NO) {
		partition_nest(p, &np->na, &np->dec, true, &np->part);
		np->offload = np->part.feasible;
		np->forced = np->offload && !np->dec.offload;
	}
}

static std::string nest_name(Kernel *k, int idx, int total)
{
	if (total == 1)
		return k->name;
	char b[16];
	snprintf(b, sizeof b, "#%d", idx + 1);
	return k->name + b;
}

ProgramPlan *plan_program(IRProgram *p, bool nemo)
{
	ProgramPlan *pp = new ProgramPlan();
	size_t ki, i;

	pp->prog = p;
	pp->nemo = nemo;

	for (ki = 0; ki < p->kernels.size(); ki++) {
		Kernel *k = p->kernels[ki];

		if (nemo) {
			int round;
			for (round = 0; round < 4; round++) {
				int changed = 0;
				changed += fission_kernel(pp, k);
				changed += interchange_kernel(pp, k);
				if (!changed)
					break;
			}
			fusion_kernel(pp, k);
		}

		KernelPlan kp;
		kp.kernel = k;
		int total = 0;
		for (i = 0; i < k->body.size(); i++)
			if (k->body[i].kind == BK_LOOP)
				total++;
		int idx = 0;
		for (i = 0; i < k->body.size(); i++) {
			if (k->body[i].kind != BK_LOOP)
				continue;
			NestPlan *np = make_nest_plan(p, k, k->body[i].loop, nest_name(k, idx, total));
			if (!nemo)
				naive_nest(p, np);
			kp.nests.push_back(np);
			idx++;
		}
		pp->kernels.push_back(kp);
	}

	/* intermediates fusion proved dead never leave PIM */
	for (ki = 0; ki < pp->kernels.size(); ki++)
		for (i = 0; i < pp->kernels[ki].nests.size(); i++) {
			NestPlan *np = pp->kernels[ki].nests[i];
			size_t a;
			for (a = 0; a < np->part.arrays.size(); a++)
				if (pp->dead_arrays.count(np->part.arrays[a].name))
					np->part.arrays[a].local_only = true;
		}
	return pp;
}

NestPlan *find_nest_plan(ProgramPlan *pp, const std::string &name)
{
	size_t k, n;
	for (k = 0; k < pp->kernels.size(); k++)
		for (n = 0; n < pp->kernels[k].nests.size(); n++)
			if (pp->kernels[k].nests[n]->name == name)
				return pp->kernels[k].nests[n];
	/* a plain kernel name matches its first nest */
	for (k = 0; k < pp->kernels.size(); k++)
		if (pp->kernels[k].kernel->name == name && !pp->kernels[k].nests.empty())
			return pp->kernels[k].nests[0];
	return 0;
}

void print_decisions(ProgramPlan *pp)
{
	size_t k, n;
	printf("%-12s %-8s %s\n", "nest", "verdict", "reason");
	for (k = 0; k < pp->kernels.size(); k++)
		for (n = 0; n < pp->kernels[k].nests.size(); n++) {
			NestPlan *np = pp->kernels[k].nests[n];
			std::string line = decision_line(&np->dec);
			if (np->forced)
				line += "   [forced on by the naive offloader]";
			printf("%s\n", line.c_str());
		}
}

void print_transformations(ProgramPlan *pp)
{
	size_t i;
	if (pp->transforms.empty()) {
		printf("no enabling transformation was needed\n");
		return;
	}
	for (i = 0; i < pp->transforms.size(); i++) {
		TransformRecord &t = pp->transforms[i];
		printf("%-12s %-12s %s\n", t.kind.c_str(), t.kernel.c_str(), t.what.c_str());
		printf("%-12s %-12s => %s\n", "", "", t.gain.c_str());
	}
}
