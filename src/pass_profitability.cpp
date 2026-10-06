/* ============================================================================
 * pass_profitability.cpp  --  the offload profitability analysis
 *
 * Compiler-theory concepts combined here:
 *   - the ROOFLINE model: a kernel is memory-bound when its arithmetic
 *     intensity (flops per byte moved) is below the machine's balance point;
 *     only memory-bound kernels gain from computing next to the memory;
 *   - data-reuse analysis (Wolf & Lam 1991, "A Data Locality Optimizing
 *     Algorithm"; ALSU 2nd ed. section 11.5): self-temporal, self-spatial and
 *     group-temporal reuse read directly off the access matrices F;
 *   - parallelism read off the direction vectors of Phase D (a loop is parallel
 *     exactly when it carries no dependence), NOT from syntax patterns.
 *
 * The decision rule is the project's contribution:
 *
 *   OFFLOAD  iff  AI < MACHINE_BALANCE
 *            and  no reference exhibits significant self-temporal reuse
 *            and  the outermost loop is parallel (or a recognised reduction)
 *            and  the class is not IRREGULAR
 *
 * FLOPs   arithmetic operations per iteration times the trip counts of the
 *         loops around them (comparisons are not counted).
 * BYTES   per distinct stream of references (same array, same F, same
 *         direction): the footprint (distinct elements touched) times the bytes
 *         each element costs on the bus: elem_bytes when the access is
 *         contiguous or the whole working set stays in the host cache; the
 *         stride (capped at a full CACHE_LINE) when it is not.  Reuse that a
 *         cache would capture does not add traffic -- which is exactly why
 *         reuse-heavy kernels like matmul come out with a HIGH intensity.
 * REUSE   self-temporal: a column of F is all zeros, so that loop re-reads the
 *         same element.  It only blocks offload when that stream carries at
 *         least REUSE_SIGNIFICANCE of the traffic: in a matrix-vector product
 *         the small vectors are reused but the matrix, which is nearly all the
 *         traffic, is not (see the note in docs/ and README on this refinement
 *         of the course rule).
 * ==========================================================================*/
#include <cstdio>
#include <cstdlib>
#include <algorithm>
#include <map>
#include <set>
#include <string>
#include <vector>

#include "config.h"
#include "ir.h"
#include "analysis.h"
#include "profit.h"

const char *class_text(int c)
{
	switch (c) {
	case PC_ELEMENTWISE:    return "ELEMENTWISE";
	case PC_REDUCTION:      return "REDUCTION";
	case PC_STENCIL:        return "STENCIL";
	case PC_COMPUTE_BOUND:  return "COMPUTE_BOUND";
	default:                return "IRREGULAR";
	}
}

/* A temporary is an ADDRESS value when every use of it is an array subscript or
 * an operand of another address computation: i-1 in A[i-1] is index arithmetic,
 * not a floating-point operation, and must not be counted as one. */
static void find_address_temps(IRProgram *p, LoopNest *root, std::set<std::string> *addr)
{
	std::vector<IRStmt*> all;
	ir_collect_stmts(root->body, &all);
	size_t i, j;
	bool changed = true;

	for (std::map<std::string, IVInfo>::iterator it = p->ivs.begin(); it != p->ivs.end(); it++)
		addr->insert(it->first);

	while (changed) {
		changed = false;
		for (i = 0; i < all.size(); i++) {
			IRStmt *s = all[i];
			if ((s->kind != IR_BINOP && s->kind != IR_COPY) || !is_temp_name(s->dst) ||
			    addr->count(s->dst))
				continue;
			bool any_use = false, all_addr = true;
			for (j = 0; j < all.size(); j++) {
				IRStmt *u = all[j];
				size_t k;
				for (k = 0; k < u->idx.size(); k++)
					if (u->idx[k].value.kind == OPD_NAME && u->idx[k].value.name == s->dst)
						any_use = true;           /* a subscript use: address */
				std::vector<std::string> uses;
				if (u->kind == IR_BINOP || u->kind == IR_COPY || u->kind == IR_PRINT) {
					if (u->kind == IR_BINOP) {
						if (u->lhs.kind == OPD_NAME && u->lhs.name == s->dst) uses.push_back(s->dst);
						if (u->rhs.kind == OPD_NAME && u->rhs.name == s->dst) uses.push_back(s->dst);
					} else if (u->src.kind == OPD_NAME && u->src.name == s->dst) {
						uses.push_back(s->dst);
					}
					if (!uses.empty()) {
						any_use = true;
						if (!addr->count(u->dst))
							all_addr = false;
					}
				}
				if (u->kind == IR_ARRAY_STORE && u->src.kind == OPD_NAME && u->src.name == s->dst) {
					any_use = true;
					all_addr = false;                 /* stored data */
				}
			}
			if (any_use && all_addr) {
				addr->insert(s->dst);
				changed = true;
			}
		}
	}
}

static double flops_body(std::vector<BodyItem> &body, double w, std::set<std::string> &addr)
{
	double sum = 0;
	size_t i;
	for (i = 0; i < body.size(); i++) {
		BodyItem &it = body[i];
		if (it.kind == BK_STMT) {
			IRStmt *s = it.stmt;
			if (s->kind == IR_BINOP && s->op <= OP_MOD && !addr.count(s->dst))
				sum += w;
		} else if (it.kind == BK_IF) {
			sum += flops_body(it.ifn->then_body, w, addr);
			sum += flops_body(it.ifn->else_body, w, addr);
		} else {
			sum += flops_body(it.loop->body, w * (double)loop_trips(it.loop), addr);
		}
	}
	return sum;
}

static long array_elems(const ArrayInfo &ai)
{
	long n = 1;
	size_t i;
	for (i = 0; i < ai.dims.size(); i++)
		n *= ai.dims[i];
	return n;
}

/* row-major stride (in elements) of dimension k */
static long dim_stride(const ArrayInfo &ai, size_t k)
{
	long s = 1;
	size_t d;
	for (d = k + 1; d < ai.dims.size(); d++)
		s *= ai.dims[d];
	return s;
}

/* exact number of distinct elements a stream of references touches, found by
 * walking the iteration space (only loops whose index appears in a subscript
 * change the address) and marking each element in a bitmap of the array */
static long stream_footprint(IRProgram *p, NestAnalysis *na, std::vector<int> &refs, long fallback)
{
	RefInfo &r0 = na->refs[refs[0]];
	std::map<std::string, ArrayInfo>::iterator ai = p->arrays.find(r0.name);
	if (ai == p->arrays.end())
		return fallback;
	long total = 1;
	size_t i, j, k;
	for (i = 0; i < ai->second.dims.size(); i++)
		total *= ai->second.dims[i];

	/* the loops that matter, and a work estimate */
	std::vector<size_t> cols;
	double work = (double)refs.size();
	for (j = 0; j < r0.loops.size(); j++) {
		bool nz = false;
		for (k = 0; k < r0.F.size(); k++)
			if (r0.F[k][j] != 0)
				nz = true;
		if (nz) {
			cols.push_back(j);
			work *= (double)std::max(1L, loop_trips(r0.loops[j]));
		}
	}
	if (work > 4000000.0)
		return fallback;

	std::vector<char> seen((size_t)total, 0);
	long count = 0;
	std::vector<long> n(cols.size(), 0);
	for (;;) {
		for (i = 0; i < refs.size(); i++) {
			RefInfo &r = na->refs[refs[i]];
			long lin = 0;
			bool inside = true;
			for (k = 0; k < r.F.size(); k++) {
				long row = r.c[k];
				for (size_t c = 0; c < cols.size(); c++) {
					LoopNest *L = r.loops[cols[c]];
					row += r.F[k][cols[c]] * (L->lower + L->step * n[c]);
				}
				if (row < 0 || row >= ai->second.dims[k])
					inside = false;
				lin = lin * ai->second.dims[k] + row;
			}
			if (inside && !seen[(size_t)lin]) {
				seen[(size_t)lin] = 1;
				count++;
			}
		}
		/* odometer */
		size_t c = 0;
		for (; c < cols.size(); c++) {
			n[c]++;
			if (n[c] < std::max(1L, loop_trips(r0.loops[cols[c]])))
				break;
			n[c] = 0;
		}
		if (c == cols.size())
			break;
	}
	return count;
}

static std::string ref_key(RefInfo &r)
{
	std::string k = r.name + (r.is_write ? "|w|" : "|r|");
	size_t i, j;
	if (!r.affine)
		return k + "nonaffine";
	for (i = 0; i < r.F.size(); i++) {
		for (j = 0; j < r.F[i].size(); j++) {
			char buf[24];
			snprintf(buf, sizeof buf, "%ld,", r.F[i][j]);
			k += buf;
		}
		k += ";";
	}
	return k;
}

static std::string ref_label(RefInfo &r)
{
	std::string t = r.name + "[";
	size_t k, j;
	for (k = 0; k < r.F.size(); k++) {
		if (k) t += ", ";
		if (!r.affine) { t += "?"; continue; }
		std::string e;
		for (j = 0; j < r.loops.size(); j++)
			if (r.F[k][j] != 0) {
				if (!e.empty()) e += "+";
				if (r.F[k][j] != 1) e += std::to_string(r.F[k][j]) + "*";
				e += r.loops[j]->index_var;
			}
		if (e.empty()) e = "0";
		t += e;
	}
	return t + "]";
}

static std::string join_names(const std::vector<std::string> &v)
{
	std::string s;
	size_t i;
	for (i = 0; i < v.size(); i++)
		s += (i ? "," : "") + v[i];
	return s;
}

static void add_unique(std::vector<std::string> &v, const std::string &s)
{
	if (std::find(v.begin(), v.end(), s) == v.end())
		v.push_back(s);
}

void profitability(IRProgram *p, NestAnalysis *na, const std::string &name, Decision *d)
{
	size_t i, j;

	d->name = name;
	d->flops = 0;
	d->bytes = 0;
	d->ai = 0;
	d->groups.clear();
	d->temporal.clear();
	d->temporal_sig.clear();
	d->spatial.clear();
	d->group_temporal.clear();
	d->fails.clear();
	d->analysable = na->analysable;
	d->why_not = na->why_not;
	d->float_reduction = false;
	d->offload = false;

	/* ---- FLOPs ----------------------------------------------------------- */
	std::set<std::string> addr;
	find_address_temps(p, na->root, &addr);
	d->flops = flops_body(na->root->body, (double)loop_trips(na->root), addr);

	/* ---- parallelism from the direction vectors ----------------------------- */
	d->par_loop = 0;
	d->par_kind = PAR_NO;
	d->par_dirs.clear();
	for (i = 0; i < na->loops.size(); i++) {
		int k = na->par[na->loops[i]];
		if (i) d->par_dirs += ",";
		d->par_dirs += (k == PAR_YES) ? "=" : (k == PAR_REDUCTION ? "R" : "<");
		if (!d->par_loop && k != PAR_NO) {
			d->par_loop = na->loops[i];
			d->par_kind = k;
		}
	}
	d->par_outermost = (d->par_loop == na->root);

	/* a float sum/product is only a problem when the reduction loop itself is
	 * split across DPUs (the partial sums would be added in a different order);
	 * an inner reduction that stays inside one DPU keeps its sequential order */
	if (na->par[na->root] == PAR_REDUCTION)
		for (i = 0; i < na->reductions.size(); i++)
			if (na->reductions[i].is_float &&
			    (na->reductions[i].kind == RED_SUM || na->reductions[i].kind == RED_PROD))
				d->float_reduction = true;

	/* ---- bytes moved and reuse, per stream of references ------------------- */
	std::map<std::string, std::vector<int> > members;
	for (i = 0; i < na->refs.size(); i++)
		if (!na->refs[i].is_scalar)
			members[ref_key(na->refs[i])].push_back((int)i);

	std::map<std::string, int> seen;
	for (i = 0; i < na->refs.size(); i++) {
		RefInfo &r = na->refs[i];
		if (r.is_scalar)
			continue;
		std::string key = ref_key(r);
		if (seen.count(key))
			continue;
		seen[key] = (int)d->groups.size();

		std::map<std::string, ArrayInfo>::iterator ai = p->arrays.find(r.name);
		if (ai == p->arrays.end())
			continue;
		long total = array_elems(ai->second);
		int ebytes = ai->second.elem_bytes;

		RefGroupCost g;
		g.array = r.name;
		g.is_write = r.is_write;
		g.nonaffine = !r.affine;
		g.text = ref_label(r);
		g.temporal = false;
		g.reuse_factor = 1;
		g.significant = false;
		g.spatial = false;
		g.stride_bytes = 0;
		g.bytes = 0;

		long foot = 1;
		int jv = -1;
		if (!r.affine) {
			for (j = 0; j < r.loops.size(); j++)
				foot *= std::max(1L, loop_trips(r.loops[j]));
		} else {
			for (j = 0; j < r.loops.size(); j++) {
				bool nz = false;
				size_t k;
				for (k = 0; k < r.F.size(); k++)
					if (r.F[k][j] != 0)
						nz = true;
				long trips = loop_trips(r.loops[j]);
				if (nz) {
					foot *= std::max(1L, trips);
					jv = (int)j;
				} else if (trips > 1) {
					g.temporal = true;
					g.reuse_factor *= trips;
				}
			}
			if (jv >= 0) {
				long se = 0;
				size_t k;
				for (k = 0; k < r.F.size(); k++)
					se += r.F[k][jv] * dim_stride(ai->second, k);
				g.stride_bytes = labs(se) * r.loops[jv]->step * ebytes;

				/* self-spatial: the innermost index appears only in the last
				 * dimension, with coefficient 1 */
				bool only_last = true;
				for (k = 0; k + 1 < r.F.size(); k++)
					if (r.F[k][jv] != 0)
						only_last = false;
				if (only_last && !r.F.empty() && r.F.back()[jv] == 1 && jv == (int)r.loops.size() - 1)
					g.spatial = true;
			}
		}
		g.footprint = std::min(foot, total);
		if (r.affine)
			g.footprint = std::min(stream_footprint(p, na, members[key], g.footprint), total);
		d->groups.push_back(g);
	}

	long ws = 0;
	for (i = 0; i < d->groups.size(); i++)
		ws += d->groups[i].footprint * ELEM_BYTES;
	bool fits = ws <= HOST_CACHE_BYTES;

	for (i = 0; i < d->groups.size(); i++) {
		RefGroupCost &g = d->groups[i];
		ArrayInfo &ai = p->arrays[g.array];
		double per_elem = ai.elem_bytes;
		if (g.nonaffine) {
			long abytes = array_elems(ai) * ai.elem_bytes;
			per_elem = (abytes <= HOST_CACHE_BYTES) ? ai.elem_bytes : CACHE_LINE;
		} else if (!fits && g.stride_bytes > ai.elem_bytes) {
			per_elem = std::min<double>(g.stride_bytes, CACHE_LINE);
		}
		g.bytes = (double)g.footprint * per_elem;
		d->bytes += g.bytes;
	}
	d->ai = d->bytes > 0 ? d->flops / d->bytes : 0.0;

	/* ---- reuse classification ------------------------------------------------ */
	for (i = 0; i < d->groups.size(); i++) {
		RefGroupCost &g = d->groups[i];
		if (g.temporal) {
			add_unique(d->temporal, g.array);
			g.significant = d->bytes > 0 && g.bytes / d->bytes >= REUSE_SIGNIFICANCE;
			if (g.significant)
				add_unique(d->temporal_sig, g.array);
		}
		if (g.spatial)
			add_unique(d->spatial, g.array);
	}

	/* group-temporal: two references to one array with the same F, differing offsets */
	bool halo_on_parallel = false;
	for (i = 0; i < na->refs.size(); i++) {
		for (j = i + 1; j < na->refs.size(); j++) {
			RefInfo &a = na->refs[i];
			RefInfo &b = na->refs[j];
			if (a.is_scalar || b.is_scalar || a.name != b.name || !a.affine || !b.affine)
				continue;
			if (a.F != b.F || a.c == b.c)
				continue;
			bool small = true;
			size_t k;
			for (k = 0; k < a.c.size(); k++)
				if (labs(a.c[k] - b.c[k]) > 8)
					small = false;
			if (!small)
				continue;
			add_unique(d->group_temporal, a.name);
			for (k = 0; k < a.c.size(); k++)
				if (a.c[k] != b.c[k] && !a.F[k].empty() && a.F[k][0] != 0)
					halo_on_parallel = true;
		}
	}

	/* ---- parallelism class ---------------------------------------------------- */
	bool compute_bound = d->ai >= MACHINE_BALANCE || !d->temporal_sig.empty();
	if (!d->analysable || d->par_loop == 0)
		d->pclass = PC_IRREGULAR;
	else if (compute_bound)
		d->pclass = PC_COMPUTE_BOUND;
	else if (d->par_outermost && d->par_kind == PAR_REDUCTION)
		d->pclass = PC_REDUCTION;
	else if (halo_on_parallel)
		d->pclass = PC_STENCIL;
	else
		d->pclass = PC_ELEMENTWISE;

	/* ---- the decision rule ------------------------------------------------------ */
	if (!d->analysable) {
		d->fails.push_back(d->why_not);
	} else {
		if (d->par_loop == 0) {
			std::string why = "no loop is parallel";
			for (i = 0; i < na->deps.size(); i++)
				if (na->deps[i].level > 0 && !na->deps[i].reduction) {
					const char *k = na->deps[i].kind == DEP_FLOW ? "flow" :
					                (na->deps[i].kind == DEP_ANTI ? "anti" : "output");
					why = std::string("loop ") + na->deps[i].common[na->deps[i].level - 1]->index_var +
					      " carries a " + k + " dependence on " + na->deps[i].var;
					break;
				}
			d->fails.push_back(why);
		} else if (!d->par_outermost) {
			d->fails.push_back("outermost loop " + na->root->index_var +
			                   " is serial; only inner loop " + d->par_loop->index_var +
			                   " is parallel (needs interchange)");
		}
		if (d->ai >= MACHINE_BALANCE) {
			char b[96];
			snprintf(b, sizeof b, "AI=%.2f >= %.2f (compute-bound)", d->ai, MACHINE_BALANCE);
			d->fails.push_back(b);
		}
		if (!d->temporal_sig.empty())
			d->fails.push_back("self-temporal reuse on " + join_names(d->temporal_sig));
		if (d->float_reduction)
			d->fails.push_back("float reduction would be reassociated (not bit-identical)");
	}
	d->offload = d->fails.empty();

	/* ---- the one-line reason, in the course's format --------------------------- */
	std::string r;
	char b[96];
	if (!d->analysable) {
		r = d->why_not;
	} else {
		snprintf(b, sizeof b, "AI=%.2f %s %.2f", d->ai, d->ai < MACHINE_BALANCE ? "< " : ">=",
		         MACHINE_BALANCE);
		r = b;
		if (!d->temporal_sig.empty())
			r += "; self-temporal reuse on " + join_names(d->temporal_sig);
		else if (!d->temporal.empty())
			r += "; negligible temporal reuse (" + join_names(d->temporal) + ")";
		else
			r += "; no temporal reuse";
		if (d->par_loop != 0 && d->par_outermost)
			r += std::string("; ") + (d->par_kind == PAR_REDUCTION ? "reduction-parallel" : "parallel") +
			     "(dir=[" + d->par_dirs + "])";
		else if (d->par_loop != 0)
			r += "; serial outer loop(dir=[" + d->par_dirs + "])";
		else
			r += "; NOT parallel(dir=[" + d->par_dirs + "])";
		if (d->float_reduction)
			r += "; float reduction";
	}
	r += std::string("; class=") + class_text(d->pclass);
	d->reason = r;
}

std::string decision_line(const Decision *d)
{
	char b[512];
	snprintf(b, sizeof b, "%-12s %-8s %s", d->name.c_str(), d->offload ? "OFFLOAD" : "REJECT",
	         d->reason.c_str());
	return std::string(b);
}

/* --------------------------------------------------------------------------
 * --explain : the complete decision trace in plain English
 * ------------------------------------------------------------------------*/
void explain_decision(IRProgram *p, NestAnalysis *na, const Decision *d)
{
	size_t i;
	(void)p;

	printf("==== why `%s` is %s ====\n\n", d->name.c_str(), d->offload ? "OFFLOADED" : "REJECTED");

	printf("Step 1. The loop nest.\n");
	for (i = 0; i < na->loops.size(); i++)
		printf("   loop %s runs %ld iteration%s (from %ld, step %ld)\n",
		       na->loops[i]->index_var.c_str(), loop_trips(na->loops[i]),
		       loop_trips(na->loops[i]) == 1 ? "" : "s", na->loops[i]->lower, na->loops[i]->step);

	printf("\nStep 2. Can the compiler analyse the memory accesses?\n");
	if (!d->analysable) {
		printf("   NO: %s.\n", d->why_not.c_str());
		printf("   The dependence tests need every subscript to be a linear function of the\n"
		       "   loop indices (an access matrix F and offset c). A data-dependent subscript\n"
		       "   cannot be tested, so the compiler must assume the worst: class IRREGULAR.\n");
		printf("\nVerdict: REJECT.  %s\n", d->reason.c_str());
		return;
	}
	printf("   Yes: all %d array/scalar references are affine.\n", (int)na->refs.size());

	printf("\nStep 3. Arithmetic: %.0f floating-point/integer operations in total.\n", d->flops);
	printf("   (every +,-,*,/,%% counted once per iteration of the loops around it;\n"
	       "    comparisons are not counted)\n");

	printf("\nStep 4. Memory traffic, one line per stream of references:\n");
	printf("   %-12s %-6s %10s %12s %12s\n", "reference", "dir", "elements", "stride(B)", "bytes");
	for (i = 0; i < d->groups.size(); i++) {
		const RefGroupCost &g = d->groups[i];
		printf("   %-12s %-6s %10ld %12ld %12.0f\n", g.text.c_str(), g.is_write ? "write" : "read",
		       g.footprint, g.stride_bytes, g.bytes);
	}
	printf("   total %.0f bytes.  A cache captures any element that is re-read, so only the\n"
	       "   distinct elements (the footprint) cross the bus; a stride larger than a cache\n"
	       "   line (%d B) drags in a whole line per element when the data does not fit the\n"
	       "   %d-byte cache.\n", d->bytes, CACHE_LINE, HOST_CACHE_BYTES);

	printf("\nStep 5. Arithmetic intensity = %.0f / %.0f = %.3f flops per byte.\n", d->flops,
	       d->bytes, d->ai);
	printf("   The machine balance is %.2f: below it the kernel is memory-bound (PIM helps),\n"
	       "   above it the kernel is compute-bound (the host's caches and ALUs win).\n",
	       (double)MACHINE_BALANCE);
	printf("   %.3f is %s %.2f  ->  %s\n", d->ai, d->ai < MACHINE_BALANCE ? "below" : "NOT below",
	       (double)MACHINE_BALANCE, d->ai < MACHINE_BALANCE ? "memory-bound" : "compute-bound");

	printf("\nStep 6. Data reuse (Wolf & Lam), read off the access matrices.\n");
	if (d->temporal.empty())
		printf("   No reference has an all-zero column in F: no loop re-reads the same element.\n");
	for (i = 0; i < d->groups.size(); i++) {
		const RefGroupCost &g = d->groups[i];
		if (!g.temporal)
			continue;
		printf("   self-temporal reuse: %s is re-read %ld times (a loop's column of F is zero);\n"
		       "      it carries %.0f%% of the traffic -> %s\n", g.text.c_str(), g.reuse_factor,
		       d->bytes > 0 ? 100.0 * g.bytes / d->bytes : 0.0,
		       g.significant ? "SIGNIFICANT (a cache would capture it)" : "negligible, ignored");
	}
	if (!d->spatial.empty())
		printf("   self-spatial reuse (stride-1 innermost): %s\n", join_names(d->spatial).c_str());
	if (!d->group_temporal.empty())
		printf("   group-temporal reuse (neighbouring elements): %s\n",
		       join_names(d->group_temporal).c_str());

	printf("\nStep 7. Parallelism, from the dependence direction vectors.\n");
	printf("   %d dependence%s found.\n", (int)na->deps.size(), na->deps.size() == 1 ? "" : "s");
	for (i = 0; i < na->loops.size(); i++) {
		int k = na->par[na->loops[i]];
		printf("   loop %-3s : %s\n", na->loops[i]->index_var.c_str(),
		       k == PAR_YES ? "no dependence is carried by this loop -> PARALLEL" :
		       (k == PAR_REDUCTION ? "carries only reduction dependences -> parallel with private accumulators" :
		                             "carries a dependence -> SERIAL"));
	}
	for (i = 0; i < na->reductions.size(); i++)
		printf("   recognised reduction: %s of %s\n", red_text(na->reductions[i].kind),
		       na->reductions[i].var.c_str());

	printf("\nStep 8. Class: %s.\n", class_text(d->pclass));

	printf("\nStep 9. The decision rule.\n");
	printf("   [%s] AI < machine balance\n", d->ai < MACHINE_BALANCE ? "pass" : "FAIL");
	printf("   [%s] no significant self-temporal reuse\n", d->temporal_sig.empty() ? "pass" : "FAIL");
	printf("   [%s] outermost loop is parallel or a reduction\n",
	       (d->par_loop != 0 && d->par_outermost) ? "pass" : "FAIL");
	printf("   [%s] class is not IRREGULAR\n", d->pclass != PC_IRREGULAR ? "pass" : "FAIL");
	if (d->float_reduction)
		printf("   [FAIL] float reduction would be reassociated (results would not be bit-identical)\n");
	bool part_fail = false;
	for (i = 0; i < d->fails.size(); i++)
		if (d->fails[i].compare(0, 22, "partitioning impossible") == 0) {
			printf("   [FAIL] the nest can be partitioned across the DPUs (owner-computes)\n"
			       "          %s\n", d->fails[i].c_str() + 24);
			part_fail = true;
		}
	if (!part_fail && d->offload)
		printf("   [pass] the nest can be partitioned across the DPUs (owner-computes)\n");

	printf("\nVerdict: %s.  %s\n", d->offload ? "OFFLOAD" : "REJECT", d->reason.c_str());
}
