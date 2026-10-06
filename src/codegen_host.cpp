/* ============================================================================
 * codegen_host.cpp  --  host orchestration code (plain C with printf tracing)
 *
 * For every kernel with an offloaded nest this emits  <kernel>_host.c  holding
 * one function, nemoc_call_<kernel>(), that does what the `call` statement means
 * on the near-memory machine, nest by nest and in program order:
 *
 *   - the flush(A) / invalidate(A) operations Phase G placed (host cache <->
 *     DRAM coherence),
 *   - for an OFFLOADED nest: allocate the DPUs, load the DPU program, PARTITION
 *     SETUP (each DPU's first iteration, count and stride), DMA IN the arrays
 *     each DPU reads (its rows plus halo, broadcast arrays whole), launch,
 *     synchronise, DMA OUT the results, merge private accumulators;
 *   - for a nest kept on the host: the loops themselves, as ordinary C.
 *
 * The file is self-contained: a few lines of "trace runtime" stand in for the
 * vendor's DPU API and print each operation, and compiling with
 * -DNEMOC_STANDALONE adds a main() so the orchestration can be built and run on
 * its own to see the sequence of events.
 * ==========================================================================*/
#include <cstdio>
#include <algorithm>
#include <map>
#include <set>
#include <string>
#include <vector>

#include "config.h"
#include "ir.h"
#include "analysis.h"
#include "plan.h"
#include "dpu.h"

static std::string fmt(const char *f, ...) __attribute__((format(printf, 1, 2)));
#include <cstdarg>
static std::string fmt(const char *f, ...)
{
	char buf[1024];
	va_list ap;
	va_start(ap, f);
	vsnprintf(buf, sizeof buf, f, ap);
	va_end(ap);
	return std::string(buf);
}

static std::string cname(const std::string &n)
{
	std::string r;
	size_t i;
	for (i = 0; i < n.size(); i++)
		r += (n[i] == '%') ? '_' : n[i];
	return r;
}

static const char *ctype(TypeTag t)
{
	return t == TY_FLOAT ? "float" : "long";
}

static std::string c_opd(const Operand &o)
{
	if (o.kind == OPD_INT)
		return fmt("%ldL", o.ival);
	if (o.kind == OPD_FLOAT)
		return fmt("%.9gf", (double)(float)o.dval);
	return cname(o.name);
}

static std::string c_index(IRStmt *s)
{
	std::string r;
	size_t k;
	for (k = 0; k < s->idx.size(); k++)
		r += "[" + c_opd(s->idx[k].value) + "]";
	return r;
}

static void collect_temps(std::vector<BodyItem> &body, std::map<std::string, TypeTag> *t)
{
	std::vector<IRStmt*> all;
	size_t i;
	ir_collect_stmts(body, &all);
	for (i = 0; i < all.size(); i++) {
		std::string d = ir_stmt_def(all[i]);
		if (is_temp_name(d))
			(*t)[d] = all[i]->dst_type;
	}
}

static void c_body(IRProgram *p, std::vector<BodyItem> &body, int ind, std::string *out);

static void c_stmt(IRProgram *p, IRStmt *s, int ind, std::string *out)
{
	std::string pad((size_t)ind * 4, ' ');
	(void)p;
	switch (s->kind) {
	case IR_ARRAY_LOAD:
		*out += pad + cname(s->dst) + " = " + s->array + c_index(s) + ";\n";
		break;
	case IR_ARRAY_STORE:
		*out += pad + s->array + c_index(s) + " = " + c_opd(s->src) + ";\n";
		break;
	case IR_BINOP: {
		std::string a = c_opd(s->lhs), b = c_opd(s->rhs);
		bool intdiv = (s->op == OP_DIV || s->op == OP_MOD) && s->dst_type == TY_INT &&
		              s->lhs.type == TY_INT && s->rhs.type == TY_INT;
		if (intdiv)
			*out += pad + cname(s->dst) + " = (" + b + " != 0) ? (" + a + " " + op_str(s->op) + " " + b + ") : 0;\n";
		else
			*out += pad + cname(s->dst) + " = " + a + " " + op_str(s->op) + " " + b + ";\n";
		break;
	}
	case IR_COPY:
		*out += pad + cname(s->dst) + " = " + c_opd(s->src) + ";\n";
		break;
	case IR_PRINT:
		if (s->src.type == TY_FLOAT)
			*out += pad + "printf(\"%g\\n\", (double)" + c_opd(s->src) + ");\n";
		else
			*out += pad + "printf(\"%ld\\n\", (long)" + c_opd(s->src) + ");\n";
		break;
	case IR_FLUSH:
		*out += pad + "cache_flush(\"" + s->array + "\", " + "sizeof " + s->array + ");\n";
		break;
	case IR_INVALIDATE:
		*out += pad + "cache_invalidate(\"" + s->array + "\", " + "sizeof " + s->array + ");\n";
		break;
	case IR_CALL:
		*out += pad + "nemoc_call_" + s->callee + "();\n";
		break;
	}
}

static void c_body(IRProgram *p, std::vector<BodyItem> &body, int ind, std::string *out)
{
	std::string pad((size_t)ind * 4, ' ');
	size_t i;
	for (i = 0; i < body.size(); i++) {
		BodyItem &it = body[i];
		if (it.kind == BK_STMT) {
			c_stmt(p, it.stmt, ind, out);
		} else if (it.kind == BK_IF) {
			*out += pad + "if (" + c_opd(it.ifn->cond) + ") {\n";
			c_body(p, it.ifn->then_body, ind + 1, out);
			if (it.ifn->has_else) {
				*out += pad + "} else {\n";
				c_body(p, it.ifn->else_body, ind + 1, out);
			}
			*out += pad + "}\n";
		} else {
			LoopNest *l = it.loop;
			*out += pad + fmt("for (long %s = %ld; %s %s %ld; %s += %ld) {\n", l->index_var.c_str(),
			                  l->lower, l->index_var.c_str(), op_str(l->rel), l->upper,
			                  l->index_var.c_str(), l->step);
			c_body(p, l->body, ind + 1, out);
			*out += pad + "}\n";
		}
	}
}

static long rowbytes(IRProgram *p, const std::string &name)
{
	ArrayInfo &ai = p->arrays[name];
	long n = ai.elem_bytes;
	size_t i;
	for (i = 1; i < ai.dims.size(); i++)
		n *= ai.dims[i];
	return n;
}

static std::string dpu_file(const NestPlan *np)
{
	std::string n = np->name;
	size_t i;
	for (i = 0; i < n.size(); i++)
		if (n[i] == '#')
			n[i] = '_';
	return n + "_dpu.pim";
}

static void offload_nest(IRProgram *p, NestPlan *np, int idx, std::string *out)
{
	PartitionPlan &pp = np->part;
	LoopNest *L = np->root;
	size_t r;
	long T = pp.trips;

	*out += fmt("    /* ---- nest %s: OFFLOAD ------------------------------------------------\n",
	            np->name.c_str());
	*out += "     * " + np->dec.reason + "\n";
	*out += fmt("     * partition: %s distribution of loop %s over %d DPUs (%ld iterations)\n",
	            dist_text(pp.dist), L->index_var.c_str(), pp.ndpus, T);
	for (r = 0; r < pp.notes.size(); r++)
		*out += "     *   " + pp.notes[r] + "\n";
	*out += "     */\n    {\n";
	*out += fmt("        const int P = %d;\n        const long T = %ldL;\n", pp.ndpus, T);
	*out += fmt("        dpu_alloc(P);\n        dpu_load(\"%s\");\n", dpu_file(np).c_str());
	for (r = 0; r < pp.arrays.size(); r++)
		if (pp.arrays[r].mode == AM_PARTITIONED && !pp.arrays[r].local_only)
			*out += fmt("        long %s_row0[%d], %s_n[%d];   /* rows of %s each DPU transfers */\n",
			            pp.arrays[r].name.c_str(), NUM_DPUS, pp.arrays[r].name.c_str(), NUM_DPUS,
			            pp.arrays[r].name.c_str());
	*out += fmt("        for (int d = 0; d < P; d++) {\n");

	if (pp.dist == DIST_BLOCK) {
		*out += fmt("            long chunk = (T + P - 1) / P;\n"
		            "            long first = (long)d * chunk;\n"
		            "            long count = T - first < chunk ? T - first : chunk;\n"
		            "            long stride = 1;\n");
	} else if (pp.dist == DIST_CYCLIC) {
		*out += fmt("            long first = d;                      /* cyclic: iterations d, d+P, d+2P, ... */\n"
		            "            long stride = P;\n"
		            "            long count = (T - d + P - 1) / P;\n");
	} else {
		*out += fmt("            /* block-cyclic with chunks of %ld: the DPU program is launched once per\n"
		            "             * chunk, so the host loops over this DPU's chunks */\n"
		            "            long first = (long)d * %ldL;\n"
		            "            long stride = 1;\n"
		            "            long count = T - first < %ldL ? T - first : %ldL;\n",
		            pp.block, pp.block, pp.block, pp.block);
	}
	*out += "            if (count <= 0) continue;\n";
	*out += "            dpu_set_args(d, first, count, stride);\n";

	/* scalars the DPU needs */
	std::vector<IRStmt*> all;
	ir_collect_stmts(L->body, &all);
	std::set<std::string> scal;
	size_t i;
	for (i = 0; i < all.size(); i++) {
		std::vector<std::string> u;
		ir_stmt_uses(all[i], &u);
		for (size_t q = 0; q < u.size(); q++)
			if (p->scalars.count(u[q]))
				scal.insert(u[q]);
	}
	bool first_scalar = true;
	for (std::set<std::string>::iterator si = scal.begin(); si != scal.end(); si++) {
		bool priv = false;
		for (i = 0; i < np->na.reductions.size(); i++)
			if (!np->na.reductions[i].is_array && np->na.reductions[i].var == *si &&
			    (np->na.reductions[i].kind == RED_SUM || np->na.reductions[i].kind == RED_PROD))
				priv = true;
		if (priv)
			continue;
		if (first_scalar)
			*out += "            /* scalar launch arguments */\n";
		first_scalar = false;
		*out += fmt("            dpu_set_scalar(d, \"%s\", (double)%s);\n", si->c_str(), si->c_str());
	}

	for (r = 0; r < pp.arrays.size(); r++) {
		ArrayRole &role = pp.arrays[r];
		if (role.local_only) {
			*out += fmt("            /* %s never leaves PIM: no transfer */\n", role.name.c_str());
			continue;
		}
		long rb = rowbytes(p, role.name);
		long rows = p->arrays[role.name].dims[0];
		if (role.mode == AM_PARTITIONED) {
			*out += fmt("            %s_row0[d] = (%ldL + %ldL * first) * %ldL + %ldL;\n"
			            "            %s_n[d] = (count - 1) * stride * %ldL * %ldL + 1 + %ldL;\n"
			            "            if (%s_row0[d] < 0) { %s_n[d] += %s_row0[d]; %s_row0[d] = 0; }\n"
			            "            if (%s_row0[d] + %s_n[d] > %ldL) %s_n[d] = %ldL - %s_row0[d];\n",
			            role.name.c_str(), L->lower, L->step, role.row_coeff, role.row_off - role.halo_lo,
			            role.name.c_str(), L->step, role.row_coeff, role.halo_lo + role.halo_hi,
			            role.name.c_str(), role.name.c_str(), role.name.c_str(), role.name.c_str(),
			            role.name.c_str(), role.name.c_str(), rows, role.name.c_str(), rows,
			            role.name.c_str());
			if (role.read)
				*out += fmt("            dma_in(d, \"%s\", %s_row0[d], %s_n[d], %ldL);\n", role.name.c_str(),
				            role.name.c_str(), role.name.c_str(), rb);
		} else if (role.mode == AM_BROADCAST || (role.mode == AM_SCATTER && role.read)) {
			*out += fmt("            dma_in(d, \"%s\", 0, %ldL, %ldL);   /* broadcast: whole array */\n",
			            role.name.c_str(), rows, rb);
		}
	}
	*out += "        }\n";
	*out += "        dpu_launch();\n        dpu_sync();\n";

	/* results */
	*out += fmt("        for (int d = 0; d < P; d++) {\n");
	for (r = 0; r < pp.arrays.size(); r++) {
		ArrayRole &role = pp.arrays[r];
		long rb = rowbytes(p, role.name);
		long rows = p->arrays[role.name].dims[0];
		if (role.local_only)
			continue;
		if (role.mode == AM_PARTITIONED && role.written)
			*out += fmt("            dma_out(d, \"%s\", %s_row0[d], %s_n[d], %ldL);\n", role.name.c_str(),
			            role.name.c_str(), role.name.c_str(), rb);
		else if (role.mode == AM_PRIVATE)
			*out += fmt("            dma_out(d, \"%s\", 0, %ldL, %ldL);   /* private copy, merged below */\n",
			            role.name.c_str(), rows, rb);
	}
	*out += "        }\n";
	for (r = 0; r < np->na.reductions.size(); r++) {
		Reduction &red = np->na.reductions[r];
		const char *op = red.kind == RED_PROD ? "*" : "+";
		if (red.is_array)
			*out += fmt("        host_merge_array(\"%s\", P);   /* element-wise %s over the %d private copies */\n",
			            red.var.c_str(), red.kind == RED_PROD ? "product" : "sum", pp.ndpus);
		else if (red.kind == RED_MAX || red.kind == RED_MIN)
			*out += fmt("        for (int d = 0; d < P; d++) { double v = dpu_result(d, \"%s\"); "
			            "if (v %s %s) %s = v; }\n", red.var.c_str(), red.kind == RED_MAX ? ">" : "<",
			            red.var.c_str(), red.var.c_str());
		else
			*out += fmt("        for (int d = 0; d < P; d++) %s = %s %s dpu_result(d, \"%s\");\n",
			            red.var.c_str(), red.var.c_str(), op, red.var.c_str());
	}
	*out += "    }\n";
	(void)idx;
}

std::string host_codegen(ProgramPlan *pp, KernelPlan *kp, std::vector<DProgram*> &dprogs)
{
	IRProgram *p = pp->prog;
	Kernel *k = kp->kernel;
	std::string out;
	size_t i, n;
	(void)dprogs;

	out += "/* ============================================================================\n";
	out += " * " + k->name + "_host.c  --  host orchestration generated by NEMO-C\n";
	out += " *\n";
	for (n = 0; n < kp->nests.size(); n++)
		out += " *   " + decision_line(&kp->nests[n]->dec) + "\n";
	out += " *\n * Build the standalone trace:  gcc -DNEMOC_STANDALONE " + k->name + "_host.c -o " + k->name +
	       "_trace\n";
	out += " * ==========================================================================*/\n";
	out += "#include <stdio.h>\n#include <stdlib.h>\n#include <string.h>\n\n";

	/* the trace runtime */
	out += "/* ---- trace runtime: stands in for the vendor DPU API ---------------------- */\n";
	out += "static void dpu_alloc(int n)                  { printf(\"[host] dpu_alloc(%d)\\n\", n); }\n";
	out += "static void dpu_load(const char *prog)        { printf(\"[host] dpu_load(%s)\\n\", prog); }\n";
	out += "static void dpu_set_args(int d, long first, long count, long stride)\n"
	       "    { printf(\"[host] dpu %d: first=%ld count=%ld stride=%ld\\n\", d, first, count, stride); }\n";
	out += "static void dpu_set_scalar(int d, const char *n, double v)\n"
	       "    { if (d == 0) printf(\"[host] launch argument %s = %g\\n\", n, v); }\n";
	out += "static void dma_in(int d, const char *a, long row0, long nrows, long rowbytes)\n"
	       "    { if (d < 2 || d == 63) printf(\"[host] dma_in  dpu %d  %s rows [%ld,%ld) = %ld bytes\\n\","
	       " d, a, row0, row0 + nrows, nrows * rowbytes); }\n";
	out += "static void dma_out(int d, const char *a, long row0, long nrows, long rowbytes)\n"
	       "    { if (d < 2 || d == 63) printf(\"[host] dma_out dpu %d  %s rows [%ld,%ld) = %ld bytes\\n\","
	       " d, a, row0, row0 + nrows, nrows * rowbytes); }\n";
	out += "static void dpu_launch(void)                  { printf(\"[host] dpu_launch()\\n\"); }\n";
	out += "static void dpu_sync(void)                    { printf(\"[host] dpu_sync()\\n\"); }\n";
	out += "static double dpu_result(int d, const char *n) { (void)d; (void)n; return 0; }\n";
	out += "static void host_merge_array(const char *a, int p) { printf(\"[host] merge %d private copies of %s\\n\", p, a); }\n";
	out += "static void cache_flush(const char *a, size_t bytes)\n"
	       "    { printf(\"[host] cache_flush(%s)  /* %zu bytes written back */\\n\", a, bytes); }\n";
	out += "static void cache_invalidate(const char *a, size_t bytes)\n"
	       "    { printf(\"[host] cache_invalidate(%s)  /* %zu bytes dropped */\\n\", a, bytes); }\n";
	out += "\n";

	/* data */
	out += "#ifdef NEMOC_STANDALONE\n#define NEMOC_EXTERN\n#else\n#define NEMOC_EXTERN extern\n#endif\n\n";
	std::map<std::string, ArrayInfo>::iterator ai;
	for (ai = p->arrays.begin(); ai != p->arrays.end(); ai++) {
		out += std::string("NEMOC_EXTERN ") + (ai->second.elem_type == TY_FLOAT ? "float " : "int ") + ai->first;
		for (i = 0; i < ai->second.dims.size(); i++)
			out += fmt("[%d]", ai->second.dims[i]);
		out += ";\n";
	}
	std::map<std::string, TypeTag>::iterator si;
	for (si = p->scalars.begin(); si != p->scalars.end(); si++)
		out += std::string("NEMOC_EXTERN ") + ctype(si->second) + " " + si->first + ";\n";
	out += "\n";

	/* the orchestration function */
	out += "void nemoc_call_" + k->name + "(void)\n{\n";
	std::map<std::string, TypeTag> temps;
	collect_temps(k->body, &temps);
	std::map<std::string, TypeTag>::iterator ti;
	for (ti = temps.begin(); ti != temps.end(); ti++)
		out += std::string("    ") + ctype(ti->second) + " " + cname(ti->first) + ";\n";

	size_t ni = 0;
	for (i = 0; i < k->body.size(); i++) {
		BodyItem &it = k->body[i];
		if (it.kind == BK_LOOP) {
			NestPlan *np = ni < kp->nests.size() ? kp->nests[ni] : 0;
			ni++;
			if (np && np->offload) {
				offload_nest(p, np, (int)ni, &out);
			} else {
				out += "    /* ---- nest " + (np ? np->name : "?") + ": stays on the HOST ----\n";
				if (np)
					out += "     * " + np->dec.reason + "\n";
				out += "     */\n";
				std::vector<BodyItem> one;
				one.push_back(it);
				c_body(p, one, 1, &out);
			}
		} else {
			std::vector<BodyItem> one;
			one.push_back(it);
			c_body(p, one, 1, &out);
		}
	}
	out += "}\n\n";

	out += "#ifdef NEMOC_STANDALONE\nint main(void)\n{\n    nemoc_call_" + k->name +
	       "();\n    return 0;\n}\n#endif\n";
	return out;
}
