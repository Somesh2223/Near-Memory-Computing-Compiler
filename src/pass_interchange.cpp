/* ============================================================================
 * pass_interchange.cpp  --  loop interchange (permutation)
 *
 * Compiler-theory concept: unimodular loop permutation, legality tested on
 * dependence direction vectors (Aho/Lam/Sethi/Ullman 2nd ed., section 11.7.1
 * and 11.10; Allen & Kennedy 1984; Wolf & Lam 1991).
 *
 * Reordering the loops of a PERFECT nest (each loop's body is exactly the next
 * loop, the statements sit in the innermost) permutes the entries of every
 * dependence direction vector the same way.  The permutation is LEGAL iff every
 * permuted vector is still lexicographically non-negative -- its first entry
 * that is not '=' is '<' -- because then every dependence's source still
 * executes before its sink.
 *
 * Why interchange here: it can (a) make the loop that drives the first array
 * dimension outermost, which is what owner-computes partitioning needs, (b)
 * move a parallel loop outermost when the original outer loop is serial, and
 * (c) make the innermost loop stride-1, cutting the bytes the cost model
 * counts.  Each legal permutation is tried by really swapping the loop headers,
 * re-running the dependence, profitability and partition analyses, and swapping
 * back; the permutation is applied only if it is strictly better (offloadable
 * where the original is not, or at least 20% less memory traffic).
 * ==========================================================================*/
#include <cstdio>
#include <algorithm>
#include <string>
#include <vector>

#include "ir.h"
#include "analysis.h"
#include "profit.h"
#include "plan.h"

struct Header {
	std::string index_var;
	long        lower;
	OpKind      rel;
	long        upper;
	long        step;
	Loc         loc;
};

static Header get_header(LoopNest *l)
{
	Header h;
	h.index_var = l->index_var;
	h.lower = l->lower;
	h.rel = l->rel;
	h.upper = l->upper;
	h.step = l->step;
	h.loc = l->loc;
	return h;
}

static void set_header(LoopNest *l, const Header &h)
{
	l->index_var = h.index_var;
	l->lower = h.lower;
	l->rel = h.rel;
	l->upper = h.upper;
	l->step = h.step;
	l->loc = h.loc;
}

static std::string perm_text(const std::vector<Header> &hs, const std::vector<int> &perm)
{
	std::string s = "(";
	size_t i;
	for (i = 0; i < perm.size(); i++)
		s += (i ? "," : "") + hs[perm[i]].index_var;
	return s + ")";
}

int interchange_kernel(ProgramPlan *pp, Kernel *k)
{
	IRProgram *p = pp->prog;
	int changed = 0;
	size_t idx;

	for (idx = 0; idx < k->body.size(); idx++) {
		if (k->body[idx].kind != BK_LOOP)
			continue;

		/* the perfect chain starting at this loop */
		std::vector<LoopNest*> chain;
		LoopNest *l = k->body[idx].loop;
		for (;;) {
			chain.push_back(l);
			if (l->body.size() == 1 && l->body[0].kind == BK_LOOP)
				l = l->body[0].loop;
			else
				break;
		}
		int d = (int)chain.size();
		if (d < 2 || d > 4)
			continue;

		std::vector<Header> orig;
		for (int i = 0; i < d; i++)
			orig.push_back(get_header(chain[i]));

		NestPlan *base = make_nest_plan(p, k, chain[0], k->name);
		double base_bytes = base->dec.bytes;
		bool base_off = base->offload;

		std::vector<int> perm(d);
		for (int i = 0; i < d; i++)
			perm[i] = i;

		std::vector<int> best = perm;
		bool best_off = base_off;
		double best_bytes = base_bytes;
		NestPlan *best_plan = base;

		while (std::next_permutation(perm.begin(), perm.end())) {
			/* legality on the direction vectors of the ORIGINAL order */
			bool legal = true;
			size_t di;
			for (di = 0; di < base->na.deps.size() && legal; di++) {
				Dep &dep = base->na.deps[di];
				if ((int)dep.dir.size() != d) {
					legal = false;
					break;
				}
				int lead = -1;
				for (int pos = 0; pos < d; pos++) {
					int dv = dep.dir[perm[pos]];
					if (dv != DIR_EQ) {
						lead = dv;
						break;
					}
				}
				if (lead == DIR_GT)
					legal = false;
			}
			if (!legal)
				continue;

			/* try it: swap headers, analyse, swap back */
			for (int pos = 0; pos < d; pos++)
				set_header(chain[pos], orig[perm[pos]]);
			NestPlan *np = make_nest_plan(p, k, chain[0], k->name);
			for (int pos = 0; pos < d; pos++)
				set_header(chain[pos], orig[pos]);

			bool better = false;
			if (np->offload && !best_off)
				better = true;
			else if (np->offload == best_off && np->dec.bytes < 0.8 * best_bytes)
				better = true;
			if (better) {
				best = perm;
				best_off = np->offload;
				best_bytes = np->dec.bytes;
				best_plan = np;
			}
		}

		bool identity = true;
		for (int i = 0; i < d; i++)
			if (best[i] != i)
				identity = false;
		if (identity)
			continue;

		for (int pos = 0; pos < d; pos++)
			set_header(chain[pos], orig[best[pos]]);

		TransformRecord tr;
		tr.kind = "interchange";
		tr.kernel = k->name;
		std::vector<int> id(d);
		for (int i = 0; i < d; i++)
			id[i] = i;
		tr.what = "loops " + perm_text(orig, id) + " -> " + perm_text(orig, best) +
		          " (legal: every dependence direction vector stays lexicographically non-negative)";
		char g[320];
		snprintf(g, sizeof g, "memory traffic %.0f B -> %.0f B; %s", base_bytes, best_bytes,
		         (best_off && !base_off) ? "nest becomes offloadable"
		                                 : "stride-1 innermost access");
		tr.gain = g;
		tr.bytes_saved = base_bytes - best_bytes;
		pp->transforms.push_back(tr);
		(void)best_plan;
		changed++;
	}
	return changed;
}
