/* ============================================================================
 * pass_fusion.cpp  --  kernel (loop) fusion
 *
 * Compiler-theory concept: loop fusion, legality by the dependence graph
 * (Aho/Lam/Sethi/Ullman 2nd ed., section 11.9.1 "Loop Fusion"; Kennedy &
 * McKinley 1993, "Maximizing Loop Parallelism and Improving Data Locality via
 * Loop Fusion and Distribution").
 *
 * Two ADJACENT loops with identical bounds are merged into one.  Fusion
 * changes the execution order: before, every iteration of loop 1 ran before
 * any iteration of loop 2; afterwards iteration i of loop 2 runs right after
 * iteration i of loop 1.  It is LEGAL iff no dependence is reversed by that
 * change, i.e. there is no dependence from a statement of loop 2 at an earlier
 * fused iteration to a statement of loop 1 at a later one (a "fusion-preventing
 * dependence").  The test builds the fused loop, runs the Phase D analysis on
 * it, and looks for exactly such an edge.
 *
 * Why fusion pays on a near-memory machine: when loop 1 writes an array T that
 * loop 2 reads, unfused execution is two offloads -- T is DMA'd out of the
 * DPUs after loop 1 and back in for loop 2, and each offload pays the launch
 * overhead.  Fused, T stays in the DPU's scratchpad.  If nothing else in the
 * program reads T it is a pure intermediate and never leaves PIM at all; the
 * pass reports the bytes it saves.
 *
 * Scope: the two loops must use the same index variable name.
 * ==========================================================================*/
#include <cstdio>
#include <set>
#include <string>
#include <vector>

#include "config.h"
#include "ir.h"
#include "analysis.h"
#include "profit.h"
#include "plan.h"

static bool same_bounds(LoopNest *a, LoopNest *b)
{
	return a->index_var == b->index_var && a->lower == b->lower && a->upper == b->upper &&
	       a->rel == b->rel && a->step == b->step;
}

/* does any statement outside `inside` read array `name`? */
static bool read_elsewhere(IRProgram *p, const std::string &name, std::set<IRStmt*> &inside)
{
	std::vector<IRStmt*> all;
	size_t i, k;
	for (k = 0; k < p->kernels.size(); k++)
		ir_collect_stmts(p->kernels[k]->body, &all);
	for (k = 0; k < p->hosts.size(); k++)
		ir_collect_stmts(p->hosts[k]->body, &all);
	for (i = 0; i < all.size(); i++)
		if (all[i]->kind == IR_ARRAY_LOAD && all[i]->array == name && !inside.count(all[i]))
			return true;
	return false;
}

int fusion_kernel(ProgramPlan *pp, Kernel *k)
{
	IRProgram *p = pp->prog;
	int changed = 0;
	size_t idx = 0;

	while (idx + 1 < k->body.size()) {
		if (k->body[idx].kind != BK_LOOP || k->body[idx + 1].kind != BK_LOOP) {
			idx++;
			continue;
		}
		LoopNest *l1 = k->body[idx].loop;
		LoopNest *l2 = k->body[idx + 1].loop;
		if (!same_bounds(l1, l2)) {
			idx++;
			continue;
		}

		NestPlan *p1 = make_nest_plan(p, k, l1, k->name);
		NestPlan *p2 = make_nest_plan(p, k, l2, k->name);
		if (!p1->offload || !p2->offload) {
			idx++;
			continue;
		}

		/* build the fused loop */
		LoopNest *fl = new LoopNest(*l1);
		size_t i;
		for (i = 0; i < l2->body.size(); i++)
			fl->body.push_back(l2->body[i]);

		std::set<IRStmt*> s1, s2;
		{
			std::vector<IRStmt*> v;
			ir_collect_stmts(l1->body, &v);
			for (i = 0; i < v.size(); i++) s1.insert(v[i]);
			v.clear();
			ir_collect_stmts(l2->body, &v);
			for (i = 0; i < v.size(); i++) s2.insert(v[i]);
		}

		NestPlan *pf = make_nest_plan(p, k, fl, k->name);

		/* fusion-preventing dependence: loop-2 statement -> loop-1 statement */
		bool legal = true;
		for (i = 0; i < pf->na.deps.size(); i++) {
			Dep &d = pf->na.deps[i];
			if (s2.count(d.src_stmt) && s1.count(d.dst_stmt))
				legal = false;
		}
		if (!legal || !pf->offload) {
			idx++;
			continue;
		}

		/* bytes saved: arrays loop 1 writes (only) and loop 2 reads */
		double saved = 0;
		std::vector<std::string> inter;
		std::set<IRStmt*> both = s1;
		both.insert(s2.begin(), s2.end());
		size_t g;
		for (g = 0; g < p1->dec.groups.size(); g++) {
			RefGroupCost &w = p1->dec.groups[g];
			if (!w.is_write)
				continue;
			bool read_in_2 = false, read_in_1 = false;
			size_t h;
			for (h = 0; h < p2->dec.groups.size(); h++)
				if (!p2->dec.groups[h].is_write && p2->dec.groups[h].array == w.array)
					read_in_2 = true;
			for (h = 0; h < p1->dec.groups.size(); h++)
				if (!p1->dec.groups[h].is_write && p1->dec.groups[h].array == w.array)
					read_in_1 = true;
			if (!read_in_2)
				continue;
			bool dead = !read_in_1 && !read_elsewhere(p, w.array, both);
			saved += w.bytes;                 /* DMA-in of the intermediate disappears */
			if (dead) {
				saved += w.bytes;             /* ... and so does its DMA-out           */
				pp->dead_arrays.insert(w.array);
			}
			inter.push_back(w.array + (dead ? " (never leaves PIM)" : " (stays resident)"));
		}

		/* commit */
		k->body.erase(k->body.begin() + idx, k->body.begin() + idx + 2);
		k->body.insert(k->body.begin() + idx, mk_bi_loop(fl));

		TransformRecord tr;
		tr.kind = "fusion";
		tr.kernel = k->name;
		tr.what = "fused two adjacent loops over `" + l1->index_var +
		          "` (legal: no dependence from loop 2 back to loop 1)";
		char b[320];
		std::string names;
		for (i = 0; i < inter.size(); i++)
			names += (i ? ", " : "") + inter[i];
		snprintf(b, sizeof b, "saves %.0f bytes of DMA traffic and %d DPU launch (%d cycles); intermediate: %s",
		         saved, 1, LAUNCH_OVERHEAD, names.empty() ? "none" : names.c_str());
		tr.gain = b;
		tr.bytes_saved = saved;
		pp->transforms.push_back(tr);
		changed++;
		/* stay at idx: the fused loop may fuse with its new neighbour */
	}
	return changed;
}
