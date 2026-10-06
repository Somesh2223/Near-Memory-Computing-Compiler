/* ============================================================================
 * simulator.cpp  --  functional + cost simulation of the host and the PIM system
 *
 * Compiler-theory context: this is the course's "back end validation": the
 * compiler's output is only worth anything if (1) it computes the same result
 * as the original program and (2) the cost model's predictions show up when
 * the program is run.  The simulator therefore does two jobs at once.
 *
 * FUNCTIONAL.  An interpreter executes the IR directly.  Host code runs against
 * the global arrays.  An offloaded loop nest runs the way the generated code
 * would: the iteration space is cut across the DPUs by the partition plan;
 * each DPU gets a PRIVATE IMAGE of the arrays in which only the rows the
 * compiler said it needs (its rows, the halo, broadcast arrays) are resident --
 * reading anything else is an error, so a wrong halo or partition is caught;
 * writes are tracked per element and copied back; two DPUs writing the same
 * element is reported as a race; reduction accumulators are private and merged.
 * A coherence checker watches every access and reports an access the static
 * coherence analysis failed to protect.  The result is compared bit-for-bit
 * with the pure host execution.
 *
 * COST.  Host: a set-associative LRU cache model charges HOST_CACHE_HIT or
 * HOST_DRAM_ACCESS per array access and counts the bytes of every line fill and
 * write-back.  PIM: launch overhead, DMA (bytes and non-contiguous runs), and
 * the slowest DPU's compute (ALU/multiply/divide cycles plus local accesses,
 * overlapped across tasklets, serialised by bank conflicts).  Energy follows
 * from the same event counts.  Every constant is in config.h.
 * ==========================================================================*/
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <algorithm>
#include <map>
#include <set>
#include <string>
#include <vector>

#include "config.h"
#include "ast.h"
#include "ir.h"
#include "semantic.h"
#include "frontend.h"
#include "opt.h"
#include "analysis.h"
#include "profit.h"
#include "plan.h"
#include "dpu.h"
#include "sim.h"

const char *sim_config_name(int c)
{
	return c == SIM_HOST_ONLY ? "HOST_ONLY" : (c == SIM_NAIVE_OFFLOAD ? "NAIVE_OFFLOAD" :
	       (c == SIM_NEMO_C ? "NEMO_C" : "NEMO_C+asm"));
}

/* ============================ memory ===================================== */
struct ArrayMem {
	std::vector<Value> v;
	std::vector<char>  valid;     /* DPU images: is this element resident?     */
	std::vector<char>  dirty;     /* DPU images: written by the DPU            */
	std::vector<int>   dims;
	TypeTag            type;
	long               base;      /* byte address, for the host cache model    */
};
typedef std::map<std::string, ArrayMem> MemMap;
typedef std::map<std::string, Value>    ScalarMap;

/* ============================ host cache ================================= */
struct CLine {
	long tag;
	bool valid;
	bool dirty;
	long lru;
};

struct Cache {
	int                 nsets;
	std::vector<CLine>  lines;      /* nsets * HOST_CACHE_WAYS                  */
	long                tick;
	double              writeback_bytes;
};

static void cache_init(Cache *c)
{
	c->nsets = HOST_CACHE_BYTES / (CACHE_LINE * HOST_CACHE_WAYS);
	CLine empty;
	empty.tag = -1;
	empty.valid = false;
	empty.dirty = false;
	empty.lru = 0;
	c->lines.assign((size_t)c->nsets * HOST_CACHE_WAYS, empty);
	c->tick = 0;
	c->writeback_bytes = 0;
}

/* returns 1 on a hit, 0 on a miss; *bus receives the bytes this access put on the bus */
static int cache_access(Cache *c, long addr, bool write, double *bus)
{
	long line = addr / CACHE_LINE;
	int set = (int)(line % c->nsets);
	CLine *base = &c->lines[(size_t)set * HOST_CACHE_WAYS];
	int w, victim = 0;
	long best = -1;
	c->tick++;
	*bus = 0;

	for (w = 0; w < HOST_CACHE_WAYS; w++)
		if (base[w].valid && base[w].tag == line) {
			base[w].lru = c->tick;
			if (write)
				base[w].dirty = true;
			return 1;
		}
	for (w = 0; w < HOST_CACHE_WAYS; w++) {
		if (!base[w].valid) {
			victim = w;
			best = -2;
			break;
		}
		if (best < 0 || base[w].lru < best) {
			best = base[w].lru;
			victim = w;
		}
	}
	if (base[victim].valid && base[victim].dirty)
		*bus += CACHE_LINE;                       /* write the victim back */
	*bus += CACHE_LINE;                           /* fill the line         */
	base[victim].tag = line;
	base[victim].valid = true;
	base[victim].dirty = write;
	base[victim].lru = c->tick;
	return 0;
}

/* write back dirty lines of [lo, hi); returns how many */
static long cache_flush_range(Cache *c, long lo, long hi)
{
	long n = 0;
	size_t i;
	for (i = 0; i < c->lines.size(); i++) {
		CLine &l = c->lines[i];
		if (l.valid && l.dirty && l.tag * CACHE_LINE >= lo && l.tag * CACHE_LINE < hi) {
			l.dirty = false;
			n++;
		}
	}
	return n;
}

static long cache_invalidate_range(Cache *c, long lo, long hi)
{
	long n = 0;
	size_t i;
	for (i = 0; i < c->lines.size(); i++) {
		CLine &l = c->lines[i];
		if (l.valid && l.tag * CACHE_LINE >= lo && l.tag * CACHE_LINE < hi) {
			l.valid = false;
			l.dirty = false;
			n++;
		}
	}
	return n;
}

/* ============================ the machine ================================= */
struct Machine {
	IRProgram                 *prog;
	ProgramPlan               *pp;
	int                        config;
	MemMap                     mem;
	ScalarMap                  scalars;
	Cache                      cache;
	double                     cycles, kernel_cycles, bus_bytes, energy_pj;
	int                        offloaded, launches;
	std::set<std::string>      host_dirty, pim_dirty;
	std::vector<std::string>   violations;
	std::vector<std::string>   output;
	std::string                err;
	std::map<std::string, std::vector<int> > writer;   /* race detection per offload */
	std::map<NestPlan*, DProgram*> asm_progs;           /* SIM_NEMO_ASM: generated code */
};

enum { MODE_HOST, MODE_DPU };

struct Counters {
	double alu, mul, div;
	std::map<IRStmt*, long> mem;
};

struct Ctx {
	Machine   *m;
	int        mode;
	MemMap    *mem;
	ScalarMap *scalars;
	ScalarMap  locals;           /* temporaries and loop indices                   */
	Counters   cnt;              /* DPU mode                                       */
	std::string err;
};

static void machine_init(Machine *m, IRProgram *p)
{
	std::map<std::string, ArrayInfo>::iterator ai;
	std::map<std::string, TypeTag>::iterator   si;
	long addr = 4096;

	m->prog = p;
	m->cycles = m->kernel_cycles = m->bus_bytes = m->energy_pj = 0;
	m->offloaded = m->launches = 0;
	cache_init(&m->cache);

	for (ai = p->arrays.begin(); ai != p->arrays.end(); ai++) {
		ArrayMem a;
		long n = 1;
		size_t d;
		a.dims = ai->second.dims;
		for (d = 0; d < a.dims.size(); d++)
			n *= a.dims[d];
		a.type = ai->second.elem_type;
		a.v.assign((size_t)n, a.type == TY_FLOAT ? mk_val_float(0.0f) : mk_val_int(0));
		a.base = addr;
		addr += (n * ELEM_BYTES + 4095) / 4096 * 4096;
		m->mem[ai->first] = a;
	}
	for (si = p->scalars.begin(); si != p->scalars.end(); si++)
		m->scalars[si->first] = si->second == TY_FLOAT ? mk_val_float(0.0f) : mk_val_int(0);
}

static bool truthy(Value v)
{
	return v.type == TY_INT ? v.i != 0 : v.f != 0.0f;
}

/* ============================ the interpreter ============================== */
static bool exec_body(Ctx *c, std::vector<BodyItem> &body);
static bool run_kernel(Machine *m, const std::string &name);
static bool pim_nest(Machine *m, NestPlan *np);

static bool fail(Ctx *c, const std::string &msg)
{
	if (c->err.empty())
		c->err = msg;
	return false;
}

static bool opd_value(Ctx *c, const Operand &o, Value *v)
{
	if (o.kind != OPD_NAME) {
		*v = value_of_operand(o);
		return true;
	}
	ScalarMap::iterator it = c->locals.find(o.name);
	if (it != c->locals.end()) {
		*v = it->second;
		return true;
	}
	it = c->scalars->find(o.name);
	if (it != c->scalars->end()) {
		*v = it->second;
		return true;
	}
	return fail(c, "use of undefined name " + o.name);
}

static bool locate(Ctx *c, IRStmt *s, ArrayMem **arr, long *lin)
{
	MemMap::iterator ai = c->mem->find(s->array);
	size_t k;
	long l = 0;
	if (ai == c->mem->end())
		return fail(c, "array " + s->array + " is not resident");
	ArrayMem &a = ai->second;
	for (k = 0; k < s->idx.size(); k++) {
		Value iv;
		if (!opd_value(c, s->idx[k].value, &iv))
			return false;
		long x = (iv.type == TY_INT) ? iv.i : (long)iv.f;
		if (x < 0 || x >= a.dims[k]) {
			char b[160];
			snprintf(b, sizeof b, "index %ld out of range [0,%d) in dimension %d of %s", x,
			         a.dims[k], (int)k, s->array.c_str());
			return fail(c, b);
		}
		l = l * a.dims[k] + x;
	}
	*arr = &a;
	*lin = l;
	return true;
}

static void host_alu(Ctx *c)
{
	c->m->cycles += HOST_ALU;
	c->m->energy_pj += ALU_PJ;
}

static void host_mem(Ctx *c, ArrayMem *a, long lin, bool write)
{
	double bus;
	int hit = cache_access(&c->m->cache, a->base + lin * ELEM_BYTES, write, &bus);
	c->m->cycles += hit ? HOST_CACHE_HIT : HOST_DRAM_ACCESS;
	if (!hit) {
		c->m->bus_bytes += bus;
		c->m->energy_pj += DRAM_ACCESS_PJ * (bus / CACHE_LINE);
	}
}

static void note_violation(Machine *m, const std::string &msg)
{
	if (m->violations.size() < 20)
		m->violations.push_back(msg);
}

static bool exec_stmt(Ctx *c, IRStmt *s)
{
	Machine *m = c->m;
	switch (s->kind) {
	case IR_ARRAY_LOAD: {
		ArrayMem *a;
		long lin;
		if (!locate(c, s, &a, &lin))
			return false;
		if (c->mode == MODE_DPU) {
			if (!a->valid[(size_t)lin])
				return fail(c, "PIM read of non-resident element of " + s->array +
				                   " (the partition/halo plan is wrong)");
			c->cnt.mem[s]++;
		} else {
			if (m->pim_dirty.count(s->array))
				note_violation(m, "host read of " + s->array + " while PimDirty (missing invalidate)");
			host_mem(c, a, lin, false);
		}
		c->locals[s->dst] = value_convert(a->v[(size_t)lin], s->dst_type);
		return true;
	}
	case IR_ARRAY_STORE: {
		ArrayMem *a;
		long lin;
		Value v;
		if (!locate(c, s, &a, &lin) || !opd_value(c, s->src, &v))
			return false;
		a->v[(size_t)lin] = value_convert(v, a->type);
		if (c->mode == MODE_DPU) {
			a->valid[(size_t)lin] = 1;
			a->dirty[(size_t)lin] = 1;
			c->cnt.mem[s]++;
		} else {
			if (m->pim_dirty.count(s->array))
				note_violation(m, "host write of " + s->array + " while PimDirty (missing invalidate)");
			m->host_dirty.insert(s->array);
			host_mem(c, a, lin, true);
		}
		return true;
	}
	case IR_BINOP: {
		Value a, b;
		if (!opd_value(c, s->lhs, &a) || !opd_value(c, s->rhs, &b))
			return false;
		c->locals[s->dst] = value_convert(eval_binop(s->op, a, b), s->dst_type);
		if (c->mode == MODE_DPU) {
			if (s->op == OP_MUL) c->cnt.mul++;
			else if (s->op == OP_DIV || s->op == OP_MOD) c->cnt.div++;
			else c->cnt.alu++;
		} else {
			host_alu(c);
		}
		return true;
	}
	case IR_COPY: {
		Value v;
		if (!opd_value(c, s->src, &v))
			return false;
		v = value_convert(v, s->dst_type);
		if (is_temp_name(s->dst))
			c->locals[s->dst] = v;
		else
			(*c->scalars)[s->dst] = v;
		return true;
	}
	case IR_CALL:
		if (c->mode != MODE_HOST)
			return fail(c, "call inside a DPU kernel");
		return run_kernel(m, s->callee);
	case IR_PRINT: {
		Value v;
		if (!opd_value(c, s->src, &v))
			return false;
		m->output.push_back(value_text(v));
		return true;
	}
	case IR_FLUSH:
	case IR_INVALIDATE: {
		MemMap::iterator ai = m->mem.find(s->array);
		if (ai == m->mem.end())
			return fail(c, "coherence operation on unknown array");
		long lo = ai->second.base;
		long hi = lo + (long)ai->second.v.size() * ELEM_BYTES;
		m->cycles += COHERENCE_OP_CYCLES;
		if (s->kind == IR_FLUSH) {
			long n = cache_flush_range(&m->cache, lo, hi);
			m->cycles += (double)n * HOST_DRAM_ACCESS;
			m->bus_bytes += (double)n * CACHE_LINE;
			m->energy_pj += (double)n * DRAM_ACCESS_PJ;
			m->host_dirty.erase(s->array);
		} else {
			long n = cache_invalidate_range(&m->cache, lo, hi);
			m->cycles += (double)n * INVALIDATE_PER_LINE;
			m->pim_dirty.erase(s->array);
		}
		return true;
	}
	}
	return true;
}

static bool exec_loop(Ctx *c, LoopNest *l)
{
	long i;
	for (i = l->lower; l->rel == OP_LT ? i < l->upper : i <= l->upper; i += l->step) {
		c->locals[l->index_var] = mk_val_int(i);
		if (c->mode == MODE_DPU)
			c->cnt.alu += LOOP_OVERHEAD_OPS;
		else {
			c->m->cycles += LOOP_OVERHEAD_OPS * HOST_ALU;
			c->m->energy_pj += LOOP_OVERHEAD_OPS * ALU_PJ;
		}
		if (!exec_body(c, l->body))
			return false;
	}
	return true;
}

static bool exec_body(Ctx *c, std::vector<BodyItem> &body)
{
	size_t i;
	for (i = 0; i < body.size(); i++) {
		BodyItem &it = body[i];
		if (it.kind == BK_STMT) {
			if (!exec_stmt(c, it.stmt))
				return false;
		} else if (it.kind == BK_IF) {
			Value v;
			if (!opd_value(c, it.ifn->cond, &v))
				return false;
			if (c->mode == MODE_DPU) c->cnt.alu += 1; else host_alu(c);
			if (!exec_body(c, truthy(v) ? it.ifn->then_body : it.ifn->else_body))
				return false;
		} else {
			if (!exec_loop(c, it.loop))
				return false;
		}
	}
	return true;
}

/* ============================ PIM execution ================================ */
static int bank_factor(NestPlan *np, IRProgram *p, IRStmt *s)
{
	size_t i;
	for (i = 0; i < np->na.refs.size(); i++) {
		RefInfo &r = np->na.refs[i];
		if (r.stmt != s || r.is_scalar || !r.affine || r.loops.empty())
			continue;
		int jv = (int)r.loops.size() - 1;
		ArrayInfo &ai = p->arrays[r.name];
		long se = 0;
		size_t k, d;
		for (k = 0; k < r.F.size(); k++) {
			long ds = 1;
			for (d = k + 1; d < ai.dims.size(); d++)
				ds *= ai.dims[d];
			se += r.F[k][jv] * ds;
		}
		se = labs(se) * r.loops[jv]->step;
		if (se == 0)
			return 1;
		return gcd_long(se, NUM_BANKS);
	}
	return 1;
}

static Value reduction_identity(int kind, TypeTag t, Value cur)
{
	if (kind == RED_PROD)
		return t == TY_FLOAT ? mk_val_float(1.0f) : mk_val_int(1);
	if (kind == RED_MAX || kind == RED_MIN)
		return cur;
	return t == TY_FLOAT ? mk_val_float(0.0f) : mk_val_int(0);
}

static Value reduction_merge(int kind, Value g, Value p, TypeTag t)
{
	if (kind == RED_PROD)
		return value_convert(eval_binop(OP_MUL, g, p), t);
	if (kind == RED_MAX)
		return truthy(eval_binop(OP_GT, p, g)) ? p : g;
	if (kind == RED_MIN)
		return truthy(eval_binop(OP_LT, p, g)) ? p : g;
	return value_convert(eval_binop(OP_ADD, g, p), t);
}

static long count_runs(std::vector<long> &idx)
{
	long runs = 0;
	size_t i;
	std::sort(idx.begin(), idx.end());
	for (i = 0; i < idx.size(); i++)
		if (i == 0 || idx[i] != idx[i - 1] + 1)
			runs++;
	return runs;
}

/* Run an offloaded nest as the GENERATED DPU PROGRAM (after register allocation),
 * one launch per arithmetic progression of iterations a DPU owns.  Functional
 * only -- this is translation validation, costs are charged by pim_nest(). */
static bool pim_nest_asm(Machine *m, NestPlan *np)
{
	PartitionPlan &pp = np->part;
	IRProgram *p = m->prog;
	DProgram *dp = m->asm_progs[np];
	size_t r, i;
	int d;

	if (!dp) {
		m->err = "no generated DPU program for " + np->name;
		return false;
	}
	for (d = 0; d < pp.ndpus; d++) {
		std::vector<long> iters;
		partition_iterations(&pp, d, &iters);
		i = 0;
		while (i < iters.size()) {
			/* the longest arithmetic progression starting at i */
			long first = iters[i], stride = 1, count = 1;
			if (i + 1 < iters.size()) {
				stride = iters[i + 1] - iters[i];
				count = 2;
				while (i + count < iters.size() && iters[i + count] - iters[i + count - 1] == stride)
					count++;
			}
			DpuLaunch launch;
			launch.first = first;
			launch.count = count;
			launch.stride = stride;
			launch.scalars = m->scalars;
			DpuRun run;
			std::map<std::string, ArrayInfo>::iterator ai;
			for (ai = p->arrays.begin(); ai != p->arrays.end(); ai++)
				run.mram[ai->first] = &m->mem[ai->first].v;
			if (!dpu_interpret(p, dp, &launch, &run)) {
				m->err = "generated DPU program " + np->name + " on DPU " + std::to_string(d) + ": " + run.err;
				return false;
			}
			for (r = 0; r < np->na.reductions.size(); r++) {
				Reduction &red = np->na.reductions[r];
				if (red.is_array)
					continue;
				if (run.results.count(red.var)) {
					Value g = m->scalars[red.var];
					m->scalars[red.var] = reduction_merge(red.kind, g, run.results[red.var], g.type);
				}
			}
			for (r = 0; r < pp.arrays.size(); r++)
				if (pp.arrays[r].mode == AM_PRIVATE && run.priv_out.count(pp.arrays[r].name)) {
					std::vector<Value> &g = m->mem[pp.arrays[r].name].v;
					std::vector<Value> &po = run.priv_out[pp.arrays[r].name];
					for (size_t e = 0; e < g.size(); e++)
						g[e] = reduction_merge(pp.arrays[r].red_kind, g[e], po[e], m->mem[pp.arrays[r].name].type);
				}
			i += (size_t)count;
		}
	}
	m->launches++;
	m->offloaded++;
	for (r = 0; r < pp.arrays.size(); r++)
		if ((pp.arrays[r].written && !pp.arrays[r].local_only) || pp.arrays[r].mode == AM_PRIVATE)
			m->pim_dirty.insert(pp.arrays[r].name);
	return true;
}

static bool pim_nest(Machine *m, NestPlan *np)
{
	if (m->config == SIM_NEMO_ASM)
		return pim_nest_asm(m, np);
	PartitionPlan &pp = np->part;
	IRProgram *p = m->prog;
	LoopNest *L = np->root;
	int P = pp.ndpus;
	int d;
	size_t r, i;

	double in_bytes = 0, out_bytes = 0, in_runs = 0, out_runs = 0, merge_cycles = 0;
	int sets = 0;
	double max_dpu = 0, dpu_mem_accesses = 0, dpu_ops = 0;
	std::vector<ScalarMap> part_scalars(P);
	m->writer.clear();

	/* coherence: every array the DPUs touch must be flushed by now */
	for (r = 0; r < pp.arrays.size(); r++)
		if (!pp.arrays[r].local_only && m->host_dirty.count(pp.arrays[r].name))
			note_violation(m, "PIM nest " + np->name + " touches " + pp.arrays[r].name +
			                      " while HostDirty (missing flush)");

	std::vector<std::vector<long> > iters(P);
	for (d = 0; d < P; d++)
		partition_iterations(&pp, d, &iters[d]);

	/* transfer accounting is per array: do the in-direction first */
	std::map<std::string, double> in_b, out_b, in_r, out_r;

	for (d = 0; d < P; d++) {
		if (iters[d].empty())
			continue;

		MemMap img;
		ScalarMap simg = m->scalars;

		for (r = 0; r < pp.arrays.size(); r++) {
			ArrayRole &role = pp.arrays[r];
			ArrayMem &g = m->mem[role.name];
			ArrayMem a;
			a.dims = g.dims;
			a.type = g.type;
			a.base = g.base;
			a.v = g.v;
			size_t total = g.v.size();
			long rows = g.dims[0];
			long rowelems = (long)total / rows;
			a.valid.assign(total, 0);
			a.dirty.assign(total, 0);

			if (role.mode == AM_PRIVATE) {
				/* private accumulator: starts at the reduction identity */
				Value id = reduction_identity(role.red_kind, a.type,
				                              a.type == TY_FLOAT ? mk_val_float(0.0f) : mk_val_int(0));
				for (i = 0; i < total; i++)
					a.v[i] = id;
				a.valid.assign(total, 1);
			} else if (role.mode == AM_BROADCAST || (role.mode == AM_SCATTER && role.read)) {
				a.valid.assign(total, 1);
				if (!role.local_only) {
					in_b[role.name] += (double)total * ELEM_BYTES;
					in_r[role.name] += 1;
				}
			} else if (role.mode == AM_PARTITIONED && role.read) {
				std::vector<char> flag((size_t)rows, 0);
				size_t t;
				for (t = 0; t < iters[d].size(); t++) {
					long iv = L->lower + L->step * iters[d][t];
					long row0 = role.row_coeff * iv + role.row_off;
					long rr;
					for (rr = row0 - role.halo_lo; rr <= row0 + role.halo_hi; rr++)
						if (rr >= 0 && rr < rows)
							flag[(size_t)rr] = 1;
				}
				long nrows = 0, runs = 0;
				for (long rr = 0; rr < rows; rr++)
					if (flag[(size_t)rr]) {
						nrows++;
						if (rr == 0 || !flag[(size_t)rr - 1])
							runs++;
						for (long e = 0; e < rowelems; e++)
							a.valid[(size_t)(rr * rowelems + e)] = 1;
					}
				if (!role.local_only) {
					in_b[role.name] += (double)nrows * rowelems * ELEM_BYTES;
					in_r[role.name] += (double)runs;
				}
			}
			img[role.name] = a;
		}

		/* private scalar reductions start from the identity */
		for (r = 0; r < np->na.reductions.size(); r++) {
			Reduction &red = np->na.reductions[r];
			if (red.is_array)
				continue;
			TypeTag t = m->scalars[red.var].type;
			simg[red.var] = reduction_identity(red.kind, t, m->scalars[red.var]);
		}

		Ctx dc;
		dc.m = m;
		dc.mode = MODE_DPU;
		dc.mem = &img;
		dc.scalars = &simg;
		dc.cnt.alu = dc.cnt.mul = dc.cnt.div = 0;

		size_t t;
		for (t = 0; t < iters[d].size(); t++) {
			long iv = L->lower + L->step * iters[d][t];
			dc.locals[L->index_var] = mk_val_int(iv);
			dc.cnt.alu += LOOP_OVERHEAD_OPS;
			/* strength-reduced accumulators of this loop: recompute from the index
			 * so any subset of iterations (cyclic distribution) is correct */
			std::map<std::string, IVInfo>::iterator ivs;
			for (ivs = p->ivs.begin(); ivs != p->ivs.end(); ivs++)
				if (ivs->second.basic == L->index_var)
					dc.locals[ivs->first] = mk_val_int(ivs->second.coeff * iv + ivs->second.offset);
			if (!exec_body(&dc, L->body)) {
				m->err = "DPU " + std::to_string(d) + " of " + np->name + ": " + dc.err;
				return false;
			}
		}

		/* DPU time */
		double overlap = std::min<double>(TASKLETS_PER_DPU, TASKLET_SATURATION);
		double mem_cycles = 0, accesses = 0;
		std::map<IRStmt*, long>::iterator mi;
		for (mi = dc.cnt.mem.begin(); mi != dc.cnt.mem.end(); mi++) {
			mem_cycles += (double)mi->second * (PIM_LOCAL_ACCESS / overlap) * bank_factor(np, p, mi->first);
			accesses += (double)mi->second;
		}
		double dpu = dc.cnt.alu * PIM_ALU + dc.cnt.mul * PIM_MUL_CYCLES + dc.cnt.div * PIM_DIV_CYCLES +
		             mem_cycles;
		max_dpu = std::max(max_dpu, dpu);
		dpu_mem_accesses += accesses;
		dpu_ops += dc.cnt.alu + dc.cnt.mul + dc.cnt.div;

		/* results back */
		for (r = 0; r < pp.arrays.size(); r++) {
			ArrayRole &role = pp.arrays[r];
			ArrayMem &a = img[role.name];
			ArrayMem &g = m->mem[role.name];
			if (role.mode == AM_PRIVATE) {
				for (i = 0; i < g.v.size(); i++)
					g.v[i] = reduction_merge(role.red_kind, g.v[i], a.v[i], g.type);
				out_b[role.name] += (double)g.v.size() * ELEM_BYTES;
				out_r[role.name] += 1;
				merge_cycles += (double)g.v.size() * (HOST_ALU + HOST_CACHE_HIT);
				continue;
			}
			if (!role.written)
				continue;
			std::vector<long> dirty;
			std::vector<int> &w = m->writer[role.name];
			if (w.empty())
				w.assign(g.v.size(), -1);
			for (i = 0; i < a.dirty.size(); i++)
				if (a.dirty[i]) {
					if (w[i] >= 0 && w[i] != d) {
						m->err = "write-write race on " + role.name + " between DPUs in " + np->name;
						return false;
					}
					w[i] = d;
					if (!role.local_only)
						g.v[i] = a.v[i];
					dirty.push_back((long)i);
				}
			if (!role.local_only && !dirty.empty()) {
				out_b[role.name] += (double)dirty.size() * ELEM_BYTES;
				out_r[role.name] += (double)count_runs(dirty);
			}
		}
		for (r = 0; r < np->na.reductions.size(); r++)
			if (!np->na.reductions[r].is_array)
				part_scalars[d][np->na.reductions[r].var] = simg[np->na.reductions[r].var];
	}

	/* merge scalar reductions on the host */
	for (r = 0; r < np->na.reductions.size(); r++) {
		Reduction &red = np->na.reductions[r];
		if (red.is_array)
			continue;
		Value g = m->scalars[red.var];
		for (d = 0; d < P; d++)
			if (part_scalars[d].count(red.var))
				g = reduction_merge(red.kind, g, part_scalars[d][red.var], g.type);
		m->scalars[red.var] = g;
		merge_cycles += (double)P * (HOST_ALU + 1);
	}

	/* ---- accounting ------------------------------------------------------ */
	std::map<std::string, double>::iterator it;
	for (it = in_b.begin(); it != in_b.end(); it++) {
		in_bytes += it->second;
		sets++;
	}
	for (it = in_r.begin(); it != in_r.end(); it++)
		in_runs += it->second;
	for (it = out_b.begin(); it != out_b.end(); it++) {
		out_bytes += it->second;
		sets++;
	}
	for (it = out_r.begin(); it != out_r.end(); it++)
		out_runs += it->second;

	double dma = (in_bytes + out_bytes) * DMA_PER_BYTE + (in_runs + out_runs) * DMA_RUN_SETUP +
	             sets * DMA_SETUP;
	m->cycles += LAUNCH_OVERHEAD + dma + max_dpu + merge_cycles;
	m->bus_bytes += in_bytes + out_bytes;
	/* energy: each 64-byte burst of DMA crosses the bus (a DRAM access) and
	 * touches the DPU's bank (a local access); the DPU's compute then runs out
	 * of its scratchpad, whose accesses cost about one ALU operation each */
	m->energy_pj += ((in_bytes + out_bytes) / CACHE_LINE) * (DRAM_ACCESS_PJ + PIM_ACCESS_PJ) +
	                (dpu_mem_accesses + dpu_ops) * ALU_PJ;
	m->launches++;
	m->offloaded++;

	for (r = 0; r < pp.arrays.size(); r++)
		if (pp.arrays[r].written && !pp.arrays[r].local_only)
			m->pim_dirty.insert(pp.arrays[r].name);
		else if (pp.arrays[r].mode == AM_PRIVATE)
			m->pim_dirty.insert(pp.arrays[r].name);
	return true;
}

/* ============================ running a program ============================= */
static bool run_kernel(Machine *m, const std::string &name)
{
	Kernel *k = ir_find_kernel(m->prog, name);
	if (!k)
		return false;
	KernelPlan *kp = 0;
	size_t i, ni = 0;
	if (m->pp)
		for (i = 0; i < m->pp->kernels.size(); i++)
			if (m->pp->kernels[i].kernel == k)
				kp = &m->pp->kernels[i];

	double c0 = m->cycles;
	Ctx hc;
	hc.m = m;
	hc.mode = MODE_HOST;
	hc.mem = &m->mem;
	hc.scalars = &m->scalars;
	hc.cnt.alu = hc.cnt.mul = hc.cnt.div = 0;

	for (i = 0; i < k->body.size(); i++) {
		BodyItem &it = k->body[i];
		if (it.kind == BK_LOOP) {
			NestPlan *np = (kp && ni < kp->nests.size()) ? kp->nests[ni] : 0;
			ni++;
			if (np && np->offload && m->config != SIM_HOST_ONLY) {
				if (!pim_nest(m, np))
					return false;
			} else if (!exec_loop(&hc, it.loop)) {
				m->err = hc.err;
				return false;
			}
		} else if (it.kind == BK_IF) {
			Value v;
			if (!opd_value(&hc, it.ifn->cond, &v))
				return false;
			if (!exec_body(&hc, truthy(v) ? it.ifn->then_body : it.ifn->else_body)) {
				m->err = hc.err;
				return false;
			}
		} else if (!exec_stmt(&hc, it.stmt)) {
			m->err = hc.err;
			return false;
		}
	}
	m->kernel_cycles += m->cycles - c0;
	return true;
}

static bool run_program(Machine *m)
{
	Kernel *host = 0;
	size_t i;
	for (i = 0; i < m->prog->hosts.size(); i++)
		if (m->prog->hosts[i]->name == "main")
			host = m->prog->hosts[i];
	if (!host && !m->prog->hosts.empty())
		host = m->prog->hosts[0];
	if (!host) {
		m->err = "program has no host block";
		return false;
	}
	Ctx hc;
	hc.m = m;
	hc.mode = MODE_HOST;
	hc.mem = &m->mem;
	hc.scalars = &m->scalars;
	hc.cnt.alu = hc.cnt.mul = hc.cnt.div = 0;
	if (!exec_body(&hc, host->body)) {
		if (m->err.empty())
			m->err = hc.err;
		return false;
	}
	return true;
}

/* ============================ verification ================================== */
static std::string compare_machines(Machine *ref, Machine *got, std::set<std::string> &dead)
{
	MemMap::iterator ai;
	size_t i;
	for (ai = ref->mem.begin(); ai != ref->mem.end(); ai++) {
		if (dead.count(ai->first))
			continue;
		ArrayMem &a = ai->second;
		ArrayMem &b = got->mem[ai->first];
		for (i = 0; i < a.v.size(); i++)
			if (!value_equal_bits(a.v[i], b.v[i])) {
				char buf[200];
				snprintf(buf, sizeof buf, "array %s[%zu]: host %s vs PIM %s", ai->first.c_str(), i,
				         value_text(a.v[i]).c_str(), value_text(b.v[i]).c_str());
				return buf;
			}
	}
	ScalarMap::iterator si;
	for (si = ref->scalars.begin(); si != ref->scalars.end(); si++)
		if (!value_equal_bits(si->second, got->scalars[si->first]))
			return "scalar " + si->first + " differs";
	if (ref->output != got->output)
		return "printed output differs";
	return "";
}

/* build, plan, run one configuration */
static bool run_config(const char *path, int config, Machine *m, bool optimise)
{
	int ne = 0;
	Program *ast = frontend_parse(path, &ne);
	if (!ast || semantic_check(ast) > 0)
		return false;
	IRProgram *p = lower_program(ast);
	if (optimise)
		opt_run_all(p, "");
	ProgramPlan *pp = 0;
	if (config == SIM_NAIVE_OFFLOAD) {
		pp = plan_program(p, false);
		coherence_run(pp, 0);
	} else if (config == SIM_NEMO_C || config == SIM_NEMO_ASM) {
		pp = plan_program(p, true);
		coherence_run(pp, 0);
		if (config == SIM_NEMO_ASM) {
			size_t k, n;
			for (k = 0; k < pp->kernels.size(); k++)
				for (n = 0; n < pp->kernels[k].nests.size(); n++) {
					NestPlan *np = pp->kernels[k].nests[n];
					if (!np->offload)
						continue;
					DProgram *dp = new DProgram();
					std::string err;
					if (!dpu_codegen(p, np, dp, &err)) {
						m->err = "codegen: " + err;
						return false;
					}
					RAResult ra;
					regalloc_run(dp, NUM_REGS, &ra, 0);
					if (!ra.verified) {
						m->err = "register allocation produced an invalid colouring";
						return false;
					}
					m->asm_progs[np] = dp;
				}
		}
	}
	m->pp = pp;
	m->config = config;
	machine_init(m, p);
	m->pp = pp;
	return run_program(m);
}

int sim_benchmark(const char *path, const std::string &name, FILE *csv, int verbose,
                  std::vector<SimResult> *results)
{
	Machine *ms[3];
	std::set<std::string> none, dead;
	int c, bad = 0;

	for (c = 0; c < 3; c++) {
		ms[c] = new Machine();
		ms[c]->err.clear();
		ms[c]->pp = 0;
		if (!run_config(path, c, ms[c], true)) {
			fprintf(stderr, "%s [%s]: simulation failed: %s\n", name.c_str(), sim_config_name(c),
			        ms[c]->err.c_str());
			return 1;
		}
	}

	/* the optimiser must not change what the program computes */
	Machine noopt;
	noopt.pp = 0;
	bool opt_ok = run_config(path, SIM_HOST_ONLY, &noopt, false);
	std::string opt_note = opt_ok ? compare_machines(&noopt, ms[SIM_HOST_ONLY], none) : "run failed";

	for (std::set<std::string>::iterator it = ms[SIM_NEMO_C]->pp->dead_arrays.begin();
	     it != ms[SIM_NEMO_C]->pp->dead_arrays.end(); it++)
		dead.insert(*it);

	std::vector<SimResult> local;
	for (c = 0; c < 3; c++) {
		SimResult r;
		Machine *m = ms[c];
		r.benchmark = name;
		r.config = sim_config_name(c);
		r.cycles = m->cycles;
		r.kernel_cycles = m->kernel_cycles;
		r.bus_bytes = m->bus_bytes;
		r.energy_pj = m->energy_pj;
		r.offloaded_nests = m->offloaded;
		r.launches = m->launches;
		r.coherence_violations = (int)m->violations.size();
		r.output = m->output;
		std::string note = (c == SIM_HOST_ONLY) ? "" : compare_machines(ms[SIM_HOST_ONLY], m, dead);
		r.verified = note.empty() && m->violations.empty();
		if (!note.empty())
			r.verify_note = note;
		else if (!m->violations.empty())
			r.verify_note = "coherence: " + m->violations[0];
		else
			r.verify_note = "bit-identical to HOST_ONLY";
		if (!r.verified)
			bad = 1;
		local.push_back(r);
	}
	if (!opt_note.empty()) {
		bad = 1;
		fprintf(stderr, "%s: optimiser changed the result: %s\n", name.c_str(), opt_note.c_str());
	}

	/* translation validation: run the generated DPU programs (K = NUM_REGS physical
	 * registers, scratchpad tiling, spills) and demand the host's result */
	Machine asmm;
	asmm.pp = 0;
	std::string asm_note;
	if (!run_config(path, SIM_NEMO_ASM, &asmm, true))
		asm_note = "generated-code run failed: " + asmm.err;
	else
		asm_note = compare_machines(ms[SIM_HOST_ONLY], &asmm, dead);
	if (!asm_note.empty()) {
		bad = 1;
		fprintf(stderr, "%s: generated DPU code disagrees with the host: %s\n", name.c_str(),
		        asm_note.c_str());
	}

	printf("%-12s %-14s %14s %14s %12s %12s %5s  %s\n", "benchmark", "config", "cycles", "kernel cycles",
	       "bus bytes", "energy(nJ)", "PIM", "check");
	for (c = 0; c < 3; c++) {
		SimResult &r = local[c];
		printf("%-12s %-14s %14.0f %14.0f %12.0f %12.1f %5d  %s\n", name.c_str(), r.config.c_str(),
		       r.cycles, r.kernel_cycles, r.bus_bytes, r.energy_pj / 1000.0, r.offloaded_nests,
		       r.verified ? "OK" : ("FAILED: " + r.verify_note).c_str());
	}
	printf("  speedup vs HOST_ONLY (total cycles):  NAIVE %.2fx   NEMO_C %.2fx\n",
	       local[0].cycles / local[1].cycles, local[0].cycles / local[2].cycles);
	printf("  optimiser check: %s     generated-DPU-code check: %s\n",
	       opt_note.empty() ? "OK (unoptimised IR gives the identical result)" : "FAILED",
	       asm_note.empty() ? (asmm.offloaded ? "OK (executed the allocated DPU programs)"
	                                           : "OK (nothing offloaded)")
	                        : "FAILED");
	if (verbose)
		for (c = 0; c < 3; c++)
			printf("  [%s] %s\n", local[c].config.c_str(), local[c].verify_note.c_str());

	if (csv) {
		for (c = 0; c < 3; c++) {
			SimResult &r = local[c];
			fprintf(csv, "%s,%s,cycles,%.0f\n", name.c_str(), r.config.c_str(), r.cycles);
			fprintf(csv, "%s,%s,kernel_cycles,%.0f\n", name.c_str(), r.config.c_str(), r.kernel_cycles);
			fprintf(csv, "%s,%s,bus_bytes,%.0f\n", name.c_str(), r.config.c_str(), r.bus_bytes);
			fprintf(csv, "%s,%s,energy_pj,%.0f\n", name.c_str(), r.config.c_str(), r.energy_pj);
			fprintf(csv, "%s,%s,pim_nests,%d\n", name.c_str(), r.config.c_str(), r.offloaded_nests);
			fprintf(csv, "%s,%s,verified,%d\n", name.c_str(), r.config.c_str(), r.verified ? 1 : 0);
		}
		fprintf(csv, "%s,NEMO_C,asm_verified,%d\n", name.c_str(), asm_note.empty() ? 1 : 0);
	}
	if (results)
		*results = local;
	return bad;
}
