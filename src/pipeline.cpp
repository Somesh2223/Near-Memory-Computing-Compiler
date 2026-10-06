/* ============================================================================
 * pipeline.cpp  --  ordering of the classical optimisation passes
 *
 * constfold -> cse -> dce -> indvar -> strength -> dce
 *
 * constfold first: propagating constants exposes equal operands for CSE and
 * makes more code dead.  cse leaves copies behind; dce sweeps them.  The
 * induction-variable analysis runs on the cleaned IR and feeds strength
 * reduction; a final dce removes whatever strength reduction orphaned.
 *
 * `dump` names one pass.  For that pass the IR is printed before and after,
 * with the pass's own analysis output in between -- this is the course's
 * "--dump <pass>" requirement ("each as its own file with a --dump flag").
 * ==========================================================================*/
#include <cstdio>
#include <string>

#include "ir.h"
#include "opt.h"

static int run_pass(IRProgram *p, const std::string &name, const std::string &dump,
                    int *dumped)
{
	int show = (!dump.empty() && dump == name && !*dumped);
	int changes = 0;

	if (show) {
		printf("\n==== %s: IR BEFORE ====\n", name.c_str());
		ir_print_program(p);
		printf("\n==== %s: analysis ====\n", name.c_str());
	}

	if (name == "constfold")      changes = opt_constfold(p, show);
	else if (name == "cse")       changes = opt_cse(p, show);
	else if (name == "dce")       changes = opt_dce(p, show);
	else if (name == "indvar")    changes = opt_indvar(p, show);
	else if (name == "strength")  changes = opt_strength(p, show);

	if (show) {
		printf("\n==== %s: IR AFTER  (%d change%s) ====\n", name.c_str(), changes,
		       changes == 1 ? "" : "s");
		ir_print_program(p);
		*dumped = 1;
	}
	return changes;
}

int opt_run_all(IRProgram *p, const std::string &dump)
{
	int total = 0;
	int dumped = 0;

	total += run_pass(p, "constfold", dump, &dumped);
	total += run_pass(p, "cse",       dump, &dumped);
	total += run_pass(p, "dce",       dump, &dumped);
	total += run_pass(p, "indvar",    dump, &dumped);
	total += run_pass(p, "strength",  dump, &dumped);
	total += run_pass(p, "dce",       dump, &dumped);
	return total;
}
