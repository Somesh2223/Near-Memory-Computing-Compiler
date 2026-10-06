/* ============================================================================
 * codegen_dpu.cpp  --  DPU kernel code generation for the PIM ISA
 *
 * Compiler-theory concepts: instruction selection by tree/DAG tiling of
 * three-address code (Aho/Lam/Sethi/Ullman 2nd ed., chapters 8.9 and 6) and the
 * classic strip-mining ("tiling") transformation that makes a streaming loop fit
 * a small scratchpad.
 *
 * What is generated for one offloaded nest.  Every DPU runs the same program;
 * the launch arguments `first`, `count`, `stride` select the iterations of the
 * partitioned loop it owns (iteration counter t = first + k*stride, loop index
 * i = lower + step*t).  The program is
 *
 *     read launch arguments, initialise private accumulators
 *     DMA_IN every broadcast array once
 *     HALO_EXCHANGE the stencil arrays (neighbours' boundary rows)
 *     LOOP over tiles of T iterations:                 <- scratchpad tiling
 *         DMA_IN the rows this tile reads (plus its halo)
 *         LOOP over the iterations of the tile:  the nest body, one IR
 *                                                statement -> a few instructions
 *         DMA_OUT the rows this tile wrote
 *     return private scalar accumulators, DMA_OUT private arrays
 *
 * SCRATCHPAD TILING.  The tile holds T iterations' worth of every partitioned
 * array, so the scratchpad constraint is
 *         T * (sum of the arrays' bytes per iteration) <= WRAM_BYTES - (what is
 *         permanently resident: broadcast + private arrays, halo rows, spill area)
 * which for a 1-D elementwise kernel is the course formula
 *         T * elem_bytes * num_arrays <= WRAM_BYTES.
 * The arithmetic is written into the generated listing as a comment.
 *
 * The program uses virtual registers; regalloc.cpp colours them with K = 24.
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

#define SPILL_RESERVE_BYTES 512

struct Gen {
	IRProgram                  *p;
	NestPlan                   *np;
	DProgram                   *prog;
	std::map<std::string, int>  reg;
	std::string                 err;
};

static int newreg(Gen *g, const std::string &name, bool fp)
{
	g->prog->vreg_name.push_back(name);
	g->prog->vreg_float.push_back(fp);
	return g->prog->nvregs++;
}

static DInstr &emit(Gen *g, int op, int d, int a, int b, long imm = 0, const std::string &sym = "",
                    bool fp = false, const std::string &note = "")
{
	DInstr I;
	I.op = op;
	I.fp = fp;
	I.d = d;
	I.a = a;
	I.b = b;
	I.imm = imm;
	I.fimm = 0;
	I.sym = sym;
	I.note = note;
	g->prog->code.push_back(I);
	return g->prog->code.back();
}

static int movi(Gen *g, long v, const std::string &name = "c")
{
	int r = newreg(g, name, false);
	emit(g, D_MOVI, r, -1, -1, v);
	return r;
}

static int opd_reg(Gen *g, const Operand &o)
{
	if (o.kind == OPD_INT)
		return movi(g, o.ival);
	if (o.kind == OPD_FLOAT) {
		int r = newreg(g, "f", true);
		DInstr &I = emit(g, D_MOVI, r, -1, -1, 0, "", true);
		I.fimm = (double)(float)o.dval;
		return r;
	}
	std::map<std::string, int>::iterator it = g->reg.find(o.name);
	if (it == g->reg.end()) {
		g->err = "DPU codegen: name " + o.name + " has no register";
		return 0;
	}
	return it->second;
}

/* a register for the destination name: reuse when it already exists (a loop-
 * carried accumulator), otherwise a fresh one */
static int dst_reg(Gen *g, const std::string &name, bool fp)
{
	std::map<std::string, int>::iterator it = g->reg.find(name);
	if (it != g->reg.end())
		return it->second;
	int r = newreg(g, name, fp);
	g->reg[name] = r;
	return r;
}

static int dop_of(int op)
{
	switch (op) {
	case OP_ADD: return D_ADD;
	case OP_SUB: return D_SUB;
	case OP_MUL: return D_MUL;
	case OP_DIV: return D_DIV;
	case OP_MOD: return D_MOD;
	case OP_LT:  return D_SLT;
	case OP_LE:  return D_SLE;
	case OP_GT:  return D_SGT;
	case OP_GE:  return D_SGE;
	case OP_EQ:  return D_SEQ;
	default:     return D_SNE;
	}
}

/* row-major linear index of a subscript list */
static int linear_index(Gen *g, IRStmt *s)
{
	ArrayInfo &ai = g->p->arrays[s->array];
	size_t k;
	int lin = opd_reg(g, s->idx[0].value);
	for (k = 1; k < s->idx.size(); k++) {
		int dk = movi(g, ai.dims[k], "dim");
		int t = newreg(g, "lin", false);
		emit(g, D_MUL, t, lin, dk);
		int nxt = newreg(g, "lin", false);
		emit(g, D_ADD, nxt, t, opd_reg(g, s->idx[k].value));
		lin = nxt;
	}
	return lin;
}

static bool gen_body(Gen *g, std::vector<BodyItem> &body);

static bool gen_stmt(Gen *g, IRStmt *s)
{
	switch (s->kind) {
	case IR_ARRAY_LOAD: {
		int lin = linear_index(g, s);
		int rd = dst_reg(g, s->dst, s->dst_type == TY_FLOAT);
		emit(g, D_LD, rd, lin, -1, 0, s->array, s->dst_type == TY_FLOAT);
		return true;
	}
	case IR_ARRAY_STORE: {
		int lin = linear_index(g, s);
		int rv = opd_reg(g, s->src);
		emit(g, D_ST, -1, lin, rv, 0, s->array);
		return true;
	}
	case IR_BINOP: {
		int a = opd_reg(g, s->lhs);
		int b = opd_reg(g, s->rhs);
		bool cmp = s->op >= OP_LT && s->op <= OP_NE;
		bool fp = cmp ? (s->lhs.type == TY_FLOAT || s->rhs.type == TY_FLOAT)
		              : (s->dst_type == TY_FLOAT);
		int rd = dst_reg(g, s->dst, s->dst_type == TY_FLOAT);
		emit(g, dop_of(s->op), rd, a, b, 0, "", fp);
		return true;
	}
	case IR_COPY: {
		bool fp = (s->dst_type == TY_FLOAT);
		int rd = dst_reg(g, s->dst, fp);
		if (s->src.kind == OPD_INT) {
			emit(g, D_MOVI, rd, -1, -1, s->src.ival, "", fp);
		} else if (s->src.kind == OPD_FLOAT) {
			DInstr &I = emit(g, D_MOVI, rd, -1, -1, 0, "", true);
			I.fimm = (double)(float)s->src.dval;
			I.fp = true;
		} else {
			emit(g, D_MOV, rd, opd_reg(g, s->src), -1, 0, "", fp);
		}
		return true;
	}
	default:
		g->err = "DPU codegen: unsupported statement inside a kernel nest: " + ir_stmt_text(s);
		return false;
	}
}

static bool gen_loop(Gen *g, LoopNest *l)
{
	int ri = newreg(g, l->index_var, false);
	emit(g, D_MOVI, ri, -1, -1, l->lower, "", false, "for " + l->index_var);
	int rb = movi(g, l->rel == OP_LE ? l->upper + 1 : l->upper, "bound");
	g->reg[l->index_var] = ri;
	emit(g, D_LOOP, -1, ri, rb);
	if (!gen_body(g, l->body))
		return false;
	int rs = movi(g, l->step, "step");
	emit(g, D_ADD, ri, ri, rs);
	emit(g, D_ENDLOOP, -1, -1, -1);
	return true;
}

static bool gen_body(Gen *g, std::vector<BodyItem> &body)
{
	size_t i;
	for (i = 0; i < body.size(); i++) {
		BodyItem &it = body[i];
		if (it.kind == BK_STMT) {
			if (!gen_stmt(g, it.stmt))
				return false;
		} else if (it.kind == BK_IF) {
			int rc = opd_reg(g, it.ifn->cond);
			emit(g, D_IFNZ, -1, rc, -1);
			if (!gen_body(g, it.ifn->then_body))
				return false;
			if (it.ifn->has_else) {
				emit(g, D_ELSE, -1, -1, -1);
				if (!gen_body(g, it.ifn->else_body))
					return false;
			}
			emit(g, D_ENDIF, -1, -1, -1);
		} else if (!gen_loop(g, it.loop)) {
			return false;
		}
	}
	return true;
}

/* ---- scratchpad tiling: T x bytes-per-iteration <= free scratchpad ----------------- */
static long row_bytes_of(IRProgram *p, const std::string &name)
{
	ArrayInfo &ai = p->arrays[name];
	long n = ai.elem_bytes;
	size_t i;
	for (i = 1; i < ai.dims.size(); i++)
		n *= ai.dims[i];
	return n;
}

long dpu_tile_rows(IRProgram *p, NestPlan *np, std::string *arith)
{
	PartitionPlan &pp = np->part;
	long per_iter = 0, resident = 0, halo = 0;
	int narr = 0;
	size_t i;
	for (i = 0; i < pp.arrays.size(); i++) {
		ArrayRole &r = pp.arrays[i];
		ArrayInfo &ai = p->arrays[r.name];
		long total = ai.elem_bytes;
		for (size_t d = 0; d < ai.dims.size(); d++)
			total *= ai.dims[d];
		if (r.mode == AM_PARTITIONED) {
			per_iter += row_bytes_of(p, r.name);
			halo += (r.halo_lo + r.halo_hi) * row_bytes_of(p, r.name);
			narr++;
		} else if (r.mode == AM_BROADCAST || r.mode == AM_PRIVATE) {
			resident += total;
		}
	}
	long free_b = WRAM_BYTES - SPILL_RESERVE_BYTES - resident - halo;
	long T = per_iter > 0 ? free_b / per_iter : 1;
	if (T < 1)
		T = 1;
	if (pp.dist != DIST_BLOCK)
		T = 1;
	if (arith) {
		char b[360];
		snprintf(b, sizeof b,
		         "T = floor((WRAM_BYTES %d - spill area %d - resident %ld - halo %ld) / "
		         "(%ld bytes per iteration over %d arrays)) = %ld iterations per tile%s",
		         WRAM_BYTES, SPILL_RESERVE_BYTES, resident, halo, per_iter, narr, T,
		         pp.dist != DIST_BLOCK ? "   [strided distribution: T forced to 1]" : "");
		*arith = b;
	}
	return T;
}

static double identity_f(int kind)
{
	return kind == RED_PROD ? 1.0 : 0.0;
}

bool dpu_codegen(IRProgram *p, NestPlan *np, DProgram *out, std::string *err)
{
	Gen g;
	PartitionPlan &pp = np->part;
	LoopNest *L = np->root;
	size_t i, r;

	out->name = np->name;
	out->code.clear();
	out->vreg_name.clear();
	out->vreg_float.clear();
	out->nvregs = 0;
	out->allocated = false;
	out->nspill = 0;
	out->header.clear();
	out->strided = (pp.dist == DIST_CYCLIC);

	g.p = p;
	g.np = np;
	g.prog = out;

	std::string arith;
	long T = dpu_tile_rows(p, np, &arith);
	out->tile_rows = T;
	out->header.push_back("kernel " + np->name + " : " + std::string(dist_text(pp.dist)) +
	                      " distribution over " + std::to_string(pp.ndpus) + " DPUs");
	out->header.push_back("scratchpad tiling: " + arith);
	out->header.push_back("(constraint: T x bytes-per-iteration <= free WRAM; for a 1-D elementwise "
	                      "kernel this is T x elem_bytes x num_arrays <= WRAM_BYTES)");

	/* launch arguments */
	int rFirst  = newreg(&g, "first", false);
	int rCount  = newreg(&g, "count", false);
	int rStride = newreg(&g, "stride", false);
	emit(&g, D_ARG, rFirst, -1, -1, 0, "first");
	emit(&g, D_ARG, rCount, -1, -1, 0, "count");
	emit(&g, D_ARG, rStride, -1, -1, 0, "stride");

	/* scalars the nest uses */
	std::set<std::string> used;
	std::vector<IRStmt*> all;
	ir_collect_stmts(L->body, &all);
	for (i = 0; i < all.size(); i++) {
		std::vector<std::string> u;
		ir_stmt_uses(all[i], &u);
		for (size_t q = 0; q < u.size(); q++)
			if (p->scalars.count(u[q]))
				used.insert(u[q]);
		if (p->scalars.count(ir_stmt_def(all[i])))
			used.insert(ir_stmt_def(all[i]));
	}
	std::set<std::string> priv_scalars;
	for (i = 0; i < np->na.reductions.size(); i++)
		if (!np->na.reductions[i].is_array)
			priv_scalars.insert(np->na.reductions[i].var);
	std::set<std::string>::iterator si;
	for (si = used.begin(); si != used.end(); si++) {
		bool fp = p->scalars[*si] == TY_FLOAT;
		int rs = newreg(&g, *si, fp);
		g.reg[*si] = rs;
		bool identity = false;
		int kind = -1;
		for (i = 0; i < np->na.reductions.size(); i++)
			if (!np->na.reductions[i].is_array && np->na.reductions[i].var == *si) {
				kind = np->na.reductions[i].kind;
				identity = (kind == RED_SUM || kind == RED_PROD);
			}
		if (identity) {
			DInstr &I = emit(&g, D_MOVI, rs, -1, -1, kind == RED_PROD ? 1 : 0, "", fp,
			                 "private accumulator, identity element");
			I.fimm = identity_f(kind);
		} else {
			emit(&g, D_ARG, rs, -1, -1, 0, *si);
		}
	}

	/* broadcast and private arrays */
	for (r = 0; r < pp.arrays.size(); r++) {
		ArrayRole &role = pp.arrays[r];
		if (role.local_only && role.mode != AM_PARTITIONED)
			continue;
		if (role.mode == AM_BROADCAST || (role.mode == AM_SCATTER && role.read)) {
			int r0 = movi(&g, 0, "row0");
			int rn = movi(&g, p->arrays[role.name].dims[0], "rows");
			emit(&g, D_DMA_IN, -1, r0, rn, row_bytes_of(p, role.name), role.name, false,
			     "broadcast: the whole array");
		} else if (role.mode == AM_PRIVATE) {
			DInstr &I = emit(&g, D_FILL, -1, -1, -1,
			                 role.red_kind == RED_PROD ? 1 : 0, role.name, p->arrays[role.name].elem_type == TY_FLOAT,
			                 "private copy starts at the identity");
			I.fimm = identity_f(role.red_kind);
		}
		if (role.mode == AM_PARTITIONED && (role.halo_lo || role.halo_hi)) {
			int rh = movi(&g, role.halo_lo + role.halo_hi, "halo");
			emit(&g, D_HALO, -1, rh, -1, role.halo_lo + role.halo_hi, role.name, false,
			     "boundary rows owned by the neighbouring DPUs");
		}
	}

	/* the tile loop */
	int rK0 = movi(&g, 0, "k0");
	int rT  = movi(&g, T, "T");
	emit(&g, D_LOOP, -1, rK0, rCount, 0, "", false, "for each tile of T iterations");
	int rSum = newreg(&g, "k0+T", false);
	emit(&g, D_ADD, rSum, rK0, rT);
	int rKe = newreg(&g, "kend", false);
	emit(&g, D_MIN, rKe, rSum, rCount);

	int rt0 = newreg(&g, "t0", false);
	int rm0 = newreg(&g, "k0*stride", false);
	emit(&g, D_MUL, rm0, rK0, rStride);
	emit(&g, D_ADD, rt0, rm0, rFirst);
	int rStep = movi(&g, L->step, "step");
	int rLow  = movi(&g, L->lower, "lower");
	int ri0   = newreg(&g, "i0", false);
	int rtmp  = newreg(&g, "t0*step", false);
	emit(&g, D_MUL, rtmp, rt0, rStep);
	emit(&g, D_ADD, ri0, rtmp, rLow);

	int rLen = newreg(&g, "tile_len", false);
	emit(&g, D_SUB, rLen, rKe, rK0);
	int rOne = movi(&g, 1, "one");
	int rLm1 = newreg(&g, "len-1", false);
	emit(&g, D_SUB, rLm1, rLen, rOne);
	int rSpan0 = newreg(&g, "span", false);
	emit(&g, D_MUL, rSpan0, rLm1, rStep);

	/* per-array row windows */
	std::vector<std::pair<std::string, std::pair<int,int> > > windows;   /* name -> (row0 reg, nrows reg) */
	std::vector<bool> win_in, win_out;
	for (r = 0; r < pp.arrays.size(); r++) {
		ArrayRole &role = pp.arrays[r];
		if (role.mode != AM_PARTITIONED && role.mode != AM_SCATTER)
			continue;
		if (role.mode == AM_SCATTER)
			continue;                                       /* handled as whole-array transfers */
		if (role.local_only)
			continue;                                       /* never leaves PIM: no row window */
		int rOff = movi(&g, role.row_off - role.halo_lo, "rowoff");
		int rr0 = newreg(&g, role.name + ".row0", false);
		emit(&g, D_ADD, rr0, ri0, rOff);
		int rAdd = movi(&g, 1 + role.halo_lo + role.halo_hi, "span+");
		int rn = newreg(&g, role.name + ".rows", false);
		emit(&g, D_ADD, rn, rSpan0, rAdd);
		windows.push_back(std::make_pair(role.name, std::make_pair(rr0, rn)));
		/* an array that never leaves PIM is produced and consumed inside the
		 * scratchpad: there is nothing in the bank to fetch */
		if (role.read && !role.local_only)
			emit(&g, D_DMA_IN, -1, rr0, rn, row_bytes_of(p, role.name), role.name);
	}

	/* strength-reduced accumulators of the partitioned loop restart from i0 */
	std::set<std::string> sr_done;
	for (i = 0; i < all.size(); i++) {
		std::string d = ir_stmt_def(all[i]);
		if (d.empty() || sr_done.count(d) || !p->ivs.count(d) || p->ivs[d].basic != L->index_var)
			continue;
		sr_done.insert(d);
		IVInfo &iv = p->ivs[d];
		int rsr = dst_reg(&g, d, false);
		int rc = movi(&g, iv.coeff, "coeff");
		int rm = newreg(&g, "a*i0", false);
		emit(&g, D_MUL, rm, rc, ri0);
		int ro = movi(&g, iv.offset, "offset");
		emit(&g, D_ADD, rsr, rm, ro, 0, "", false, "strength-reduced accumulator restarts from i0");
	}

	/* the iteration loop */
	int rK = newreg(&g, "k", false);
	emit(&g, D_MOV, rK, rK0, -1);
	emit(&g, D_LOOP, -1, rK, rKe, 0, "", false, "iterations of the tile");
	int rtK = newreg(&g, "t", false);
	int rmk = newreg(&g, "k*stride", false);
	emit(&g, D_MUL, rmk, rK, rStride);
	emit(&g, D_ADD, rtK, rmk, rFirst);
	int rmi = newreg(&g, "t*step", false);
	emit(&g, D_MUL, rmi, rtK, rStep);
	int rI = newreg(&g, L->index_var, false);
	emit(&g, D_ADD, rI, rmi, rLow);
	g.reg[L->index_var] = rI;

	if (!gen_body(&g, L->body)) {
		*err = g.err;
		return false;
	}
	int rOne2 = movi(&g, 1, "one");
	emit(&g, D_ADD, rK, rK, rOne2);
	emit(&g, D_ENDLOOP, -1, -1, -1);

	for (r = 0; r < pp.arrays.size(); r++) {
		ArrayRole &role = pp.arrays[r];
		if (role.mode != AM_PARTITIONED || !role.written || role.local_only)
			continue;
		size_t w;
		for (w = 0; w < windows.size(); w++)
			if (windows[w].first == role.name)
				emit(&g, D_DMA_OUT, -1, windows[w].second.first, windows[w].second.second,
				     row_bytes_of(p, role.name), role.name);
	}
	emit(&g, D_ADD, rK0, rK0, rT);
	emit(&g, D_ENDLOOP, -1, -1, -1);

	/* results */
	for (si = priv_scalars.begin(); si != priv_scalars.end(); si++)
		if (g.reg.count(*si))
			emit(&g, D_RET, -1, g.reg[*si], -1, 0, *si);
	for (r = 0; r < pp.arrays.size(); r++) {
		ArrayRole &role = pp.arrays[r];
		if (role.mode == AM_PRIVATE) {
			int r0 = movi(&g, 0, "row0");
			int rn = movi(&g, p->arrays[role.name].dims[0], "rows");
			emit(&g, D_DMA_OUT, -1, r0, rn, row_bytes_of(p, role.name), role.name, false,
			     "private array returned for merging");
		} else if (role.mode == AM_SCATTER && role.written) {
			int r0 = movi(&g, 0, "row0");
			int rn = movi(&g, p->arrays[role.name].dims[0], "rows");
			emit(&g, D_DMA_OUT, -1, r0, rn, row_bytes_of(p, role.name), role.name, false,
			     "scattered writes");
		}
	}
	if (!g.err.empty()) {
		*err = g.err;
		return false;
	}
	return true;
}

/* ---- listing ----------------------------------------------------------------------- */
static std::string regname(DProgram *prog, int r)
{
	char b[24];
	snprintf(b, sizeof b, "%s%d", prog->allocated ? "r" : "v", r);
	return b;
}

void dpu_print(DProgram *prog, FILE *out)
{
	static const char *names[] = {"MOVI", "MOV", "MIN", "ADD", "SUB", "MUL", "DIV", "MOD", "SLT", "SLE",
	                              "SGT", "SGE", "SEQ", "SNE", "LD", "ST", "DMA_IN", "DMA_OUT",
	                              "HALO_EXCHANGE", "FILL", "LOOP", "ENDLOOP", "IFNZ", "ELSE", "ENDIF",
	                              "ARG", "RET", "LD", "ST"};
	size_t i;
	int indent = 0;
	for (i = 0; i < prog->header.size(); i++)
		fprintf(out, "; %s\n", prog->header[i].c_str());
	fprintf(out, "; %s registers (%d available), %d spill slot(s)\n",
	        prog->allocated ? "physical" : "virtual", prog->nregs, prog->nspill);
	for (i = 0; i < prog->code.size(); i++) {
		DInstr &I = prog->code[i];
		if (I.op == D_ENDLOOP || I.op == D_ENDIF || I.op == D_ELSE)
			indent--;
		char line[256];
		std::string mn = names[I.op];
		if (I.fp && (I.op == D_ADD || I.op == D_SUB || I.op == D_MUL || I.op == D_DIV || I.op == D_MOVI ||
		             (I.op >= D_SLT && I.op <= D_SNE)))
			mn += ".F";
		std::string args;
		switch (I.op) {
		case D_MOVI:
			if (I.fp) { snprintf(line, sizeof line, "%s, %g", regname(prog, I.d).c_str(), I.fimm); }
			else      { snprintf(line, sizeof line, "%s, %ld", regname(prog, I.d).c_str(), I.imm); }
			args = line;
			break;
		case D_MOV:
			args = regname(prog, I.d) + ", " + regname(prog, I.a);
			break;
		case D_LD:
			args = regname(prog, I.d) + ", " + I.sym + "[" + regname(prog, I.a) + "]";
			break;
		case D_ST:
			args = I.sym + "[" + regname(prog, I.a) + "], " + regname(prog, I.b);
			break;
		case D_DMA_IN: case D_DMA_OUT:
			args = I.sym + ", " + regname(prog, I.a) + ", " + regname(prog, I.b);
			break;
		case D_HALO:
			args = I.sym + ", " + regname(prog, I.a);
			break;
		case D_FILL: {
			snprintf(line, sizeof line, "%s, %g", I.sym.c_str(), I.fp ? I.fimm : (double)I.imm);
			args = line;
			break;
		}
		case D_LOOP:
			args = regname(prog, I.a) + ", " + regname(prog, I.b);
			break;
		case D_IFNZ:
			args = regname(prog, I.a);
			break;
		case D_ARG:
			args = regname(prog, I.d) + ", " + I.sym;
			break;
		case D_RET:
			args = I.sym + ", " + regname(prog, I.a);
			break;
		case D_SPILL_LD:
			snprintf(line, sizeof line, "%s, [spill+%ld]", regname(prog, I.d).c_str(), I.imm);
			args = line;
			break;
		case D_SPILL_ST:
			snprintf(line, sizeof line, "[spill+%ld], %s", I.imm, regname(prog, I.a).c_str());
			args = line;
			break;
		case D_ELSE: case D_ENDIF: case D_ENDLOOP:
			break;
		default:
			args = regname(prog, I.d) + ", " + regname(prog, I.a) + ", " + regname(prog, I.b);
		}
		fprintf(out, "%*s%-14s %s", indent * 2 + 2, "", mn.c_str(), args.c_str());
		if (!I.note.empty())
			fprintf(out, "%*s; %s", (int)std::max<long>(1, 36 - (long)args.size()), "", I.note.c_str());
		fprintf(out, "\n");
		if (I.op == D_LOOP || I.op == D_IFNZ || I.op == D_ELSE)
			indent++;
	}
}

int dpu_generate_all(ProgramPlan *pp, int K, std::vector<DProgram*> *out, int verbose)
{
	size_t k, n;
	int count = 0;
	for (k = 0; k < pp->kernels.size(); k++)
		for (n = 0; n < pp->kernels[k].nests.size(); n++) {
			NestPlan *np = pp->kernels[k].nests[n];
			if (!np->offload)
				continue;
			DProgram *dp = new DProgram();
			std::string err;
			if (!dpu_codegen(pp->prog, np, dp, &err)) {
				fprintf(stderr, "nemoc: DPU codegen failed for %s: %s\n", np->name.c_str(), err.c_str());
				continue;
			}
			RAResult ra;
			regalloc_run(dp, K, &ra, verbose);
			if (verbose)
				regalloc_print(dp, &ra);
			out->push_back(dp);
			count++;
		}
	return count;
}
