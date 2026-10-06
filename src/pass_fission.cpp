/* ============================================================================
 * pass_fission.cpp  --  loop distribution (fission)
 *
 * Compiler-theory concept: Allen-Kennedy loop distribution via the strongly
 * connected components of the statement dependence graph (Allen & Kennedy,
 * "Automatic Translation of Fortran Programs to Vector Form", 1987; ALSU 2nd
 * ed. section 11.7.9; Kuck et al.).
 *
 *   1. build the dependence graph whose nodes are the loop body's statements
 *      (here: STATEMENT GROUPS -- the run of three-address instructions that
 *      implements one source statement) and whose edges are the dependences of
 *      Phase D, plus an edge in both directions between two groups that share
 *      a compiler temporary (so a value computed in one stays with its user);
 *   2. find its strongly connected components with TARJAN's algorithm -- the
 *      statements of one component form a recurrence and cannot be separated;
 *   3. topologically sort the component DAG;
 *   4. emit each component as its own loop with the original header.
 *
 * Distribution is legal by construction: every dependence either lies inside a
 * component or goes forward in the sorted order.  Its purpose here is to split
 * a loop that mixes an offloadable statement with a non-offloadable one (a
 * recurrence, an indirect access), so the offloadable part can go to PIM while
 * the rest stays on the host.  A split is kept only if at least one of the
 * resulting loops is offloadable -- the profitability analysis is re-run on
 * every candidate.
 * ==========================================================================*/
#include <cstdio>
#include <algorithm>
#include <map>
#include <set>
#include <string>
#include <vector>

#include "ir.h"
#include "analysis.h"
#include "profit.h"
#include "plan.h"

struct Group {
	std::vector<BodyItem> items;
};

static bool ends_group(IRStmt *s)
{
	if (s->kind == IR_ARRAY_STORE || s->kind == IR_CALL || s->kind == IR_PRINT ||
	    s->kind == IR_FLUSH || s->kind == IR_INVALIDATE)
		return true;
	return s->kind == IR_COPY && !is_temp_name(s->dst);
}

static void make_groups(std::vector<BodyItem> &body, std::vector<Group> *out)
{
	Group cur;
	size_t i;
	for (i = 0; i < body.size(); i++) {
		cur.items.push_back(body[i]);
		if (body[i].kind != BK_STMT || ends_group(body[i].stmt)) {
			out->push_back(cur);
			cur.items.clear();
		}
	}
	if (!cur.items.empty())
		out->push_back(cur);
}

static void index_group(std::vector<BodyItem> &items, int g, std::map<IRStmt*, int> *m)
{
	std::vector<IRStmt*> all;
	ir_collect_stmts(items, &all);
	size_t i;
	for (i = 0; i < all.size(); i++)
		(*m)[all[i]] = g;
}

/* ---- Tarjan's strongly-connected-components algorithm ----------------------- */
struct Tarjan {
	int                          n;
	std::vector<std::vector<int> > adj;
	std::vector<int>             index, low, comp;
	std::vector<bool>            on_stack;
	std::vector<int>             stack;
	int                          counter;
	int                          ncomp;
};

static void tarjan_visit(Tarjan *t, int v)
{
	size_t i;
	t->index[v] = t->low[v] = t->counter++;
	t->stack.push_back(v);
	t->on_stack[v] = true;
	for (i = 0; i < t->adj[v].size(); i++) {
		int w = t->adj[v][i];
		if (t->index[w] < 0) {
			tarjan_visit(t, w);
			t->low[v] = std::min(t->low[v], t->low[w]);
		} else if (t->on_stack[w]) {
			t->low[v] = std::min(t->low[v], t->index[w]);
		}
	}
	if (t->low[v] == t->index[v]) {
		for (;;) {
			int w = t->stack.back();
			t->stack.pop_back();
			t->on_stack[w] = false;
			t->comp[w] = t->ncomp;
			if (w == v)
				break;
		}
		t->ncomp++;
	}
}

static std::string group_text(Group &g)
{
	size_t i;
	for (i = g.items.size(); i-- > 0; )
		if (g.items[i].kind == BK_STMT)
			return ir_stmt_text(g.items[i].stmt);
	return "<nested control>";
}

int fission_kernel(ProgramPlan *pp, Kernel *k)
{
	IRProgram *p = pp->prog;
	int changed = 0;
	size_t idx;

	for (idx = 0; idx < k->body.size(); idx++) {
		if (k->body[idx].kind != BK_LOOP)
			continue;
		LoopNest *loop = k->body[idx].loop;

		NestPlan *base = make_nest_plan(p, k, loop, k->name);
		if (base->dec.offload)
			continue;                               /* nothing to enable */

		std::vector<Group> groups;
		make_groups(loop->body, &groups);
		if (groups.size() < 2)
			continue;

		/* strength-reduced accumulators are loop-carried across groups: skip */
		std::vector<IRStmt*> all;
		ir_collect_stmts(loop->body, &all);
		bool has_iv = false;
		size_t i, j;
		for (i = 0; i < all.size(); i++)
			if (p->ivs.count(all[i]->dst))
				has_iv = true;
		if (has_iv)
			continue;

		/* statement-group dependence graph */
		int n = (int)groups.size();
		std::map<IRStmt*, int> gof;
		for (i = 0; i < groups.size(); i++)
			index_group(groups[i].items, (int)i, &gof);

		Tarjan t;
		t.n = n;
		t.adj.assign(n, std::vector<int>());
		t.index.assign(n, -1);
		t.low.assign(n, 0);
		t.comp.assign(n, -1);
		t.on_stack.assign(n, false);
		t.counter = 0;
		t.ncomp = 0;

		for (i = 0; i < base->na.deps.size(); i++) {
			Dep &d = base->na.deps[i];
			if (!gof.count(d.src_stmt) || !gof.count(d.dst_stmt))
				continue;
			t.adj[gof[d.src_stmt]].push_back(gof[d.dst_stmt]);
		}
		/* shared temporaries bind groups together */
		for (i = 0; i < all.size(); i++) {
			std::vector<std::string> uses;
			ir_stmt_uses(all[i], &uses);
			for (j = 0; j < uses.size(); j++) {
				std::map<std::string, IRStmt*>::iterator dd = base->na.defs.find(uses[j]);
				if (dd == base->na.defs.end() || !gof.count(dd->second))
					continue;
				int gd = gof[dd->second], gu = gof[all[i]];
				if (gd != gu) {
					t.adj[gd].push_back(gu);
					t.adj[gu].push_back(gd);
				}
			}
		}
		/* an IfNode's condition temp is used by the node itself (not a stmt) */
		for (i = 0; i < groups.size(); i++)
			for (j = 0; j < groups[i].items.size(); j++)
				if (groups[i].items[j].kind == BK_IF &&
				    groups[i].items[j].ifn->cond.kind == OPD_NAME) {
					std::map<std::string, IRStmt*>::iterator dd =
					    base->na.defs.find(groups[i].items[j].ifn->cond.name);
					if (dd != base->na.defs.end() && gof.count(dd->second) &&
					    gof[dd->second] != (int)i) {
						t.adj[gof[dd->second]].push_back((int)i);
						t.adj[i].push_back(gof[dd->second]);
					}
				}

		for (i = 0; i < (size_t)n; i++)
			if (t.index[i] < 0)
				tarjan_visit(&t, (int)i);
		if (t.ncomp < 2)
			continue;

		/* topological order of the component DAG (Kahn, ties by first group) */
		int nc = t.ncomp;
		std::vector<std::set<int> > cadj(nc);
		std::vector<int> indeg(nc, 0), first(nc, n);
		for (i = 0; i < (size_t)n; i++) {
			first[t.comp[i]] = std::min(first[t.comp[i]], (int)i);
			for (j = 0; j < t.adj[i].size(); j++) {
				int a = t.comp[i], b = t.comp[t.adj[i][j]];
				if (a != b && !cadj[a].count(b)) {
					cadj[a].insert(b);
					indeg[b]++;
				}
			}
		}
		std::vector<int> order;
		std::vector<bool> done(nc, false);
		while ((int)order.size() < nc) {
			int pick = -1;
			for (int c = 0; c < nc; c++)
				if (!done[c] && indeg[c] == 0 && (pick < 0 || first[c] < first[pick]))
					pick = c;
			if (pick < 0)
				break;                                 /* cannot happen: DAG */
			done[pick] = true;
			order.push_back(pick);
			std::set<int>::iterator it;
			for (it = cadj[pick].begin(); it != cadj[pick].end(); it++)
				indeg[*it]--;
		}
		if ((int)order.size() != nc)
			continue;

		/* candidate loops, one per component */
		std::vector<LoopNest*> parts;
		std::vector<std::string> descr;
		for (size_t o = 0; o < order.size(); o++) {
			LoopNest *nl = new LoopNest(*loop);
			nl->body.clear();
			std::string txt;
			for (i = 0; i < (size_t)n; i++)
				if (t.comp[i] == order[o]) {
					for (j = 0; j < groups[i].items.size(); j++)
						nl->body.push_back(groups[i].items[j]);
					txt += (txt.empty() ? "" : " ; ") + group_text(groups[i]);
				}
			parts.push_back(nl);
			descr.push_back(txt);
		}

		/* keep the split only if some part becomes offloadable */
		std::vector<NestPlan*> plans;
		bool any = false;
		for (i = 0; i < parts.size(); i++) {
			plans.push_back(make_nest_plan(p, k, parts[i], k->name));
			if (plans[i]->dec.offload)
				any = true;
		}
		if (!any)
			continue;

		std::vector<BodyItem> repl;
		for (i = 0; i < parts.size(); i++)
			repl.push_back(mk_bi_loop(parts[i]));
		k->body.erase(k->body.begin() + idx);
		k->body.insert(k->body.begin() + idx, repl.begin(), repl.end());

		TransformRecord tr;
		tr.kind = "fission";
		tr.kernel = k->name;
		char b[256];
		snprintf(b, sizeof b, "split loop `%s` (%d statement groups) into %d loops by SCC of the dependence graph",
		         loop->index_var.c_str(), n, (int)parts.size());
		tr.what = b;
		std::string gain;
		for (i = 0; i < parts.size(); i++) {
			char g[256];
			snprintf(g, sizeof g, "%sloop %d {%s}: %s", i ? "; " : "", (int)i + 1,
			         descr[i].c_str(), plans[i]->dec.offload ? "OFFLOADABLE" : "stays on host");
			gain += g;
		}
		gain += "; the whole nest was previously REJECTED (" + base->dec.reason + ")";
		tr.gain = gain;
		tr.bytes_saved = 0;
		pp->transforms.push_back(tr);

		changed++;
		idx += parts.size() - 1;
	}
	return changed;
}
