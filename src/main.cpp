/* ============================================================================
 * main.cpp  --  NEMO-C driver
 *
 * Phase A scope: open a .nmc file and run the front end.
 *   --dump tokens   stop after lexical analysis, print the token stream
 *   --dump ast      lex + parse, print the AST, skip semantic analysis
 *   --check         lex + parse + semantic analysis, report pass/fail only
 *   (no flag)       all of the above, then print the AST
 * Later phases hang IR lowering, the optimiser and the near-memory passes off
 * the same Program* the parser produces here.
 * ==========================================================================*/
#include <cstdio>
#include <cstring>

#include "ast.h"
#include "semantic.h"
#include "parser.tab.h"        /* token codes, YYSTYPE yylval, YYLTYPE yylloc */

/* provided by the generated scanner / parser */
extern FILE *yyin;
extern int   yylex(void);
extern int   yyparse(void);
extern int   g_error_count;
extern const char *g_srcfile;
extern Program    *g_program;

static void usage(const char *prog)
{
	fprintf(stderr,
	        "usage: %s <file.nmc> [--dump tokens|ast | --check]\n"
	        "  runs the NEMO-C front end on a source file\n", prog);
}

/* --------------------------------------------------------------------------
 * spelling of a token code, for --dump tokens.  Returns 0 for a single-
 * character token (the caller prints the character itself).
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

/* --------------------------------------------------------------------------
 * --dump tokens : call the scanner directly until end of file.
 * ------------------------------------------------------------------------*/
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

int main(int argc, char **argv)
{
	const char *path = 0;
	const char *mode = "full";        /* full | tokens | ast | check */
	int i;

	for (i = 1; i < argc; i++) {
		if (strcmp(argv[i], "--dump") == 0 && i + 1 < argc) {
			mode = argv[++i];
		} else if (strcmp(argv[i], "--check") == 0) {
			mode = "check";
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
	if (strcmp(mode, "full") != 0 && strcmp(mode, "tokens") != 0 &&
	    strcmp(mode, "ast") != 0 && strcmp(mode, "check") != 0) {
		fprintf(stderr, "%s: --dump expects 'tokens' or 'ast'\n", argv[0]);
		return 2;
	}

	yyin = fopen(path, "r");
	if (!yyin) {
		fprintf(stderr, "%s: cannot open '%s'\n", argv[0], path);
		return 2;
	}
	g_srcfile = path;

	if (strcmp(mode, "tokens") == 0) {
		printf("nemoc: lexical analysis of %s\n\n", path);
		int rc = dump_tokens();
		fclose(yyin);
		if (rc)
			fprintf(stderr, "\nnemoc: %d lexical error%s in %s.\n",
			        g_error_count, g_error_count == 1 ? "" : "s", path);
		return rc;
	}

	yyparse();
	fclose(yyin);

	if (g_error_count > 0) {
		fprintf(stderr, "\nnemoc: %d syntax error%s in %s; no AST produced.\n",
		        g_error_count, g_error_count == 1 ? "" : "s", path);
		return 1;
	}

	/* --dump ast: syntax view only, semantic analysis skipped */
	if (strcmp(mode, "ast") == 0) {
		printf("nemoc: parsed %s successfully.\n\n", path);
		ast_print_program(g_program);
		return 0;
	}

	/* --check and default: run semantic analysis */
	int se = semantic_check(g_program);
	if (se > 0) {
		fprintf(stderr, "\nnemoc: %d semantic error%s in %s.\n",
		        se, se == 1 ? "" : "s", path);
		return 1;
	}

	printf("nemoc: %s passed lexing, parsing and semantic analysis.\n", path);
	if (strcmp(mode, "check") != 0) {
		printf("\n");
		ast_print_program(g_program);
	}
	return 0;
}
