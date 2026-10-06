/* ============================================================================
 * frontend.cpp  --  glue between the generated scanner/parser and the passes
 *
 * The scanner and parser communicate through globals (yyin, g_error_count,
 * g_program, ...).  This file is the only place that manipulates them, so the
 * rest of the compiler -- and every unit test -- can say "parse this file".
 * ==========================================================================*/
#include <cstdio>
#include <cstdlib>
#include <string>

#include "frontend.h"
#include "semantic.h"
#include "opt.h"

extern FILE       *yyin;
extern int         yyparse(void);
extern void        yyrestart(FILE *f);
extern void        lexer_reset(void);
extern int         g_error_count;
extern const char *g_srcfile;
extern Program    *g_program;

Program *frontend_parse(const char *path, int *nerrors)
{
	FILE *f = fopen(path, "r");
	if (!f) {
		*nerrors = -1;
		return 0;
	}
	/* keep our own copy: callers pass temporaries, and later diagnostics (the
	 * semantic pass) still need the file name */
	static std::string stored_path;
	stored_path = path;
	g_error_count = 0;
	g_srcfile = stored_path.c_str();
	g_program = 0;
	lexer_reset();
	yyrestart(f);
	yyparse();
	fclose(f);
	*nerrors = g_error_count;
	if (g_error_count > 0)
		return 0;
	return g_program;
}

IRProgram *frontend_compile(const char *path, int optimise)
{
	int ne = 0;
	Program *ast = frontend_parse(path, &ne);
	if (!ast)
		return 0;
	if (semantic_check(ast) > 0)
		return 0;
	IRProgram *p = lower_program(ast);
	if (optimise)
		opt_run_all(p, "");
	return p;
}

/* in-memory variants write the text to a scratch file next to the binary */
static std::string write_temp(const std::string &text)
{
	static int counter = 0;
	char name[64];
	snprintf(name, sizeof name, "build/_frontend_tmp_%d.nmc", counter++);
	FILE *f = fopen(name, "w");
	if (f) {
		fputs(text.c_str(), f);
		fclose(f);
	}
	return std::string(name);
}

Program *frontend_parse_text(const std::string &text, int *nerrors)
{
	std::string path = write_temp(text);
	return frontend_parse(path.c_str(), nerrors);
}

IRProgram *frontend_compile_text(const std::string &text, int optimise)
{
	std::string path = write_temp(text);
	return frontend_compile(path.c_str(), optimise);
}
