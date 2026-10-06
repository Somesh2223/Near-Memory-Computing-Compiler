/* ============================================================================
 * opt_dce.cpp  --  dead-code elimination driven by liveness analysis
 *
 * Compiler-theory concept: LIVE-VARIABLE ANALYSIS, a BACKWARD dataflow problem
 * (Aho/Lam/Sethi/Ullman 2nd ed., section 9.2.5), followed by removal of every
 * assignment whose target is not live afterwards (section 9.1.6, "Dead-Code
 * Elimination").
 *
 *   a variable is live at a point if its current value may be read later
 *   def[B]  names assigned in block B before any use      use[B]  names read first
 *   IN[B]  = use[B]  union  (OUT[B] - def[B])
 *   OUT[B] = union of IN[S] over the successors S of B     (meet = union)
 *   OUT[exit] = every global scalar: a scalar is program state that later code
 *               (the host, another kernel, the final print) may observe
 *
 * Array stores, calls, prints and the coherence operations have side effects
 * and are always kept; a load whose result is dead has none and is removed.
 * Removing a statement can make its operands dead too, so the analysis and the
 * sweep repeat until nothing changes.  With verbose set, the live-in / live-out
 * sets of the FIRST iteration are printed for every block -- that is the
 * analysis result the sweep acted on.
 * ==========================================================================*/
#include <cstdio>
#include <set>
#include <string>
#include <vector>

#include "ir.h"
#include "cfg.h"
#include "opt.h"

typedef std::set<std::string> NameSet;

static void print_set(NameSet &s)
{
	NameSet::iterator it;
	int first = 1;
	printf("{");
	for (it = s.begin(); it != s.end(); it++) {
		printf("%s%s", first ? "" : ", ", it->c_str());
		first = 0;
	}
	printf("}");
}

/* backward transfer of one statement: live-before = (live-after - def) + uses */
static void live_step(IRStmt *s, NameSet &live)
{
	std::vector<std::string> uses;
	size_t i;
	std::string d = ir_stmt_def(s);
	if (!d.empty())
		live.erase(d);
	ir_stmt_uses(s, &uses);
	for (i = 0; i < uses.size(); i++)
		live.insert(uses[i]);
}

static bool has_side_effect(IRStmt *s)
{
	return s->kind == IR_ARRAY_STORE || s->kind == IR_CALL || s->kind == IR_PRINT ||
	       s->kind == IR_FLUSH || s->kind == IR_INVALIDATE;
}

static void block_end_uses(BasicBlock *bb, NameSet &live)
{
	if (bb->branch != 0 && bb->branch->cond.kind == OPD_NAME)
		live.insert(bb->branch->cond.name);
}

static void block_live_in(BasicBlock *bb, NameSet out, NameSet *in)
{
	int i;
	block_end_uses(bb, out);
	for (i = (int)bb->stmts.size() - 1; i >= 0; i--)
		live_step(bb->stmts[i], out);
	if (bb->loop_header != 0)
		out.erase(bb->loop_header->index_var);
	*in = out;
}

/* rebuild a body without the statements in `dead` */
static void remove_stmts(std::vector<BodyItem> &body, std::set<IRStmt*> &dead)
{
	std::vector<BodyItem> out;
	size_t i;
	for (i = 0; i < body.size(); i++) {
		BodyItem it = body[i];
		if (it.kind == BK_STMT) {
			if (dead.find(it.stmt) == dead.end())
				out.push_back(it);
		} else if (it.kind == BK_IF) {
			remove_stmts(it.ifn->then_body, dead);
			remove_stmts(it.ifn->else_body, dead);
			out.push_back(it);
		} else {
			remove_stmts(it.loop->body, dead);
			out.push_back(it);
		}
	}
	body = out;
}

static int dce_kernel(IRProgram *p, Kernel *k, int verbose)
{
	int total = 0;
	int round = 0;

	for (;;) {
		CFG *g = build_cfg(k);
		size_t n = g->blocks.size();
		std::vector<NameSet> in(n), out(n);
		bool changed = true;
		size_t b, j;
		int i;

		NameSet exit_live;
		std::map<std::string, TypeTag>::iterator sc;
		for (sc = p->scalars.begin(); sc != p->scalars.end(); sc++)
			exit_live.insert(sc->first);

		while (changed) {
			changed = false;
			for (b = n; b-- > 0; ) {
				BasicBlock *bb = g->blocks[b];
				NameSet new_out;
				if ((int)b == g->exit)
					new_out = exit_live;
				for (j = 0; j < bb->succ.size(); j++) {
					NameSet &si = in[bb->succ[j]];
					new_out.insert(si.begin(), si.end());
				}
				NameSet new_in;
				block_live_in(bb, new_out, &new_in);
				if (new_out != out[b] || new_in != in[b]) {
					out[b] = new_out;
					in[b] = new_in;
					changed = true;
				}
			}
		}

		if (verbose && round == 0) {
			printf("  [%s %s] liveness (iteration 1):\n",
			       k->is_host ? "host" : "kernel", k->name.c_str());
			for (b = 0; b < n; b++) {
				printf("    B%-2d %-16s live-in  ", (int)b, g->blocks[b]->label.c_str());
				print_set(in[b]);
				printf("\n    %-20s live-out ", "");
				print_set(out[b]);
				printf("\n");
			}
		}

		/* sweep: mark every assignment whose target is dead right after it */
		std::set<IRStmt*> dead;
		for (b = 0; b < n; b++) {
			BasicBlock *bb = g->blocks[b];
			NameSet live = out[b];
			block_end_uses(bb, live);
			for (i = (int)bb->stmts.size() - 1; i >= 0; i--) {
				IRStmt *s = bb->stmts[i];
				std::string d = ir_stmt_def(s);
				if (!has_side_effect(s) && !d.empty() && live.find(d) == live.end()) {
					dead.insert(s);
					if (verbose)
						printf("    removes dead `%s`\n", ir_stmt_text(s).c_str());
					continue;           /* a removed statement contributes no uses */
				}
				live_step(s, live);
			}
		}

		if (dead.empty())
			break;
		remove_stmts(k->body, dead);
		total += (int)dead.size();
		round++;
	}
	return total;
}

int opt_dce(IRProgram *p, int verbose)
{
	int total = 0;
	size_t k;
	for (k = 0; k < p->kernels.size(); k++)
		total += dce_kernel(p, p->kernels[k], verbose);
	for (k = 0; k < p->hosts.size(); k++)
		total += dce_kernel(p, p->hosts[k], verbose);
	return total;
}
