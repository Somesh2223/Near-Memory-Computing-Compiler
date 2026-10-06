/* The headline checks: for every benchmark, the host and the PIM executions are
 * bit-identical (including the generated DPU code), and the cost model's
 * verdicts show up in the cycle counts. */
#include "testutil.h"
#include <map>
#include <vector>
#include "sim.h"
#include "plan.h"

int main(void)
{
	const char *offloaded[] = {"vecadd", "saxpy", "dotprod", "maxreduce", "histogram", "gemv",
	                           "stencil1d", "stencil2d", "filter", "fusion_chain", "mixed_loop",
	                           "colmajor"};
	const char *rejected[]  = {"matmul", "transpose", "indirect"};
	std::map<std::string, std::vector<SimResult> > done;
	size_t i;

	for (i = 0; i < sizeof offloaded / sizeof offloaded[0]; i++) {
		std::string path = std::string("benchmarks/") + offloaded[i] + ".nmc";
		std::vector<SimResult> r;
		int rc = sim_benchmark(path.c_str(), offloaded[i], 0, 0, &r);
		done[offloaded[i]] = r;
		CHECK_INT(rc, 0);                                       /* all configs verified */
		if (r.size() == 3) {
			CHECK(r[SIM_NEMO_C].verified);
			CHECK(r[SIM_NAIVE_OFFLOAD].verified);
			CHECK(r[SIM_NEMO_C].cycles < r[SIM_HOST_ONLY].cycles);   /* offloading pays */
			CHECK(r[SIM_NEMO_C].offloaded_nests >= 1);
			CHECK_INT(r[SIM_NEMO_C].coherence_violations, 0);
		}
	}

	/* the three results the project exists to show */
	for (i = 0; i < sizeof rejected / sizeof rejected[0]; i++) {
		std::string path = std::string("benchmarks/") + rejected[i] + ".nmc";
		std::vector<SimResult> r;
		int rc = sim_benchmark(path.c_str(), rejected[i], 0, 0, &r);
		CHECK_INT(rc, 0);
		if (r.size() == 3) {
			/* NAIVE_OFFLOAD is measurably slower than HOST_ONLY ... */
			CHECK(r[SIM_NAIVE_OFFLOAD].cycles > 1.10 * r[SIM_HOST_ONLY].cycles);
			/* ... and NEMO_C matches HOST_ONLY by declining to offload */
			CHECK(r[SIM_NEMO_C].cycles == r[SIM_HOST_ONLY].cycles);
			CHECK_INT(r[SIM_NEMO_C].offloaded_nests, 0);
			CHECK(r[SIM_NAIVE_OFFLOAD].offloaded_nests >= 1);
		}
	}

	/* fusion, fission and interchange really help over the naive offloader */
	{
		std::vector<SimResult> &f = done["fusion_chain"];
		CHECK(f[SIM_NEMO_C].cycles < f[SIM_NAIVE_OFFLOAD].cycles);
		CHECK(f[SIM_NEMO_C].bus_bytes < f[SIM_NAIVE_OFFLOAD].bus_bytes);
		std::vector<SimResult> &m = done["mixed_loop"];
		CHECK(m[SIM_NEMO_C].cycles < m[SIM_NAIVE_OFFLOAD].cycles);
		std::vector<SimResult> &c = done["colmajor"];
		CHECK(c[SIM_NEMO_C].cycles < c[SIM_NAIVE_OFFLOAD].cycles);
	}

	/* the optimiser preserves a program that uses every pass */
	{
		std::vector<SimResult> r;
		CHECK_INT(sim_benchmark("tests/opt_demo.nmc", "opt_demo", 0, 0, &r), 0);
		CHECK_INT(sim_benchmark("tests/coherence_demo.nmc", "coherence_demo", 0, 0, &r), 0);
		CHECK_INT(r[SIM_NEMO_C].coherence_violations, 0);

		/* strength-reduced accumulators inside an offloaded nest restart per DPU */
		CHECK_INT(sim_benchmark("tests/sr_offload.nmc", "sr_offload", 0, 0, &r), 0);
		CHECK(r[SIM_NEMO_C].offloaded_nests == 1);
	}

	/* every distribution is functionally correct; the cost model picks the cheap one */
	{
		std::vector<SimResult> r;
		g_force_dist = DIST_CYCLIC;
		CHECK_INT(sim_benchmark("benchmarks/stencil1d.nmc", "stencil1d/cyclic", 0, 0, &r), 0);
		double cyclic = r[SIM_NEMO_C].cycles;
		g_force_dist = DIST_BLOCK_CYCLIC;
		CHECK_INT(sim_benchmark("benchmarks/stencil1d.nmc", "stencil1d/bcyclic", 0, 0, &r), 0);
		double bcyclic = r[SIM_NEMO_C].cycles;
		g_force_dist = -1;
		double block = done["stencil1d"][SIM_NEMO_C].cycles;
		CHECK(block < bcyclic);
		CHECK(bcyclic < cyclic);
	}

	TEST_DONE();
}
