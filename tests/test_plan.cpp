/* Phases E-G: offload decisions, transformations, partitioning, coherence. */
#include "testutil.h"
#include <vector>
#include "frontend.h"
#include "plan.h"

static ProgramPlan *plan_of(const char *path, bool nemo)
{
	IRProgram *p = frontend_compile(path, 1);
	if (!p)
		return 0;
	return plan_program(p, nemo);
}

static NestPlan *only_nest(ProgramPlan *pp)
{
	return pp->kernels[0].nests[0];
}

static int count_ops(std::vector<BodyItem> &b, int kind)
{
	std::vector<IRStmt*> all;
	int n = 0;
	ir_collect_stmts(b, &all);
	for (size_t i = 0; i < all.size(); i++)
		if (all[i]->kind == kind)
			n++;
	return n;
}

int main(void)
{
	ProgramPlan *pp;

	/* ---- the expected verdict of every benchmark ------------------------------ */
	struct { const char *bench; bool offload; int pclass; } expected[] = {
		{"vecadd", true, PC_ELEMENTWISE}, {"saxpy", true, PC_ELEMENTWISE},
		{"dotprod", true, PC_REDUCTION}, {"maxreduce", true, PC_REDUCTION},
		{"histogram", true, PC_REDUCTION}, {"gemv", true, PC_ELEMENTWISE},
		{"stencil1d", true, PC_STENCIL}, {"stencil2d", true, PC_STENCIL},
		{"filter", true, PC_ELEMENTWISE}, {"matmul", false, PC_COMPUTE_BOUND},
		{"indirect", false, PC_IRREGULAR},
	};
	for (size_t i = 0; i < sizeof expected / sizeof expected[0]; i++) {
		std::string path = std::string("benchmarks/") + expected[i].bench + ".nmc";
		pp = plan_of(path.c_str(), true);
		CHECK(pp != 0);
		if (!pp)
			continue;
		NestPlan *np = only_nest(pp);
		if (np->offload != expected[i].offload)
			printf("  %s: offload=%d expected %d (%s)\n", expected[i].bench, np->offload,
			       expected[i].offload, np->dec.reason.c_str());
		CHECK(np->offload == expected[i].offload);
		CHECK_INT(np->dec.pclass, expected[i].pclass);
	}

	/* arithmetic intensity: the two numbers the course gives for matmul and vecadd */
	pp = plan_of("benchmarks/matmul.nmc", true);
	CHECK(only_nest(pp)->dec.ai > 1.99 && only_nest(pp)->dec.ai < 2.01);
	pp = plan_of("benchmarks/vecadd.nmc", true);
	CHECK(only_nest(pp)->dec.ai > 0.08 && only_nest(pp)->dec.ai < 0.09);

	/* size check: an elementwise nest with AI far below the balance is still kept on
	 * the host when it is too small to pay for the launch; the same shape at
	 * 16384 elements (saxpy) is offloaded */
	pp = plan_of("tests/tiny_size.nmc", true);
	CHECK(!only_nest(pp)->offload);
	CHECK(only_nest(pp)->dec.ai < 0.5);
	CHECK(only_nest(pp)->dec.host_est <= only_nest(pp)->dec.pim_fixed);
	CHECK_INT(only_nest(pp)->dec.pclass, PC_ELEMENTWISE);
	pp = plan_of("benchmarks/saxpy.nmc", true);
	CHECK(only_nest(pp)->offload);
	CHECK(only_nest(pp)->dec.host_est > only_nest(pp)->dec.pim_fixed);

	/* transpose: profitable on paper, but partitioning needs an all-to-all shuffle */
	pp = plan_of("benchmarks/transpose.nmc", true);
	CHECK(!only_nest(pp)->offload);
	CHECK(!only_nest(pp)->part.feasible);
	CHECK(only_nest(pp)->part.reason.size() > 0);

	/* stencil: one halo row on each side of the partition */
	pp = plan_of("benchmarks/stencil1d.nmc", true);
	bool halo = false;
	for (size_t a = 0; a < only_nest(pp)->part.arrays.size(); a++) {
		ArrayRole &r = only_nest(pp)->part.arrays[a];
		if (r.name == "A" && r.halo_lo == 1 && r.halo_hi == 1)
			halo = true;
	}
	CHECK(halo);

	/* block distribution wins for an elementwise kernel; forcing the others still
	 * produces a plan, which costs more */
	pp = plan_of("benchmarks/vecadd.nmc", true);
	CHECK_INT(only_nest(pp)->part.dist, DIST_BLOCK);
	CHECK_INT(only_nest(pp)->part.ndpus, 64);
	double block = only_nest(pp)->part.considered[0].score;
	CHECK(only_nest(pp)->part.considered[1].score > block);
	CHECK(only_nest(pp)->part.considered[2].score > block);

	/* a reduction becomes a private accumulator */
	pp = plan_of("benchmarks/histogram.nmc", true);
	bool priv = false;
	for (size_t a = 0; a < only_nest(pp)->part.arrays.size(); a++)
		if (only_nest(pp)->part.arrays[a].mode == AM_PRIVATE)
			priv = true;
	CHECK(priv);
	pp = plan_of("benchmarks/maxreduce.nmc", true);
	CHECK(only_nest(pp)->na.reductions.size() == 1 && only_nest(pp)->na.reductions[0].kind == RED_MAX);

	/* ---- fission: the mixed loop splits into an offloadable and a host loop ------ */
	pp = plan_of("benchmarks/mixed_loop.nmc", true);
	CHECK_INT(pp->kernels[0].nests.size(), 2);
	CHECK(pp->kernels[0].nests[0]->offload);
	CHECK(!pp->kernels[0].nests[1]->offload);
	CHECK_INT(pp->transforms.size(), 1);
	CHECK_STR(pp->transforms[0].kind, "fission");

	/* ---- fusion: the chain becomes one nest and the intermediate never leaves PIM ---- */
	pp = plan_of("benchmarks/fusion_chain.nmc", true);
	CHECK_INT(pp->kernels[0].nests.size(), 1);
	CHECK(pp->kernels[0].nests[0]->offload);
	CHECK(pp->dead_arrays.count("T") == 1);
	CHECK_STR(pp->transforms[0].kind, "fusion");
	CHECK(pp->transforms[0].bytes_saved > 100000);

	/* fusion is refused when a dependence would be reversed: loop 2 reads
	 * A[i+1] which loop 1 has not yet written when fused */
	{
		IRProgram *p = frontend_compile_text(
		    "array float A[64]; array float B[64];\n"
		    "kernel k() { for (i = 0; i < 63; i += 1) { A[i] = 1.0; }\n"
		    "             for (i = 0; i < 63; i += 1) { B[i] = A[i + 1]; } }\n"
		    "host h() { call k(); }\n", 1);
		pp = plan_program(p, true);
		CHECK_INT(pp->kernels[0].nests.size(), 2);
	}

	/* ---- interchange ---------------------------------------------------------------- */
	pp = plan_of("benchmarks/colmajor.nmc", true);
	CHECK(only_nest(pp)->offload);
	CHECK_STR(only_nest(pp)->root->index_var, "i");
	CHECK_STR(pp->transforms[0].kind, "interchange");

	/* interchange is refused when a direction vector would become negative */
	{
		IRProgram *p = frontend_compile_text(
		    "array float A[16, 16];\n"
		    "kernel k() { for (i = 1; i < 15; i += 1) { for (j = 0; j < 15; j += 1) {\n"
		    "    A[i, j] = A[i - 1, j + 1]; } } }\n"
		    "host h() { call k(); }\n", 1);
		pp = plan_program(p, true);
		CHECK_STR(only_nest(pp)->root->index_var, "i");
	}

	/* ---- coherence -------------------------------------------------------------------- */
	pp = plan_of("benchmarks/saxpy.nmc", true);
	coherence_run(pp, 0);
	Kernel *saxpy = ir_find_kernel(pp->prog, "saxpy");
	Kernel *mainh = ir_find_kernel(pp->prog, "main");
	CHECK_INT(count_ops(saxpy->body, IR_FLUSH), 2);              /* A and B were written by the host */
	CHECK_INT(count_ops(mainh->body, IR_INVALIDATE), 2);         /* C is read in each branch */

	/* the if/else merge: the array is PimDirty on the fall-through path, so the read
	 * AFTER the merge still needs an invalidate */
	pp = plan_of("tests/coherence_demo.nmc", true);
	coherence_run(pp, 0);
	mainh = ir_find_kernel(pp->prog, "main");
	CHECK_INT(count_ops(mainh->body, IR_INVALIDATE), 2);

	/* host-only kernels need no coherence at all */
	pp = plan_of("benchmarks/matmul.nmc", true);
	coherence_run(pp, 0);
	CHECK_INT(count_ops(ir_find_kernel(pp->prog, "main")->body, IR_FLUSH) +
	          count_ops(ir_find_kernel(pp->prog, "main")->body, IR_INVALIDATE), 0);

	TEST_DONE();
}
