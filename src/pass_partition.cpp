/* ============================================================================
 * pass_partition.cpp  --  iteration-space partitioning with owner-computes
 *
 * Compiler-theory concepts: data and computation decomposition for
 * distributed-memory machines (Aho/Lam/Sethi/Ullman 2nd ed., section 11.8
 * "Array-Data-Dependence"/11.9 and the "owner-computes rule" of High
 * Performance Fortran compilers; Gupta & Banerjee, "Compile-time estimation of
 * communication costs").
 *
 * The outermost parallel loop is distributed over the DPUs.  Every array is
 * viewed as a stack of ROWS (its first dimension).  OWNER-COMPUTES: iteration
 * i runs on the DPU that owns the row it writes, so a write whose first
 * subscript is  i + c  is local by construction.  For each READ reference the
 * pass compares the row it needs with the row the iteration owns:
 *
 *    identical access function            -> zero communication (local)
 *    differs by a constant delta          -> a HALO of |delta| rows from the
 *                                            neighbouring DPU (stencils)
 *    independent of the partitioned loop  -> read-only data every DPU needs:
 *                                            BROADCAST (replicated)
 *    anything else (a transpose is the    -> partitioning is IMPOSSIBLE without
 *    canonical case)                         an all-to-all shuffle; the nest
 *                                            falls back to the host, and the
 *                                            reason is recorded
 *
 * Reductions are partitioned by blocks of the reduction loop; the accumulator
 * becomes a PRIVATE copy per DPU, merged on the host afterwards.
 *
 * BLOCK, CYCLIC and BLOCK-CYCLIC distributions are each costed (halo bytes
 * plus the number of non-contiguous transfer runs, converted to byte-
 * equivalents) and the cheapest is chosen; ties go to BLOCK.
 * ==========================================================================*/
#include <cstdio>
#include <cstdlib>
#include <algorithm>
#include <map>
#include <string>
#include <vector>

#include "config.h"
#include "ir.h"
#include "analysis.h"
#include "profit.h"
#include "plan.h"

int g_force_dist = -1;

const char *dist_text(int d)
{
	return d == DIST_BLOCK ? "block" : (d == DIST_CYCLIC ? "cyclic" : "block-cyclic");
}

static long row_bytes(IRProgram *p, const std::string &name)
{
	ArrayInfo &ai = p->arrays[name];
	long n = ai.elem_bytes;
	size_t i;
	for (i = 1; i < ai.dims.size(); i++)
		n *= ai.dims[i];
	return n;
}

/* is this reference "row-structured": first subscript is  1*L + c  and L appears
 * in no other subscript? */
static bool row_structured(RefInfo &r)
{
	size_t k, j;
	if (!r.affine || r.F.empty())
		return false;
	if (r.F[0][0] != 1)
		return false;
	for (j = 1; j < r.loops.size(); j++)
		if (r.F[0][j] != 0)
			return false;
	for (k = 1; k < r.F.size(); k++)
		if (r.F[k][0] != 0)
			return false;
	return true;
}

static bool independent_of_root(RefInfo &r)
{
	size_t k;
	if (!r.affine)
		return false;
	for (k = 0; k < r.F.size(); k++)
		if (r.F[k][0] != 0)
			return false;
	return true;
}

static std::string ref_str(RefInfo &r)
{
	std::string t = r.name + "[";
	size_t k, j;
	for (k = 0; k < r.F.size(); k++) {
		if (k) t += ",";
		if (!r.affine) { t += "?"; continue; }
		std::string e;
		for (j = 0; j < r.loops.size(); j++)
			if (r.F[k][j] != 0) {
				if (!e.empty()) e += "+";
				e += r.loops[j]->index_var;
			}
		if (e.empty()) e = "const";
		t += e;
	}
	return t + "]";
}

bool partition_nest(IRProgram *p, NestAnalysis *na, const Decision *d, bool naive,
                    PartitionPlan *out)
{
	size_t i, k;

	out->feasible = false;
	out->reason.clear();
	out->arrays.clear();
	out->considered.clear();
	out->notes.clear();
	out->privatised = false;
	out->loop = na->root;
	out->trips = loop_trips(na->root);
	out->dist = DIST_BLOCK;
	out->block = 1;
	out->ndpus = 1;

	if (!naive && !d->analysable) {
		out->reason = d->why_not;
		return false;
	}
	int rootpar = na->par[na->root];
	if (rootpar == PAR_NO) {
		out->reason = "outermost loop " + na->root->index_var + " is serial";
		return false;
	}
	if (out->trips < 1) {
		out->reason = "loop never runs";
		return false;
	}

	/* the owner-computes driver offset: first row-structured array write */
	long driver = 0;
	bool have_driver = false;
	for (i = 0; i < na->refs.size() && !have_driver; i++) {
		RefInfo &r = na->refs[i];
		if (!r.is_scalar && r.is_write && r.red_group < 0 && row_structured(r)) {
			driver = r.c[0];
			have_driver = true;
		}
	}

	/* arrays in order of first appearance */
	std::vector<std::string> names;
	for (i = 0; i < na->refs.size(); i++)
		if (!na->refs[i].is_scalar &&
		    std::find(names.begin(), names.end(), na->refs[i].name) == names.end())
			names.push_back(na->refs[i].name);

	std::string fail;
	for (size_t n = 0; n < names.size(); n++) {
		const std::string &X = names[n];
		std::vector<int> refs;
		bool has_write = false, has_read = false, all_red = true, any_red = false;
		for (i = 0; i < na->refs.size(); i++)
			if (!na->refs[i].is_scalar && na->refs[i].name == X) {
				refs.push_back((int)i);
				if (na->refs[i].is_write) has_write = true; else has_read = true;
				if (na->refs[i].red_group >= 0) any_red = true; else all_red = false;
			}

		ArrayRole role;
		role.name = X;
		role.read = has_read;
		role.written = has_write;
		role.mode = AM_PARTITIONED;
		role.row_off = 0;
		role.row_coeff = 1;
		role.halo_lo = role.halo_hi = 0;
		role.local_only = false;
		role.red_kind = -1;

		/* a reduction accumulator that does not depend on the partitioned loop */
		if (any_red && all_red) {
			bool depends = false;
			for (k = 0; k < refs.size(); k++) {
				RefInfo &r = na->refs[refs[k]];
				if (r.affine && !independent_of_root(r))
					depends = true;
			}
			if (!depends) {
				role.mode = AM_PRIVATE;
				role.read = role.written = true;
				role.red_kind = na->reductions[na->refs[refs[0]].red_group].kind;
				out->privatised = true;
				out->arrays.push_back(role);
				out->notes.push_back(X + ": private accumulator (" +
				                     red_text(role.red_kind) + "), merged across DPUs");
				continue;
			}
		}

		bool structured = true;
		long minc = 0, maxc = 0;
		bool first = true;
		for (k = 0; k < refs.size(); k++) {
			RefInfo &r = na->refs[refs[k]];
			if (!row_structured(r)) {
				structured = false;
				continue;
			}
			if (first || r.c[0] < minc) minc = r.c[0];
			if (first || r.c[0] > maxc) maxc = r.c[0];
			first = false;
		}

		if (structured) {
			long base = have_driver ? driver : 0;
			if (has_write) {
				for (k = 0; k < refs.size(); k++)
					if (na->refs[refs[k]].is_write) {
						base = na->refs[refs[k]].c[0];
						break;
					}
			}
			role.mode = AM_PARTITIONED;
			role.row_off = base;
			role.halo_lo = std::max(0L, base - minc);
			role.halo_hi = std::max(0L, maxc - base);
			char b[160];
			if (role.halo_lo || role.halo_hi)
				snprintf(b, sizeof b, "%s: partitioned by rows, halo %ld below + %ld above (stencil)",
				         X.c_str(), role.halo_lo, role.halo_hi);
			else
				snprintf(b, sizeof b, "%s: partitioned by rows, identical access function (no communication)",
				         X.c_str());
			out->notes.push_back(b);
			out->arrays.push_back(role);
			continue;
		}

		/* not row-structured */
		bool none_involve = true;
		for (k = 0; k < refs.size(); k++)
			if (!independent_of_root(na->refs[refs[k]]))
				none_involve = false;

		if (none_involve && !has_write) {
			role.mode = AM_BROADCAST;
			out->notes.push_back(X + ": read-only, independent of " + na->root->index_var +
			                     " -> broadcast to every DPU");
			out->arrays.push_back(role);
			continue;
		}

		if (naive) {
			/* the naive offloader ignores ownership: replicate what it reads,
			 * scatter what it writes, and let the host sort it out */
			role.mode = has_write ? AM_SCATTER : AM_BROADCAST;
			out->notes.push_back(X + ": no clean partition (" + ref_str(na->refs[refs[0]]) +
			                     ") -> " + (has_write ? "scattered writes" : "replicated"));
			out->arrays.push_back(role);
			continue;
		}

		/* feasibility failure: say precisely why */
		for (k = 0; k < refs.size(); k++) {
			RefInfo &r = na->refs[refs[k]];
			if (row_structured(r))
				continue;
			if (r.is_write)
				fail = "write " + ref_str(r) + ": its row subscript does not follow loop " +
				       na->root->index_var + ", so one iteration writes rows owned by other DPUs";
			else
				fail = "read " + ref_str(r) + ": the rows it needs are not the rows this iteration owns "
				       "(not a constant shift) -> needs an all-to-all shuffle between DPUs";
			break;
		}
		out->reason = fail;
		return false;
	}

	/* ---- iteration distribution --------------------------------------------- */
	long T = out->trips;
	int P = (int)std::min<long>(NUM_DPUS, T);
	long halo_rows = 0;
	long halo_row_bytes = 0;
	long narr = 0;
	for (i = 0; i < out->arrays.size(); i++) {
		ArrayRole &r = out->arrays[i];
		if (r.mode == AM_PARTITIONED) {
			narr++;
			halo_rows += r.halo_lo + r.halo_hi;
			halo_row_bytes += (r.halo_lo + r.halo_hi) * row_bytes(p, r.name);
		}
	}
	double run_cost = (double)DMA_RUN_SETUP / (double)DMA_PER_BYTE;

	DistCost c;
	/* block */
	c.kind = DIST_BLOCK;
	c.block = (T + P - 1) / P;
	c.halo_bytes = (double)(P - 1) * (double)halo_row_bytes;
	c.runs = (long)P * narr;
	c.score = c.halo_bytes + (double)c.runs * run_cost;
	out->considered.push_back(c);
	/* cyclic */
	c.kind = DIST_CYCLIC;
	c.block = 1;
	c.halo_bytes = (P > 1) ? (double)T * (double)halo_row_bytes : 0.0;
	c.runs = T * narr;
	c.score = c.halo_bytes + (double)c.runs * run_cost;
	out->considered.push_back(c);
	/* block-cyclic */
	long B = std::max(1L, std::min(16L, (T + P - 1) / P / 4));
	c.kind = DIST_BLOCK_CYCLIC;
	c.block = B;
	long chunks = (T + B - 1) / B;
	c.halo_bytes = (P > 1) ? (double)chunks * (double)halo_row_bytes : 0.0;
	c.runs = chunks * narr;
	c.score = c.halo_bytes + (double)c.runs * run_cost;
	out->considered.push_back(c);

	size_t best = 0;
	for (i = 1; i < out->considered.size(); i++)
		if (out->considered[i].score < out->considered[best].score)
			best = i;
	if (g_force_dist >= 0)
		for (i = 0; i < out->considered.size(); i++)
			if (out->considered[i].kind == g_force_dist)
				best = i;
	out->dist = out->considered[best].kind;
	out->block = out->considered[best].block;

	if (out->dist == DIST_BLOCK)
		out->ndpus = (int)((T + out->block - 1) / out->block);
	else
		out->ndpus = P;

	out->feasible = true;
	char b[160];
	snprintf(b, sizeof b, "%s distribution over %d DPUs (%ld iterations)", dist_text(out->dist),
	         out->ndpus, T);
	out->reason = b;
	(void)halo_rows;
	return true;
}

/* iteration counters 0..T-1 owned by DPU d under the chosen distribution */
void partition_iterations(const PartitionPlan *pp, int dpu, std::vector<long> *out)
{
	long T = pp->trips;
	long it;
	out->clear();
	if (pp->dist == DIST_BLOCK) {
		long lo = (long)dpu * pp->block;
		long hi = std::min(T, lo + pp->block);
		for (it = lo; it < hi; it++)
			out->push_back(it);
	} else if (pp->dist == DIST_CYCLIC) {
		for (it = dpu; it < T; it += pp->ndpus)
			out->push_back(it);
	} else {
		long B = pp->block;
		long chunk;
		for (chunk = dpu; chunk * B < T; chunk += pp->ndpus)
			for (it = chunk * B; it < std::min(T, chunk * B + B); it++)
				out->push_back(it);
	}
}

void partition_print(const PartitionPlan *pp)
{
	size_t i;
	if (!pp->feasible) {
		printf("  partition: IMPOSSIBLE -- %s\n", pp->reason.c_str());
		return;
	}
	printf("  partition: loop %s, %s distribution over %d DPUs (%ld iterations)\n",
	       pp->loop->index_var.c_str(), dist_text(pp->dist), pp->ndpus, pp->trips);
	for (i = 0; i < pp->notes.size(); i++)
		printf("    %s\n", pp->notes[i].c_str());
	printf("    distributions considered:\n");
	for (i = 0; i < pp->considered.size(); i++) {
		const DistCost &c = pp->considered[i];
		printf("      %-13s halo %10.0f B, %6ld transfer runs, score %10.0f%s\n",
		       dist_text(c.kind), c.halo_bytes, c.runs, c.score,
		       c.kind == pp->dist ? "   <- chosen" : "");
	}
}
