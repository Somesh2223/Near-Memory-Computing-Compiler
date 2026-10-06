/* ============================================================================
 * main.cpp  --  NEMO-C driver
 *
 * Front end
 *   --dump tokens      lexical analysis only; print the token stream
 *   --dump ast         lex + parse, print the AST (no semantic pass)
 *   --check            lex + parse + semantic analysis, report pass/fail
 *   --dump ir          lower to IR and print it (unoptimised three-address code)
 * Classical optimisations (Phase C)
 *   --dump opt         run all five passes, print the optimised IR
 *   --dump constfold|cse|dce|indvar|strength
 *                      print the IR before and after one pass, with its analysis
 * Dependence / cost model (Phases D, E)
 *   --dump affine      access matrices F, c of every reference
 *   --dump deps        dependences, GCD vs Banerjee verdicts, parallelism
 *   --emit-dot cfg|deps  write Graphviz files to build/dot
 *   --explain <nest>   the complete offload-decision trace for one kernel/nest
 *   --decisions        one decision record per nest
 * Transformations, partitioning, coherence (Phases F, G)
 *   --transformations  fission / interchange / fusion log
 *   --partition        how each offloaded nest is cut across the DPUs
 *   --coherence        flush / invalidate placement with the dataflow table
 * Code generation and simulation (Phases H, I)
 *   --codegen          write build/<kernel>_host.c and build/<kernel>_dpu.pim
 *   --dump regalloc    live intervals and colouring order   (--regs K)
 *   --emit-dot interference
 *   --simulate         run HOST_ONLY, NAIVE_OFFLOAD and NEMO_C and verify them
 *   --csv <file>       append the simulation results as CSV rows
 * Options: --no-opt (skip Phase C)  --dist block|cyclic|bcyclic  --regs K
 * ==========================================================================*/
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>

#include "ast.h"
#include "semantic.h"
#include "ir.h"
#include "cfg.h"
#include "opt.h"
#include "analysis.h"
#include "plan.h"
#include "frontend.h"
#include "config.h"
#include "dpu.h"
#include "sim.h"
#include "parser.tab.h"        /* token codes, YYSTYPE yylval, YYLTYPE yylloc */

/* provided by the generated scanner / parser */
extern FILE *yyin;
extern int   yylex(void);
extern int   g_error_count;
extern const char *g_srcfile;
extern void  lexer_reset(void);

struct Options {
	const char *path;
	std::string dump;
	std::string emit_dot;
	std::string explain;
	std::string csv;
	bool check, decisions, transformations, partition, coherence, codegen, simulate;
	bool report_decisions, report_transforms;
	bool no_opt;
	int  regs;
};

static void usage(const char *prog)
{
	fprintf(stderr,
	        "usage: %s <file.nmc> [options]\n"
	        "  --dump tokens|ast|ir|opt|constfold|cse|dce|indvar|strength|affine|deps|regalloc\n"
	        "  --check  --decisions  --transformations  --partition  --coherence\n"
	        "  --explain <nest>  --emit-dot cfg|deps|interference\n"
	        "  --codegen  --simulate [--csv file]  --no-opt  --dist block|cyclic|bcyclic  --regs K\n",
	        prog);
}

/* --------------------------------------------------------------------------
 * --dump tokens
 * ------------------------------------------------------------------------*/
static const char *token_name(int t)
{
	switch (t) {
	case ARRAY:     return "ARRAY";
	case SCALAR:    return "SCALAR";
	case INT:       return "INT";
	case FLOAT:     return "FLOAT";
	case KERNEL:    return "KERNEL";
	case HOST:      return "HOST";
	case FOR:       return "FOR";
	case IF:        return "IF";
	case ELSE:      return "ELSE";
	case CALL:      return "CALL";
	case PRINT:     return "PRINT";
	case IDENT:     return "IDENT";
	case INT_LIT:   return "INT_LIT";
	case FLOAT_LIT: return "FLOAT_LIT";
	case PLUSEQ:    return "PLUSEQ";
	case LE:        return "LE";
	case GE:        return "GE";
	case EQ:        return "EQ";
	case NE:        return "NE";
	}
	return 0;
}

static int dump_tokens(void)
{
	int t;
	int n = 0;

	printf("  line:col   token        lexeme\n");
	printf("  --------   ----------   ------\n");
	while ((t = yylex()) != 0) {
		const char *nm = token_name(t);
		printf("  %4d:%-4d  ", yylloc.first_line, yylloc.first_column);
		if (nm == 0) {
			printf("'%c'\n", t);
		} else if (t == IDENT) {
			printf("%-10s   %s\n", nm, yylval.sval);
		} else if (t == INT_LIT) {
			printf("%-10s   %ld\n", nm, yylval.ival);
		} else if (t == FLOAT_LIT) {
			printf("%-10s   %g\n", nm, yylval.dval);
		} else {
			printf("%s\n", nm);
		}
		n++;
	}
	printf("  --- %d tokens ---\n", n);
	return g_error_count > 0 ? 1 : 0;
}

/* write build/dot/<name>.cfg.dot for one kernel or host block */
static void emit_cfg_dot(Kernel *k)
{
	std::string path = "build/dot/" + k->name + ".cfg.dot";
	FILE *out = fopen(path.c_str(), "w");
	if (!out) {
		fprintf(stderr, "nemoc: cannot write '%s' (did you 'mkdir -p build/dot'?)\n",
		        path.c_str());
		return;
	}
	CFG *g = build_cfg(k);
	cfg_emit_dot(g, out);
	fclose(out);
	printf("nemoc: wrote %s (%d basic blocks)\n", path.c_str(), (int)g->blocks.size());
}

/* analyse every top-level loop nest of every kernel (before any transformation) */
static void analyse_all_nests(IRProgram *p, bool print_pairs, bool dot)
{
	size_t k, i;
	for (k = 0; k < p->kernels.size(); k++) {
		Kernel *kern = p->kernels[k];
		for (i = 0; i < kern->body.size(); i++) {
			if (kern->body[i].kind != BK_LOOP)
				continue;
			NestAnalysis na;
			analyse_nest(p, kern, kern->body[i].loop, &na);
			printf("---- kernel %s ----\n", kern->name.c_str());
			dependence_print(&na, print_pairs);
			if (dot) {
				std::string path = "build/dot/" + kern->name + ".deps.dot";
				FILE *f = fopen(path.c_str(), "w");
				if (f) {
					dependence_emit_dot(&na, f);
					fclose(f);
					printf("nemoc: wrote %s\n", path.c_str());
				}
			}
		}
	}
}

static bool is_pass_name(const std::string &s)
{
	return s == "constfold" || s == "cse" || s == "dce" || s == "indvar" || s == "strength";
}

int main(int argc, char **argv)
{
	Options o;
	int i;

	o.path = 0;
	o.check = o.decisions = o.transformations = o.partition = o.coherence = false;
	o.codegen = o.simulate = o.no_opt = false;
	o.report_decisions = o.report_transforms = false;
	o.regs = 0;

	for (i = 1; i < argc; i++) {
		const char *a = argv[i];
		if (!strcmp(a, "--dump") && i + 1 < argc)            o.dump = argv[++i];
		else if (!strcmp(a, "--check"))                      o.check = true;
		else if (!strcmp(a, "--emit-dot") && i + 1 < argc)   o.emit_dot = argv[++i];
		else if (!strcmp(a, "--explain") && i + 1 < argc)    o.explain = argv[++i];
		else if (!strcmp(a, "--csv") && i + 1 < argc)        o.csv = argv[++i];
		else if (!strcmp(a, "--decisions"))                  o.decisions = true;
		else if (!strcmp(a, "--transformations"))            o.transformations = true;
		else if (!strcmp(a, "--partition"))                  o.partition = true;
		else if (!strcmp(a, "--coherence"))                  o.coherence = true;
		else if (!strcmp(a, "--codegen"))                    o.codegen = true;
		else if (!strcmp(a, "--simulate"))                   o.simulate = true;
		else if (!strcmp(a, "--report-decisions"))           o.report_decisions = true;
		else if (!strcmp(a, "--report-transforms"))          o.report_transforms = true;
		else if (!strcmp(a, "--no-opt"))                     o.no_opt = true;
		else if (!strcmp(a, "--regs") && i + 1 < argc)       o.regs = atoi(argv[++i]);
		else if (!strcmp(a, "--dist") && i + 1 < argc) {
			const char *d = argv[++i];
			g_force_dist = !strcmp(d, "block") ? DIST_BLOCK :
			               (!strcmp(d, "cyclic") ? DIST_CYCLIC : DIST_BLOCK_CYCLIC);
		} else if (!strcmp(a, "-h") || !strcmp(a, "--help")) {
			usage(argv[0]);
			return 0;
		} else if (a[0] == '-') {
			fprintf(stderr, "%s: unknown option '%s'\n", argv[0], a);
			return 2;
		} else {
			o.path = a;
		}
	}
	if (!o.path) {
		usage(argv[0]);
		return 2;
	}

	/* ---- lexical analysis only ------------------------------------------- */
	if (o.dump == "tokens") {
		yyin = fopen(o.path, "r");
		if (!yyin) {
			fprintf(stderr, "%s: cannot open '%s'\n", argv[0], o.path);
			return 2;
		}
		g_srcfile = o.path;
		g_error_count = 0;
		lexer_reset();
		printf("nemoc: lexical analysis of %s\n\n", o.path);
		int rc = dump_tokens();
		fclose(yyin);
		if (rc)
			fprintf(stderr, "\nnemoc: %d lexical error%s in %s.\n", g_error_count,
			        g_error_count == 1 ? "" : "s", o.path);
		return rc;
	}

	/* ---- parsing ---------------------------------------------------------- */
	int nerr = 0;
	Program *ast = frontend_parse(o.path, &nerr);
	if (nerr < 0) {
		fprintf(stderr, "%s: cannot open '%s'\n", argv[0], o.path);
		return 2;
	}
	if (!ast) {
		fprintf(stderr, "\nnemoc: %d syntax error%s in %s; no AST produced.\n", nerr,
		        nerr == 1 ? "" : "s", o.path);
		return 1;
	}
	if (o.dump == "ast") {
		printf("nemoc: parsed %s successfully.\n\n", o.path);
		ast_print_program(ast);
		return 0;
	}

	/* ---- semantic analysis ------------------------------------------------ */
	int se = semantic_check(ast);
	if (se > 0) {
		fprintf(stderr, "\nnemoc: %d semantic error%s in %s.\n", se, se == 1 ? "" : "s", o.path);
		return 1;
	}

	bool want_ir = o.dump.size() > 0 || o.emit_dot.size() > 0 || o.explain.size() > 0 ||
	               o.decisions || o.transformations || o.partition || o.coherence ||
	               o.codegen || o.simulate || o.report_decisions || o.report_transforms;
	if (!want_ir) {
		printf("nemoc: %s passed lexing, parsing and semantic analysis.\n", o.path);
		if (!o.check) {
			printf("\n");
			ast_print_program(ast);
		}
		return 0;
	}

	/* ---- IR -------------------------------------------------------------- */
	IRProgram *irp = lower_program(ast);
	if (o.dump == "ir") {
		printf("nemoc: %s passed lexing, parsing and semantic analysis.\n\n", o.path);
		ir_print_program(irp);
		if (o.emit_dot == "cfg") {
			size_t k;
			for (k = 0; k < irp->kernels.size(); k++) emit_cfg_dot(irp->kernels[k]);
			for (k = 0; k < irp->hosts.size(); k++)   emit_cfg_dot(irp->hosts[k]);
		}
		return 0;
	}
	if (o.emit_dot == "cfg") {
		size_t k;
		for (k = 0; k < irp->kernels.size(); k++) emit_cfg_dot(irp->kernels[k]);
		for (k = 0; k < irp->hosts.size(); k++)   emit_cfg_dot(irp->hosts[k]);
		if (o.dump.empty() && !o.decisions && !o.simulate && !o.codegen)
			return 0;
	}

	/* ---- classical optimisations ------------------------------------------- */
	if (is_pass_name(o.dump)) {
		opt_run_all(irp, o.dump);
		return 0;
	}
	if (!o.no_opt)
		opt_run_all(irp, "");
	if (o.dump == "opt") {
		printf("nemoc: optimised IR of %s\n\n", o.path);
		ir_print_program(irp);
		return 0;
	}

	/* ---- analyses --------------------------------------------------------- */
	if (o.dump == "affine" || o.dump == "deps" || o.emit_dot == "deps") {
		analyse_all_nests(irp, o.dump == "deps", o.emit_dot == "deps");
		return 0;
	}

	/* ---- simulation of the three configurations --------------------------- */
	if (o.simulate) {
		std::string name = o.path;
		size_t slash = name.find_last_of("/\\");
		if (slash != std::string::npos)
			name = name.substr(slash + 1);
		size_t dot = name.find_last_of('.');
		if (dot != std::string::npos)
			name = name.substr(0, dot);
		FILE *csv = 0;
		if (o.csv.size() > 0)
			csv = fopen(o.csv.c_str(), "a");
		int rc = sim_benchmark(o.path, name, csv, 1, 0);
		if (csv)
			fclose(csv);
		return rc;
	}

	/* ---- the cost-model-guided plan ---------------------------------------- */
	ProgramPlan *pp = plan_program(irp, true);

	if (o.report_decisions || o.report_transforms) {
		std::string bench = o.path;
		size_t sl = bench.find_last_of('/');
		if (sl != std::string::npos)
			bench = bench.substr(sl + 1);
		size_t dt = bench.find_last_of('.');
		if (dt != std::string::npos)
			bench = bench.substr(0, dt);
		size_t a, b;
		if (o.report_decisions)
			for (a = 0; a < pp->kernels.size(); a++)
				for (b = 0; b < pp->kernels[a].nests.size(); b++) {
					NestPlan *np = pp->kernels[a].nests[b];
					Decision &d = np->dec;
					std::string reuse = "none";
					if (!d.temporal_sig.empty()) {
						reuse = "";
						for (size_t q = 0; q < d.temporal_sig.size(); q++)
							reuse += (q ? "," : "") + d.temporal_sig[q];
					} else if (!d.temporal.empty()) {
						reuse = "negligible";
					}
					if (!d.group_temporal.empty())
						reuse += " (+ neighbour reuse)";
					std::string reason = d.reason;
					for (size_t q = 0; q < reason.size(); q++)
						if (reason[q] == '|')
							reason[q] = '/';
					if (d.analysable)
						printf("| %s | %s | %.2f | %s | %s | %s | %s |\n", bench.c_str(), np->name.c_str(),
						       d.ai, reuse.c_str(), class_text(d.pclass),
						       np->offload ? "OFFLOAD" : "REJECT", reason.c_str());
					else
						printf("| %s | %s | n/a | n/a | %s | %s | %s |\n", bench.c_str(), np->name.c_str(),
						       class_text(d.pclass), "REJECT", reason.c_str());
				}
		if (o.report_transforms)
			for (a = 0; a < pp->transforms.size(); a++)
				printf("- **%s** (`%s`) -- %s: %s\n  - %s\n", bench.c_str(),
				       pp->transforms[a].kernel.c_str(), pp->transforms[a].kind.c_str(),
				       pp->transforms[a].what.c_str(), pp->transforms[a].gain.c_str());
		return 0;
	}

	if (o.explain.size() > 0) {
		NestPlan *np = find_nest_plan(pp, o.explain);
		if (!np) {
			fprintf(stderr, "%s: no kernel or nest named '%s'\n", argv[0], o.explain.c_str());
			return 2;
		}
		explain_decision(irp, &np->na, &np->dec);
		if (np->dec.analysable && np->part.feasible) {
			printf("\n");
			partition_print(&np->part);
		}
		return 0;
	}
	if (o.decisions || (o.dump.empty() && !o.transformations && !o.partition && !o.coherence &&
	                    !o.codegen && !o.simulate)) {
		print_decisions(pp);
	}
	if (o.transformations) {
		print_transformations(pp);
	}
	if (o.partition) {
		size_t a, b;
		for (a = 0; a < pp->kernels.size(); a++)
			for (b = 0; b < pp->kernels[a].nests.size(); b++) {
				NestPlan *np = pp->kernels[a].nests[b];
				printf("%s\n", np->name.c_str());
				if (np->offload)
					partition_print(&np->part);
				else
					printf("  runs on the host (%s)\n", np->dec.reason.c_str());
			}
	}
	if (o.coherence) {
		coherence_run(pp, 1);
		printf("\nIR with the coherence operations inserted:\n");
		ir_print_program(irp);
	}

	/* ---- code generation, register allocation ------------------------------- */
	if (o.codegen || o.dump == "regalloc" || o.emit_dot == "interference") {
		if (!o.coherence)
			coherence_run(pp, 0);
		int K = o.regs > 0 ? o.regs : NUM_REGS;
		std::vector<DProgram*> progs;
		size_t a, b;
		for (a = 0; a < pp->kernels.size(); a++) {
			KernelPlan &kp = pp->kernels[a];
			bool any = false;
			for (b = 0; b < kp.nests.size(); b++) {
				NestPlan *np = kp.nests[b];
				if (!np->offload)
					continue;
				any = true;
				DProgram *dp = new DProgram();
				std::string err;
				if (!dpu_codegen(irp, np, dp, &err)) {
					fprintf(stderr, "nemoc: DPU codegen failed for %s: %s\n", np->name.c_str(), err.c_str());
					return 1;
				}
				RAResult ra;
				regalloc_run(dp, K, &ra, o.dump == "regalloc");
				if (o.dump == "regalloc")
					regalloc_print(dp, &ra);
				std::string base = np->name;
				for (size_t q = 0; q < base.size(); q++)
					if (base[q] == '#')
						base[q] = '_';
				if (o.emit_dot == "interference") {
					std::string dpath = "build/dot/" + base + ".interference.dot";
					FILE *f = fopen(dpath.c_str(), "w");
					if (f) {
						regalloc_emit_dot(dp, &ra, f);
						fclose(f);
						printf("nemoc: wrote %s\n", dpath.c_str());
					}
				}
				if (o.codegen) {
					std::string path = "build/" + base + "_dpu.pim";
					FILE *f = fopen(path.c_str(), "w");
					if (!f) {
						fprintf(stderr, "nemoc: cannot write %s (does build/ exist?)\n", path.c_str());
						return 1;
					}
					fprintf(f, "; registers: K = %d, %d used, %d spilled, %d round(s) of colouring%s\n", K,
					        ra.registers_used, ra.spilled, ra.rounds,
					        ra.verified ? ", colouring verified" : ", COLOURING NOT VERIFIED");
					dpu_print(dp, f);
					fclose(f);
					printf("nemoc: wrote %s  (%d instructions, tile %ld iterations)\n", path.c_str(),
					       (int)dp->code.size(), dp->tile_rows);
				}
				progs.push_back(dp);
			}
			if (any && o.codegen) {
				std::string text = host_codegen(pp, &kp, progs);
				std::string path = "build/" + kp.kernel->name + "_host.c";
				FILE *f = fopen(path.c_str(), "w");
				if (f) {
					fputs(text.c_str(), f);
					fclose(f);
					printf("nemoc: wrote %s\n", path.c_str());
				}
			}
		}
		if (progs.empty())
			printf("nemoc: no nest is offloaded; nothing to generate\n");
	}
	return 0;
}
