/* Lexer: token streams for known inputs, positions, comments, and errors. */
#include "testutil.h"
#include <vector>
#include "ast.h"
#include "parser.tab.h"

extern FILE *yyin;
extern int   yylex(void);
extern void  yyrestart(FILE *f);
extern void  lexer_reset(void);
extern int   g_error_count;
extern const char *g_srcfile;

struct Tok {
	int  code;
	int  line;
	int  col;
};

static std::vector<Tok> lex_text(const char *text)
{
	std::vector<Tok> out;
	FILE *f = fopen("build/_lex_test.nmc", "w");
	fputs(text, f);
	fclose(f);
	f = fopen("build/_lex_test.nmc", "r");
	g_error_count = 0;
	g_srcfile = "build/_lex_test.nmc";
	lexer_reset();
	yyrestart(f);
	int t;
	while ((t = yylex()) != 0) {
		Tok k;
		k.code = t;
		k.line = yylloc.first_line;
		k.col = yylloc.first_column;
		out.push_back(k);
	}
	fclose(f);
	return out;
}

int main(void)
{
	std::vector<Tok> t;

	/* keywords win over identifiers; operators use maximal munch */
	t = lex_text("kernel k() { for (i = 0; i < 8; i += 1) { A[i] = i <= 3; } }");
	int expect[] = {KERNEL, IDENT, '(', ')', '{', FOR, '(', IDENT, '=', INT_LIT, ';', IDENT, '<',
	                INT_LIT, ';', IDENT, PLUSEQ, INT_LIT, ')', '{', IDENT, '[', IDENT, ']', '=',
	                IDENT, LE, INT_LIT, ';', '}', '}'};
	CHECK_INT(t.size(), sizeof expect / sizeof expect[0]);
	for (size_t i = 0; i < t.size() && i < sizeof expect / sizeof expect[0]; i++)
		CHECK_INT(t[i].code, expect[i]);

	/* an identifier that merely starts like a keyword is an identifier */
	t = lex_text("format for_x iffy");
	CHECK_INT(t.size(), 3);
	CHECK_INT(t[0].code, IDENT);
	CHECK_INT(t[1].code, IDENT);
	CHECK_INT(t[2].code, IDENT);

	/* line and column positions, across a newline and a tab */
	t = lex_text("array\n\tfloat x;");
	CHECK_INT(t[0].line, 1);
	CHECK_INT(t[0].col, 1);
	CHECK_INT(t[1].line, 2);
	CHECK_INT(t[1].col, 2);
	CHECK_INT(t[2].col, 8);

	/* numbers */
	t = lex_text("12 3.5 .25 7.");
	CHECK_INT(t.size(), 4);
	CHECK_INT(t[0].code, INT_LIT);
	CHECK_INT(t[1].code, FLOAT_LIT);
	CHECK_INT(t[2].code, FLOAT_LIT);
	CHECK_INT(t[3].code, FLOAT_LIT);

	/* comments are whitespace; positions after them stay correct */
	t = lex_text("a // line comment\n/* block\n comment */ b");
	CHECK_INT(t.size(), 2);
	CHECK_INT(t[1].line, 3);

	/* all comparison operators */
	t = lex_text("< <= > >= == != =");
	int ops[] = {'<', LE, '>', GE, EQ, NE, '='};
	CHECK_INT(t.size(), 7);
	for (size_t i = 0; i < t.size() && i < 7; i++)
		CHECK_INT(t[i].code, ops[i]);

	/* stray characters are reported, and scanning continues */
	t = lex_text("a @ b $ c");
	CHECK_INT(g_error_count, 2);
	CHECK_INT(t.size(), 3);

	TEST_DONE();
}
