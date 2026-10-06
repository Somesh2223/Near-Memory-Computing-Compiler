/* ============================================================================
 * pass_coherence.cpp  --  host-cache / PIM-memory coherence by dataflow
 *
 * Compiler-theory concept: a FORWARD dataflow analysis over the control-flow
 * graph (Aho/Lam/Sethi/Ullman 2nd ed., section 9.2 "Introduction to Data-Flow
 * Analysis" -- the same framework as reaching definitions, with a different
 * lattice and transfer function).
 *
 * A DPU reads and writes DRAM directly, bypassing the host's cache, so the two
 * can disagree.  Two facts are tracked per array:
 *
 *   HostDirty   written by host code, not yet flushed: the freshest copy may be
 *               sitting in a dirty cache line, invisible to the DPUs
 *   PimDirty    written by a PIM kernel, not yet invalidated: the host cache
 *               may hold a stale copy of the lines
 *
 *   lattice   state = (HostDirty, PimDirty), each a SET OF ARRAY NAMES,
 *             ordered by inclusion; the analysis is a "may" analysis
 *   meet      union, per set, over the predecessors -- this is where the
 *             if/else merge and the loop header matter: an array that is dirty
 *             on ONE incoming path must be treated as dirty
 *   gen/kill  host store A            : gen HostDirty{A}
 *             host load/store of A, A in PimDirty : emit invalidate(A) first,
 *                                       kill PimDirty{A}
 *             PIM nest touching A, A in HostDirty : emit flush(A) first,
 *                                       kill HostDirty{A}
 *             PIM nest writing A      : gen PimDirty{A}
 *   transfer  the composition of the above over a block's statements; a
 *             `call k()` is expanded in place: each nest of the kernel runs on
 *             the host or on PIM according to the plan
 *
 * (A PIM nest that only WRITES A also flushes it first: otherwise a stale dirty
 * line could be written back later and clobber the DPU's result.)
 *
 * The analysis iterates round-robin until no block's state changes; with
 * `verbose` it prints the table of IN/OUT sets per round -- the worked example
 * in docs/coherence_analysis.md.  The operations found are then inserted into
 * the IR as IR_FLUSH / IR_INVALIDATE statements.  An invalidate needed inside a
 * loop that contains no kernel call is hoisted in front of the loop: the array
 * cannot become PimDirty again inside it.
 * ==========================================================================*/
#include <cstdio>
#include <algorithm>
#include <map>
#include <set>
#include <string>
#include <vector>

#include "ir.h"
#include "cfg.h"
#include "analysis.h"
#include "plan.h"

typedef std::set<std::string> NameSet;

struct CohState {
	NameSet host_dirty;
	NameSet pim_dirty;
};

/* an operation the analysis wants inserted */
struct CohOp {
	bool        is_flush;
	std::string array;
	IRStmt     *host_anchor;     /* insert before this host statement ...       */
	Kernel     *kernel;          /* ... or before this nest of this kernel      */
	LoopNest   *nest;
};

struct CohCtx {
	ProgramPlan             *pp;
	std::vector<CohOp>      *ops;     /* non-null while recording */
	std::map<Kernel*, KernelPlan*> kplan;
};

static bool state_eq(const CohState &a, const CohState &b)
{
	return a.host_dirty == b.host_dirty && a.pim_dirty == b.pim_dirty;
}

static void state_union(CohState *into, const CohState &o)
{
	into->host_dirty.insert(o.host_dirty.begin(), o.host_dirty.end());
	into->pim_dirty.insert(o.pim_dirty.begin(), o.pim_dirty.end());
}

static void need(CohCtx *c, bool flush, const std::string &a, IRStmt *anchor, Kernel *k,
                 LoopNest *nest)
{
	if (!c->ops)
		return;
	size_t i;
	for (i = 0; i < c->ops->size(); i++) {
		CohOp &o = (*c->ops)[i];
		if (o.is_flush == flush && o.array == a && o.host_anchor == anchor && o.kernel == k &&
		    o.nest == nest)
			return;
	}
	CohOp o;
	o.is_flush = flush;
	o.array = a;
	o.host_anchor = anchor;
	o.kernel = k;
	o.nest = nest;
	c->ops->push_back(o);
}

/* arrays read / written by the references of a nest */
static void nest_arrays(NestPlan *np, NameSet *reads, NameSet *writes)
{
	size_t i;
	for (i = 0; i < np->na.refs.size(); i++) {
		RefInfo &r = np->na.refs[i];
		if (r.is_scalar)
			continue;
		if (r.is_write) writes->insert(r.name); else reads->insert(r.name);
	}
}

static void host_access(CohCtx *c, CohState &st, const std::string &a, bool is_write,
                        IRStmt *anchor, Kernel *k, LoopNest *nest)
{
	if (st.pim_dirty.count(a)) {
		need(c, false, a, anchor, k, nest);
		st.pim_dirty.erase(a);
	}
	if (is_write)
		st.host_dirty.insert(a);
}

static void step_stmt(CohCtx *c, CohState &st, IRStmt *s, Kernel *kern, LoopNest *nest);

static void step_call(CohCtx *c, CohState &st, IRStmt *call)
{
	Kernel *k = ir_find_kernel(c->pp->prog, call->callee);
	if (!k || !c->kplan.count(k))
		return;
	KernelPlan *kp = c->kplan[k];
	size_t i, ni = 0;

	for (i = 0; i < k->body.size(); i++) {
		BodyItem &it = k->body[i];
		if (it.kind == BK_STMT) {
			step_stmt(c, st, it.stmt, k, 0);
		} else if (it.kind == BK_IF) {
			std::vector<IRStmt*> v;
			ir_collect_stmts(k->body, &v);     /* conservative: all statements */
			size_t j;
			for (j = 0; j < v.size(); j++)
				step_stmt(c, st, v[j], k, 0);
		} else {
			NestPlan *np = ni < kp->nests.size() ? kp->nests[ni] : 0;
			ni++;
			if (!np)
				continue;
			NameSet rd, wr;
			nest_arrays(np, &rd, &wr);
			NameSet::iterator a;
			if (np->offload) {
				NameSet touched = rd;
				touched.insert(wr.begin(), wr.end());
				for (a = touched.begin(); a != touched.end(); a++) {
					if (np->part.arrays.size()) {
						bool local = false;
						size_t q;
						for (q = 0; q < np->part.arrays.size(); q++)
							if (np->part.arrays[q].name == *a && np->part.arrays[q].local_only)
								local = true;
						if (local)
							continue;          /* never leaves PIM */
					}
					if (st.host_dirty.count(*a)) {
						need(c, true, *a, 0, k, it.loop);
						st.host_dirty.erase(*a);
					}
				}
				for (a = wr.begin(); a != wr.end(); a++) {
					bool local = false;
					size_t q;
					for (q = 0; q < np->part.arrays.size(); q++)
						if (np->part.arrays[q].name == *a && np->part.arrays[q].local_only)
							local = true;
					if (!local)
						st.pim_dirty.insert(*a);
				}
			} else {
				for (a = rd.begin(); a != rd.end(); a++)
					host_access(c, st, *a, false, 0, k, it.loop);
				for (a = wr.begin(); a != wr.end(); a++)
					host_access(c, st, *a, true, 0, k, it.loop);
			}
		}
	}
}

static void step_stmt(CohCtx *c, CohState &st, IRStmt *s, Kernel *kern, LoopNest *nest)
{
	if (s->kind == IR_ARRAY_LOAD)
		host_access(c, st, s->array, false, kern ? 0 : s, kern, nest);
	else if (s->kind == IR_ARRAY_STORE)
		host_access(c, st, s->array, true, kern ? 0 : s, kern, nest);
	else if (s->kind == IR_CALL && !kern)
		step_call(c, st, s);
}

static CohState block_transfer(CohCtx *c, BasicBlock *bb, CohState st)
{
	size_t i;
	for (i = 0; i < bb->stmts.size(); i++)
		step_stmt(c, st, bb->stmts[i], 0, 0);
	return st;
}

static std::string set_text(const NameSet &s)
{
	std::string t = "{";
	NameSet::const_iterator it;
	for (it = s.begin(); it != s.end(); it++)
		t += (it == s.begin() ? "" : ",") + *it;
	return t + "}";
}

/* ---- inserting the operations into the nested IR ---------------------------- */
struct PathEl {
	std::vector<BodyItem> *list;
	size_t                 idx;
	int                    container;    /* 0 top, 1 loop body, 2 if branch */
	LoopNest              *loop;
};

static bool find_path(std::vector<BodyItem> &body, IRStmt *target, int container, LoopNest *loop,
                      std::vector<PathEl> *path)
{
	size_t i;
	for (i = 0; i < body.size(); i++) {
		BodyItem &it = body[i];
		PathEl e;
		e.list = &body;
		e.idx = i;
		e.container = container;
		e.loop = loop;
		if (it.kind == BK_STMT) {
			if (it.stmt == target) {
				path->push_back(e);
				return true;
			}
		} else if (it.kind == BK_IF) {
			path->push_back(e);
			if (find_path(it.ifn->then_body, target, 2, 0, path)) return true;
			if (find_path(it.ifn->else_body, target, 2, 0, path)) return true;
			path->pop_back();
		} else {
			path->push_back(e);
			if (find_path(it.loop->body, target, 1, it.loop, path)) return true;
			path->pop_back();
		}
	}
	return false;
}

static bool loop_has_call(LoopNest *l)
{
	std::vector<IRStmt*> v;
	ir_collect_stmts(l->body, &v);
	size_t i;
	for (i = 0; i < v.size(); i++)
		if (v[i]->kind == IR_CALL)
			return true;
	return false;
}

/* returns the (list, index) before which to insert for a host-statement anchor */
static void locate_host(std::vector<BodyItem> &body, IRStmt *anchor, bool hoist,
                        std::vector<BodyItem> **list, size_t *idx)
{
	std::vector<PathEl> path;
	*list = 0;
	*idx = 0;
	if (!find_path(body, anchor, 0, 0, &path))
		return;
	/* path.back() is the statement's own list; walk outwards through loops */
	size_t level = path.size() - 1;
	if (hoist)
		while (level > 0 && path[level].container == 1 && !loop_has_call(path[level].loop))
			level--;
	*list = path[level].list;
	*idx = path[level].idx;
}

void coherence_run(ProgramPlan *pp, int verbose)
{
	IRProgram *p = pp->prog;
	CohCtx ctx;
	size_t hi, i, j;

	ctx.pp = pp;
	ctx.ops = 0;
	for (i = 0; i < pp->kernels.size(); i++)
		ctx.kplan[pp->kernels[i].kernel] = &pp->kernels[i];

	for (hi = 0; hi < p->hosts.size(); hi++) {
		Kernel *host = p->hosts[hi];
		CFG *g = build_cfg(host);
		size_t n = g->blocks.size();
		std::vector<CohState> in(n), out(n);
		std::vector<bool> reached(n, false);
		reached[g->entry] = true;

		if (verbose)
			printf("coherence analysis of host block `%s`  (HostDirty / PimDirty, meet = union)\n",
			       host->name.c_str());

		int round = 0;
		bool changed = true;
		while (changed) {
			changed = false;
			round++;
			if (verbose)
				printf("  round %d\n", round);
			for (i = 0; i < n; i++) {
				BasicBlock *bb = g->blocks[i];
				CohState new_in;
				bool have = ((int)i == g->entry);
				for (j = 0; j < bb->pred.size(); j++)
					if (reached[bb->pred[j]]) {
						state_union(&new_in, out[bb->pred[j]]);
						have = true;
					}
				if (!have)
					continue;
				CohState new_out = block_transfer(&ctx, bb, new_in);
				if (!reached[i] || !state_eq(new_in, in[i]) || !state_eq(new_out, out[i])) {
					changed = true;
					reached[i] = true;
				}
				in[i] = new_in;
				out[i] = new_out;
				if (verbose)
					printf("    B%-2d %-16s IN  H=%-12s P=%-12s OUT H=%-12s P=%s\n", (int)i,
					       bb->label.c_str(), set_text(new_in.host_dirty).c_str(),
					       set_text(new_in.pim_dirty).c_str(), set_text(new_out.host_dirty).c_str(),
					       set_text(new_out.pim_dirty).c_str());
			}
		}

		/* final pass: record where operations are needed */
		std::vector<CohOp> ops;
		ctx.ops = &ops;
		for (i = 0; i < n; i++)
			if (reached[i])
				block_transfer(&ctx, g->blocks[i], in[i]);
		ctx.ops = 0;

		/* insert into the IR */
		for (i = 0; i < ops.size(); i++) {
			CohOp &o = ops[i];
			IRStmt *ns = o.is_flush ? mk_flush_stmt(o.array, host->loc)
			                        : mk_invalidate_stmt(o.array, host->loc);
			if (o.kernel) {
				for (j = 0; j < o.kernel->body.size(); j++)
					if (o.kernel->body[j].kind == BK_LOOP && o.kernel->body[j].loop == o.nest) {
						o.kernel->body.insert(o.kernel->body.begin() + j, mk_bi_stmt(ns));
						break;
					}
			} else {
				std::vector<BodyItem> *list;
				size_t idx;
				locate_host(host->body, o.host_anchor, !o.is_flush, &list, &idx);
				if (list)
					list->insert(list->begin() + idx, mk_bi_stmt(ns));
			}
			if (verbose)
				printf("  insert %s(%s)%s\n", o.is_flush ? "flush" : "invalidate", o.array.c_str(),
				       o.kernel ? "  inside the kernel body, before the nest" : "");
		}
		if (verbose && ops.empty())
			printf("  no coherence operation needed\n");
	}
}
