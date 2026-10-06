/* ============================================================================
 * regalloc.cpp  --  register allocation by graph colouring (Chaitin-Briggs)
 *
 * Compiler-theory concept: global register allocation as graph colouring
 * (Chaitin et al. 1981, "Register Allocation via Coloring"; Briggs, Cooper &
 * Torczon 1994, "Improvements to Graph Coloring Register Allocation"; Aho/Lam/
 * Sethi/Ullman 2nd ed., section 8.8 for the basics).  This is NOT linear scan.
 *
 *   1. LIVENESS.  Backward dataflow over the DPU instruction-level control-flow
 *      graph (loops add a back edge, an if adds a skip edge):
 *            live-in(i) = use(i)  union  (live-out(i) - def(i)),
 *            live-out(i) = union of live-in over the successors of i.
 *   2. INTERFERENCE GRAPH.  One node per virtual register; an edge joins two
 *      registers that are simultaneously live: for every definition d at i, d
 *      interferes with everything live-out of i -- except, for a copy d := s,
 *      the source s (they hold the same value, so they may share a register).
 *   3. COALESCE.  Merge the two ends of a copy when it is conservative: the
 *      Briggs test -- the merged node must have fewer than K neighbours of
 *      significant degree (>= K) -- so coalescing can never turn a colourable
 *      graph into an uncolourable one.
 *   4. SIMPLIFY.  Repeatedly remove a node of degree < K and push it on a
 *      stack: any such node can always be coloured last.  If every remaining
 *      node has degree >= K, push a SPILL CANDIDATE anyway (Briggs's optimistic
 *      colouring): the one with the smallest  cost / degree  where
 *            cost = sum over its definitions and uses of 10^(loop depth),
 *      i.e. cheap-to-reload, long-lived, high-degree registers go first.
 *   5. SELECT.  Pop the stack, giving each node the lowest colour none of its
 *      already-coloured neighbours uses.  A candidate may still find a free
 *      colour (the optimism pays off); if one does not, it is an ACTUAL SPILL.
 *   6. SPILL.  Each actual spill gets a scratchpad slot: every use is preceded
 *      by a load into a fresh short-lived register, every definition followed
 *      by a store.  The program changed, so the whole process repeats.
 *
 * ENDLOOP is a pure branch (the increment is an explicit ADD before it), so a
 * spilled loop index needs no special case.
 * ==========================================================================*/
#include <cstdio>
#include <cmath>
#include <algorithm>
#include <map>
#include <set>
#include <string>
#include <vector>

#include "ir.h"
#include "dpu.h"

/* ---- def / use of one instruction ------------------------------------------ */
static int instr_def(const DInstr &I)
{
	switch (I.op) {
	case D_MOVI: case D_MOV: case D_MIN:
	case D_ADD: case D_SUB: case D_MUL: case D_DIV: case D_MOD:
	case D_SLT: case D_SLE: case D_SGT: case D_SGE: case D_SEQ: case D_SNE:
	case D_LD: case D_ARG: case D_SPILL_LD:
		return I.d;
	}
	return -1;
}

static int instr_uses(const DInstr &I, int out[3])
{
	int n = 0;
	switch (I.op) {
	case D_MOV: case D_IFNZ: case D_RET: case D_SPILL_ST: case D_LD: case D_HALO:
		if (I.a >= 0) out[n++] = I.a;
		break;
	case D_MIN: case D_ADD: case D_SUB: case D_MUL: case D_DIV: case D_MOD:
	case D_SLT: case D_SLE: case D_SGT: case D_SGE: case D_SEQ: case D_SNE:
	case D_ST: case D_LOOP: case D_DMA_IN: case D_DMA_OUT:
		if (I.a >= 0) out[n++] = I.a;
		if (I.b >= 0) out[n++] = I.b;
		break;
	}
	return n;
}

static void set_def(DInstr &I, int v)
{
	I.d = v;
}

/* replace every use of `from` in I by `to` */
static void replace_uses(DInstr &I, int from, int to)
{
	switch (I.op) {
	case D_MOV: case D_IFNZ: case D_RET: case D_SPILL_ST: case D_LD: case D_HALO:
		if (I.a == from) I.a = to;
		break;
	case D_MIN: case D_ADD: case D_SUB: case D_MUL: case D_DIV: case D_MOD:
	case D_SLT: case D_SLE: case D_SGT: case D_SGE: case D_SEQ: case D_SNE:
	case D_ST: case D_LOOP: case D_DMA_IN: case D_DMA_OUT:
		if (I.a == from) I.a = to;
		if (I.b == from) I.b = to;
		break;
	}
}

/* ---- control-flow graph of the instruction list ------------------------------- */
static void build_succ(const std::vector<DInstr> &code, std::vector<std::vector<int> > *succ,
                       std::vector<int> *depth)
{
	size_t n = code.size();
	std::vector<int> match(n, -1), else_of(n, -1);
	std::vector<int> st;
	size_t i;

	succ->assign(n, std::vector<int>());
	depth->assign(n, 0);

	for (i = 0; i < n; i++) {
		if (code[i].op == D_LOOP || code[i].op == D_IFNZ) {
			st.push_back((int)i);
		} else if (code[i].op == D_ELSE) {
			else_of[st.back()] = (int)i;
		} else if (code[i].op == D_ENDLOOP || code[i].op == D_ENDIF) {
			match[st.back()] = (int)i;
			match[i] = st.back();
			st.pop_back();
		}
	}
	int d = 0;
	for (i = 0; i < n; i++) {
		if (code[i].op == D_ENDLOOP) d--;
		(*depth)[i] = d;
		if (code[i].op == D_LOOP) d++;
	}
	for (i = 0; i < n; i++) {
		std::vector<int> &s = (*succ)[i];
		switch (code[i].op) {
		case D_LOOP:
			s.push_back((int)i + 1);
			s.push_back(match[i] + 1);
			break;
		case D_ENDLOOP:
			s.push_back(match[i]);
			break;
		case D_IFNZ:
			s.push_back((int)i + 1);
			s.push_back(else_of[i] >= 0 ? else_of[i] + 1 : match[i]);
			break;
		case D_ELSE: {
			/* skip the else branch: jump to the matching ENDIF */
			int j = (int)i + 1, nest = 0;
			for (; j < (int)n; j++) {
				if (code[j].op == D_IFNZ) nest++;
				if (code[j].op == D_ENDIF) {
					if (nest == 0) break;
					nest--;
				}
			}
			s.push_back(j);
			break;
		}
		default:
			if (i + 1 < n) s.push_back((int)i + 1);
		}
		/* a successor past the end is not a successor */
		size_t q = 0;
		while (q < s.size())
			if (s[q] >= (int)n) s.erase(s.begin() + q); else q++;
	}
}

/* ---- liveness ------------------------------------------------------------------ */
struct Live {
	std::vector<std::vector<char> > in, out;
};

static void liveness(DProgram *prog, const std::vector<std::vector<int> > &succ, Live *L)
{
	size_t n = prog->code.size();
	size_t nv = (size_t)prog->nvregs;
	size_t i, j;
	bool changed = true;

	L->in.assign(n, std::vector<char>(nv, 0));
	L->out.assign(n, std::vector<char>(nv, 0));
	while (changed) {
		changed = false;
		for (i = n; i-- > 0; ) {
			std::vector<char> out(nv, 0);
			for (j = 0; j < succ[i].size(); j++)
				for (size_t v = 0; v < nv; v++)
					if (L->in[succ[i][j]][v])
						out[v] = 1;
			std::vector<char> in = out;
			int d = instr_def(prog->code[i]);
			if (d >= 0)
				in[d] = 0;
			int u[3];
			int nu = instr_uses(prog->code[i], u);
			for (int k = 0; k < nu; k++)
				in[u[k]] = 1;
			if (in != L->in[i] || out != L->out[i]) {
				L->in[i] = in;
				L->out[i] = out;
				changed = true;
			}
		}
	}
}

/* ---- the interference graph ------------------------------------------------------ */
typedef std::vector<std::set<int> > Graph;

static void build_graph(DProgram *prog, Live &L, Graph *g,
                        std::vector<std::pair<int,int> > *moves)
{
	size_t i, v;
	g->assign((size_t)prog->nvregs, std::set<int>());
	moves->clear();
	for (i = 0; i < prog->code.size(); i++) {
		DInstr &I = prog->code[i];
		int d = instr_def(I);
		if (d < 0)
			continue;
		bool copy = (I.op == D_MOV);
		if (copy)
			moves->push_back(std::make_pair(d, I.a));
		for (v = 0; v < (size_t)prog->nvregs; v++) {
			if (!L.out[i][v] || (int)v == d)
				continue;
			if (copy && (int)v == I.a)
				continue;
			(*g)[d].insert((int)v);
			(*g)[v].insert(d);
		}
	}
}

static int find_rep(std::vector<int> &uf, int v)
{
	while (uf[v] != v) {
		uf[v] = uf[uf[v]];
		v = uf[v];
	}
	return v;
}

/* ---- the allocator ------------------------------------------------------------------ */
void regalloc_run(DProgram *prog, int K, RAResult *out, int verbose)
{
	std::vector<bool> nospill((size_t)prog->nvregs, false);
	out->K = K;
	out->rounds = 0;
	out->spilled = 0;
	out->coalesced.clear();
	out->order.clear();
	prog->nspill = 0;
	prog->nregs = K;

	for (;;) {
		out->rounds++;
		size_t n = (size_t)prog->nvregs;
		std::vector<std::vector<int> > succ;
		std::vector<int> depth;
		build_succ(prog->code, &succ, &depth);
		Live L;
		liveness(prog, succ, &L);
		Graph g;
		std::vector<std::pair<int,int> > moves;
		build_graph(prog, L, &g, &moves);

		/* occurrence cost of each register, weighted by loop depth */
		std::vector<double> cost(n, 0.0);
		size_t i, v;
		for (i = 0; i < prog->code.size(); i++) {
			double w = std::pow(10.0, depth[i]);
			int d = instr_def(prog->code[i]);
			if (d >= 0) cost[d] += w;
			int u[3];
			int nu = instr_uses(prog->code[i], u);
			for (int k = 0; k < nu; k++) cost[u[k]] += w;
		}

		/* ---- coalescing (Briggs) ------------------------------------------- */
		std::vector<int> uf(n);
		for (v = 0; v < n; v++) uf[v] = (int)v;
		bool again = true;
		while (again) {
			again = false;
			for (i = 0; i < moves.size(); i++) {
				int a = find_rep(uf, moves[i].first), b = find_rep(uf, moves[i].second);
				if (a == b || g[a].count(b))
					continue;
				std::set<int> uni = g[a];
				uni.insert(g[b].begin(), g[b].end());
				int significant = 0;
				std::set<int>::iterator it;
				for (it = uni.begin(); it != uni.end(); it++) {
					int deg = (int)g[*it].size();
					if (g[*it].count(a) && g[*it].count(b))
						deg--;            /* it will lose one neighbour in the merge */
					if (deg >= K)
						significant++;
				}
				if (significant >= K)
					continue;
				/* merge b into a */
				for (it = g[b].begin(); it != g[b].end(); it++) {
					g[*it].erase(b);
					g[*it].insert(a);
					g[a].insert(*it);
				}
				g[b].clear();
				uf[b] = a;
				cost[a] += cost[b];
				if (nospill[b]) nospill[a] = true;
				out->coalesced.push_back(std::make_pair(a, b));
				again = true;
			}
		}

		/* ---- simplify ------------------------------------------------------- */
		std::vector<bool> removed(n, false);
		std::vector<int> deg(n, 0), stack;
		std::vector<bool> is_node(n, false);
		for (v = 0; v < n; v++)
			if (find_rep(uf, (int)v) == (int)v) {
				is_node[v] = true;
				deg[v] = (int)g[v].size();
			}
		size_t remaining = 0;
		for (v = 0; v < n; v++) if (is_node[v]) remaining++;
		while (remaining > 0) {
			int pick = -1;
			for (v = 0; v < n; v++)
				if (is_node[v] && !removed[v] && deg[v] < K) { pick = (int)v; break; }
			if (pick < 0) {
				double best = 1e300;
				for (v = 0; v < n; v++) {
					if (!is_node[v] || removed[v])
						continue;
					double c = nospill[v] ? 1e290 : cost[v] / std::max(1, deg[v]);
					if (c < best) { best = c; pick = (int)v; }
				}
			}
			removed[pick] = true;
			stack.push_back(pick);
			remaining--;
			std::set<int>::iterator it;
			for (it = g[pick].begin(); it != g[pick].end(); it++)
				if (!removed[*it])
					deg[*it]--;
		}

		/* ---- select --------------------------------------------------------------- */
		std::vector<int> col(n, -1);
		std::vector<int> actual;
		out->order.clear();
		while (!stack.empty()) {
			int nd = stack.back();
			stack.pop_back();
			std::vector<bool> used((size_t)K, false);
			std::set<int>::iterator it;
			for (it = g[nd].begin(); it != g[nd].end(); it++)
				if (col[*it] >= 0)
					used[col[*it]] = true;
			int c = -1;
			for (int k = 0; k < K; k++)
				if (!used[k]) { c = k; break; }
			if (c < 0)
				actual.push_back(nd);
			else
				col[nd] = c;
			out->order.push_back(nd);
		}

		if (actual.empty()) {
			out->color.assign(n, -1);
			for (v = 0; v < n; v++)
				out->color[v] = col[find_rep(uf, (int)v)];
			out->graph.assign(n, std::vector<int>());
			std::vector<std::vector<int> > full;
			Graph g0;
			std::vector<std::pair<int,int> > m0;
			build_graph(prog, L, &g0, &m0);
			for (v = 0; v < n; v++)
				out->graph[v].assign(g0[v].begin(), g0[v].end());

			/* verify: interfering registers never share a colour */
			out->verified = true;
			for (v = 0; v < n; v++)
				for (size_t q = 0; q < out->graph[v].size(); q++)
					if (out->color[v] >= 0 && out->color[v] == out->color[out->graph[v][q]])
						out->verified = false;

			/* live ranges for the report */
			out->ranges.clear();
			for (v = 0; v < n; v++) {
				LiveRange lr;
				lr.vreg = (int)v;
				lr.first = -1;
				lr.last = -1;
				lr.color = out->color[v];
				for (i = 0; i < prog->code.size(); i++) {
					bool here = L.in[i][v] || L.out[i][v] || instr_def(prog->code[i]) == (int)v;
					int u[3];
					int nu = instr_uses(prog->code[i], u);
					for (int k = 0; k < nu; k++) if (u[k] == (int)v) here = true;
					if (here) {
						if (lr.first < 0) lr.first = (int)i;
						lr.last = (int)i;
					}
				}
				if (lr.first >= 0)
					out->ranges.push_back(lr);
			}

			/* rewrite the program with physical registers */
			int maxc = -1;
			for (i = 0; i < prog->code.size(); i++) {
				DInstr &I = prog->code[i];
				if (I.d >= 0) I.d = out->color[I.d];
				if (I.a >= 0) I.a = out->color[I.a];
				if (I.b >= 0) I.b = out->color[I.b];
				maxc = std::max(maxc, std::max(I.d, std::max(I.a, I.b)));
			}
			out->registers_used = maxc + 1;
			prog->allocated = true;
			return;
		}

		/* ---- spill ---------------------------------------------------------------- */
		if (verbose)
			printf("  round %d: %d register(s) must spill\n", out->rounds, (int)actual.size());
		std::set<int> spill_rep(actual.begin(), actual.end());
		std::map<int, int> slot;
		for (v = 0; v < n; v++) {
			int r = find_rep(uf, (int)v);
			if (spill_rep.count(r) && !slot.count(r))
				slot[r] = prog->nspill++;
		}
		std::vector<DInstr> nc;
		for (i = 0; i < prog->code.size(); i++) {
			DInstr I = prog->code[i];
			std::vector<DInstr> before, after;
			int u[3];
			int nu = instr_uses(I, u);
			for (int k = 0; k < nu; k++) {
				int r = find_rep(uf, u[k]);
				if (!spill_rep.count(r))
					continue;
				int t = prog->nvregs++;
				prog->vreg_name.push_back("spill" + std::to_string(slot[r]));
				prog->vreg_float.push_back(prog->vreg_float[u[k]]);
				nospill.push_back(true);
				DInstr ld;
				ld.op = D_SPILL_LD; ld.fp = false; ld.d = t; ld.a = ld.b = -1;
				ld.imm = slot[r]; ld.fimm = 0; ld.note = "reload";
				before.push_back(ld);
				replace_uses(I, u[k], t);
			}
			int d = instr_def(I);
			if (d >= 0 && spill_rep.count(find_rep(uf, d))) {
				int r = find_rep(uf, d);
				int t = prog->nvregs++;
				prog->vreg_name.push_back("spill" + std::to_string(slot[r]));
				prog->vreg_float.push_back(prog->vreg_float[d]);
				nospill.push_back(true);
				set_def(I, t);
				DInstr st;
				st.op = D_SPILL_ST; st.fp = false; st.d = -1; st.a = t; st.b = -1;
				st.imm = slot[r]; st.fimm = 0; st.note = "spill";
				after.push_back(st);
			}
			for (size_t q = 0; q < before.size(); q++) nc.push_back(before[q]);
			nc.push_back(I);
			for (size_t q = 0; q < after.size(); q++) nc.push_back(after[q]);
		}
		prog->code = nc;
		out->spilled += (int)spill_rep.size();
	}
}

/* ---- reporting ------------------------------------------------------------------------ */
void regalloc_print(DProgram *prog, RAResult *r)
{
	size_t i;
	printf("register allocation of %s  (K = %d registers, Chaitin-Briggs)\n", prog->name.c_str(), r->K);
	printf("  rounds of build/simplify/select : %d\n", r->rounds);
	printf("  copies coalesced                : %d\n", (int)r->coalesced.size());
	printf("  registers spilled to scratchpad : %d\n", r->spilled);
	printf("  physical registers used         : %d\n", r->registers_used);
	printf("  colouring verified              : %s\n", r->verified ? "yes (no interfering pair shares a register)" : "NO");
	printf("  live intervals (instruction index first..last):\n");
	for (i = 0; i < r->ranges.size(); i++) {
		LiveRange &lr = r->ranges[i];
		printf("    v%-3d %-14s [%3d..%3d]  -> r%d\n", lr.vreg,
		       lr.vreg < (int)prog->vreg_name.size() ? prog->vreg_name[lr.vreg].c_str() : "?",
		       lr.first, lr.last, lr.color);
	}
	printf("  colouring order (stack popped first-to-last):");
	for (i = 0; i < r->order.size(); i++) {
		int v = r->order[i];
		printf(" v%d=r%d", v, r->color[v]);
	}
	printf("\n");
	for (i = 0; i < r->coalesced.size(); i++)
		printf("  coalesced v%d into v%d\n", r->coalesced[i].second, r->coalesced[i].first);
}

void regalloc_emit_dot(DProgram *prog, RAResult *r, FILE *out)
{
	static const char *palette[] = {"#8dd3c7", "#ffffb3", "#bebada", "#fb8072", "#80b1d3", "#fdb462",
	                                "#b3de69", "#fccde5", "#d9d9d9", "#bc80bd", "#ccebc5", "#ffed6f"};
	size_t v, q;
	fprintf(out, "graph \"interference_%s\" {\n  node [shape=circle, style=filled, fontsize=10];\n",
	        prog->name.c_str());
	for (v = 0; v < r->graph.size(); v++) {
		if (r->graph[v].empty() && r->color[v] < 0)
			continue;
		fprintf(out, "  v%zu [label=\"%s\\nr%d\", fillcolor=\"%s\"];\n", v,
		        v < prog->vreg_name.size() ? prog->vreg_name[v].c_str() : "?", r->color[v],
		        palette[(r->color[v] < 0 ? 0 : r->color[v]) % 12]);
	}
	for (v = 0; v < r->graph.size(); v++)
		for (q = 0; q < r->graph[v].size(); q++)
			if ((int)v < r->graph[v][q])
				fprintf(out, "  v%zu -- v%d;\n", v, r->graph[v][q]);
	fprintf(out, "}\n");
}
