/* ============================================================================
 * sim.h  --  the machine simulator (Phase I)
 * ==========================================================================*/
#ifndef NEMOC_SIM_H
#define NEMOC_SIM_H

#include <cstdio>
#include <string>
#include <vector>

enum SimConfig {
	SIM_HOST_ONLY,
	SIM_NAIVE_OFFLOAD,
	SIM_NEMO_C,
	SIM_NEMO_ASM          /* NEMO_C, but offloaded nests run as generated DPU code */
};

const char *sim_config_name(int c);

struct SimResult {
	std::string benchmark;
	std::string config;
	double      cycles;           /* whole program                              */
	double      kernel_cycles;    /* inside `call`ed kernels only               */
	double      bus_bytes;        /* bytes crossing the memory bus              */
	double      energy_pj;
	int         offloaded_nests;  /* nests that ran on PIM                      */
	int         launches;
	bool        verified;         /* bit-identical to the HOST_ONLY execution   */
	std::string verify_note;
	int         coherence_violations;
	std::vector<std::string> output;
};

/* Compile `path` for each of the three configurations, execute it, verify the
 * results are bit-identical, print a table, and append CSV rows (if csv is
 * non-null).  Returns 0 if every configuration verified, else 1. */
int sim_benchmark(const char *path, const std::string &name, FILE *csv, int verbose,
                  std::vector<SimResult> *results);

#endif /* NEMOC_SIM_H */
