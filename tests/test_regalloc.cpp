/* Chaitin-Briggs register allocation: hand-built programs with known answers,
 * then generated programs run under heavy register pressure. */
#include "testutil.h"
#include <map>
#include <vector>
#include "frontend.h"
#include "plan.h"
#include "dpu.h"

/* ---- a tiny builder for hand-written DPU programs -------------------------------- */
struct B {
	DProgram p;
	B() { p.nvregs = 0; p.allocated = false; p.nregs = 0; p.nspill = 0; p.tile_rows = 1; p.strided = false; }
	int reg(const char *name) {
		p.vreg_name.push_back(name);
		p.vreg_float.push_back(false);
		return p.nvregs++;
	}
	void op(int o, int d, int a, int b, long imm = 0) {
		DInstr I;
		I.op = o; I.fp = false; I.d = d; I.a = a; I.b = b; I.imm = imm; I.fimm = 0;
		p.code.push_back(I);
	}
};

static bool colouring_valid(DProgram &, RAResult &r)
{
	for (size_t v = 0; v < r.graph.size(); v++)
		for (size_t q = 0; q < r.graph[v].size(); q++)
			if (r.color[v] >= 0 && r.color[v] == r.color[r.graph[v][q]])
				return false;
	return true;
}

/* run every DPU of a plan's nest through the interpreter */
static bool run_all_dpus(IRProgram *p, NestPlan *np, DProgram *dp,
                         std::map<std::string, std::vector<Value> > &mem)
{
	for (int d = 0; d < np->part.ndpus; d++) {
		std::vector<long> it;
		partition_iterations(&np->part, d, &it);
		if (it.empty())
			continue;
		DpuLaunch l;
		l.first = it[0];
		l.count = (long)it.size();
		l.stride = it.size() > 1 ? it[1] - it[0] : 1;
		DpuRun run;
		std::map<std::string, ArrayInfo>::iterator ai;
		for (ai = p->arrays.begin(); ai != p->arrays.end(); ai++)
			run.mram[ai->first] = &mem[ai->first];
		if (!dpu_interpret(p, dp, &l, &run)) {
			printf("  interpreter: %s\n", run.err.c_str());
			return false;
		}
	}
	return true;
}

int main(void)
{
	/* ---- three registers live at once need three colours ---------------------------- */
	{
		B b;
		int x = b.reg("x"), y = b.reg("y"), z = b.reg("z"), s = b.reg("s"), t = b.reg("t");
		b.op(D_MOVI, x, -1, -1, 1);
		b.op(D_MOVI, y, -1, -1, 2);
		b.op(D_MOVI, z, -1, -1, 3);
		b.op(D_ADD, s, x, y);
		b.op(D_ADD, t, s, z);
		b.op(D_RET, -1, t, -1);
		b.p.code.back().sym = "r";

		DProgram p3 = b.p;
		RAResult r3;
		regalloc_run(&p3, 3, &r3, 0);
		CHECK_INT(r3.spilled, 0);                 /* K = 3 suffices */
		CHECK(r3.verified && colouring_valid(p3, r3));

		DProgram p2 = b.p;
		RAResult r2;
		regalloc_run(&p2, 2, &r2, 0);
		CHECK(r2.spilled >= 1);                   /* K = 2 cannot: something spills */
		CHECK(r2.verified);
		/* the spilled program still computes 1+2+3 */
		DpuLaunch l;
		l.first = 0; l.count = 0; l.stride = 1;
		DpuRun run;
		IRProgram dummy;
		CHECK(dpu_interpret(&dummy, &p2, &l, &run));
		CHECK_INT(run.results["r"].i, 6);
	}

	/* ---- a copy between non-interfering registers is coalesced ----------------------- */
	{
		B b;
		int a = b.reg("a"), c = b.reg("c"), r = b.reg("r");
		b.op(D_MOVI, a, -1, -1, 7);
		b.op(D_MOV, c, a, -1);                    /* c := a, and a is dead afterwards */
		b.op(D_ADD, r, c, c);
		b.op(D_RET, -1, r, -1);
		b.p.code.back().sym = "r";
		RAResult ra;
		regalloc_run(&b.p, 4, &ra, 0);
		CHECK(ra.coalesced.size() >= 1);
		CHECK(ra.verified);
		DpuLaunch l;
		l.first = 0; l.count = 0; l.stride = 1;
		DpuRun run;
		IRProgram dummy;
		CHECK(dpu_interpret(&dummy, &b.p, &l, &run));
		CHECK_INT(run.results["r"].i, 14);
	}

	/* ---- a loop: the index and the accumulator are live across the back edge -------------- */
	{
		B b;
		int i = b.reg("i"), n = b.reg("n"), acc = b.reg("acc"), one = b.reg("one");
		b.op(D_MOVI, i, -1, -1, 0);
		b.op(D_MOVI, n, -1, -1, 10);
		b.op(D_MOVI, acc, -1, -1, 0);
		b.op(D_LOOP, -1, i, n);
		b.op(D_ADD, acc, acc, i);
		b.op(D_MOVI, one, -1, -1, 1);
		b.op(D_ADD, i, i, one);
		b.op(D_ENDLOOP, -1, -1, -1);
		b.op(D_RET, -1, acc, -1);
		b.p.code.back().sym = "sum";
		RAResult ra;
		regalloc_run(&b.p, 4, &ra, 0);
		CHECK(ra.verified && ra.spilled == 0);
		DpuLaunch l;
		l.first = 0; l.count = 0; l.stride = 1;
		DpuRun run;
		IRProgram dummy;
		CHECK(dpu_interpret(&dummy, &b.p, &l, &run));
		CHECK_INT(run.results["sum"].i, 45);      /* 0+1+...+9 */
	}

	/* ---- generated code under register pressure ------------------------------------------- */
	IRProgram *p = frontend_compile("benchmarks/stencil2d.nmc", 1);
	ProgramPlan *pp = plan_program(p, true);
	NestPlan *np = pp->kernels[0].nests[0];
	CHECK(np->offload);

	DProgram wide, narrow;
	std::string err;
	CHECK(dpu_codegen(p, np, &wide, &err));
	CHECK(dpu_codegen(p, np, &narrow, &err));
	RAResult rw, rn;
	regalloc_run(&wide, 24, &rw, 0);
	regalloc_run(&narrow, 5, &rn, 0);
	CHECK(rw.verified && rn.verified);
	CHECK_INT(rw.spilled, 0);                     /* 24 registers are plenty */
	CHECK(rn.spilled > 0);                        /* 5 force real spills */
	CHECK(narrow.nspill > 0);
	CHECK(rn.registers_used <= 5);

	/* both allocations compute exactly the same arrays */
	std::map<std::string, std::vector<Value> > m1, m2;
	std::map<std::string, ArrayInfo>::iterator ai;
	for (ai = p->arrays.begin(); ai != p->arrays.end(); ai++) {
		long n = 1;
		for (size_t d = 0; d < ai->second.dims.size(); d++)
			n *= ai->second.dims[d];
		std::vector<Value> v;
		for (long e = 0; e < n; e++)
			v.push_back(mk_val_float((float)((e * 7) % 23) * 0.25f));
		m1[ai->first] = v;
		m2[ai->first] = v;
	}
	CHECK(run_all_dpus(p, np, &wide, m1));
	CHECK(run_all_dpus(p, np, &narrow, m2));
	bool same = true;
	for (size_t e = 0; e < m1["B"].size(); e++)
		if (!value_equal_bits(m1["B"][e], m2["B"][e]))
			same = false;
	CHECK(same);
	/* and the result is not trivially zero */
	CHECK(m1["B"][5 * 128 + 5].f != 0.0f);

	/* the cyclic distribution generates (and computes) the same result */
	g_force_dist = DIST_CYCLIC;
	IRProgram *p2 = frontend_compile("benchmarks/stencil2d.nmc", 1);
	ProgramPlan *pp2 = plan_program(p2, true);
	g_force_dist = -1;
	NestPlan *np2 = pp2->kernels[0].nests[0];
	CHECK_INT(np2->part.dist, DIST_CYCLIC);
	DProgram cyc;
	CHECK(dpu_codegen(p2, np2, &cyc, &err));
	RAResult rc;
	regalloc_run(&cyc, 24, &rc, 0);
	std::map<std::string, std::vector<Value> > m3 = m1;
	for (size_t e = 0; e < m3["B"].size(); e++)
		m3["B"][e] = mk_val_float(0.0f);     /* clear B; A is not written by the kernel */
	m3["A"] = m2["A"];
	CHECK(run_all_dpus(p2, np2, &cyc, m3));
	bool same2 = true;
	for (size_t e = 0; e < m1["B"].size(); e++) {
		long row = (long)e / 128, col = (long)e % 128;
		if (row < 1 || row > 126 || col < 1 || col > 126)
			continue;                           /* only the interior is written */
		if (!value_equal_bits(m1["B"][e], m3["B"][e]))
			same2 = false;
	}
	CHECK(same2);

	TEST_DONE();
}
