/* ============================================================================
 * main.cpp  --  NEMO-C driver
 *
 * Phase A scope: open a .nmc file, run the flex scanner + bison LALR parser,
 * and on success dump the AST.  Later phases hang symbol-table construction,
 * semantic analysis, IR lowering, the optimiser and the near-memory passes off
 * the same Program* that the parser produces here.
 * ==========================================================================*/
#include <cstdio>
#include <cstring>

#include "ast.h"

/* provided by the generated scanner / parser */
extern FILE *yyin;
extern int   yyparse(void);
extern int   g_error_count;
extern const char *g_srcfile;
extern Program    *g_program;

static void usage(const char *prog)
{
	fprintf(stderr,
	        "usage: %s <file.nmc> [--dump ast]\n"
	        "  parses a NEMO-C source file and prints its AST\n", prog);
}

int main(int argc, char **argv)
{
	const char *path = 0;
	int dump_ast = 1;                 /* Phase A: AST dump on by default */
	int i;

	for (i = 1; i < argc; i++) {
		if (strcmp(argv[i], "--dump") == 0 && i + 1 < argc) {
			i++;                     /* only "ast" is understood for now */
		} else if (strcmp(argv[i], "-h") == 0 ||
		           strcmp(argv[i], "--help") == 0) {
			usage(argv[0]);
			return 0;
		} else if (argv[i][0] == '-') {
			fprintf(stderr, "%s: unknown option '%s'\n", argv[0], argv[i]);
			return 2;
		} else {
			path = argv[i];
		}
	}

	if (!path) {
		usage(argv[0]);
		return 2;
	}

	yyin = fopen(path, "r");
	if (!yyin) {
		fprintf(stderr, "%s: cannot open '%s'\n", argv[0], path);
		return 2;
	}
	g_srcfile = path;

	yyparse();
	fclose(yyin);

	if (g_error_count > 0) {
		fprintf(stderr, "\nnemoc: %d syntax error%s in %s; no AST produced.\n",
		        g_error_count, g_error_count == 1 ? "" : "s", path);
		return 1;
	}

	printf("nemoc: parsed %s successfully.\n\n", path);
	if (dump_ast)
		ast_print_program(g_program);
	return 0;
}
