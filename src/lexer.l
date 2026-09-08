/* ============================================================================
 * lexer.l  --  NEMO-C lexical analyser (flex)
 *
 * Compiler-theory concept: lexical analysis.  A set of regular expressions is
 * compiled by flex into a deterministic finite automaton that runs the
 * maximal-munch (longest-match) rule, with rule order breaking ties so that
 * keywords win over the identifier pattern (Aho/Lam/Sethi/Ullman 2nd ed.,
 * chapter 3, "Lexical Analysis").  Each accepted lexeme is returned to the
 * bison parser as a (token, semantic-value) pair; literals also carry their
 * value, identifiers carry a heap copy of their spelling.
 *
 * Line/column tracking: YY_USER_ACTION runs before every rule action and walks
 * the matched text one character at a time, so the location handed to the
 * parser (yylloc) is exact even for tab-indented source and multi-character
 * operators.  These positions drive every diagnostic in the front end.
 * ==========================================================================*/
%option noyywrap nodefault noinput nounput

%{
#include <cstdio>
#include <cstdlib>
#include <cstring>

#include "ast.h"
#include "parser.tab.h"

/* current scan position (1-based), maintained by YY_USER_ACTION */
static int g_line = 1;
static int g_col  = 1;

/* strdup without depending on a POSIX feature-test macro */
static char *dup_lexeme(const char *s)
{
	size_t n = strlen(s) + 1;
	char  *p = (char *)malloc(n);
	memcpy(p, s, n);
	return p;
}

/* fill yylloc for the lexeme just matched, then advance (g_line, g_col) */
#define YY_USER_ACTION                                                   \
	{                                                                   \
		yylloc.first_line   = g_line;                                    \
		yylloc.first_column = g_col;                                     \
		for (int _i = 0; _i < yyleng; _i++) {                            \
			if (yytext[_i] == '\n') { g_line++; g_col = 1; }             \
			else                    { g_col++; }                        \
		}                                                               \
		yylloc.last_line   = g_line;                                     \
		yylloc.last_column = g_col;                                      \
	}

void yyerror(const char *s);            /* defined in parser.y */
%}

DIGIT   [0-9]
IDSTART [A-Za-z_]
IDCONT  [A-Za-z_0-9]

%%

[ \t\r]+            { /* horizontal whitespace: ignored */ }
\n                 { /* newline: position handled in YY_USER_ACTION */ }
"//"[^\n]*         { /* single-line comment: treated as whitespace.  Comments
                        are not part of the source grammar -- they never reach
                        the parser -- so accepting them is not a language
                        extension. */ }
"/*"([^*]|"*"+[^*/])*"*"+"/"  { /* block comment (non-nested), also whitespace.
                        The classic C comment regex; YY_USER_ACTION keeps the
                        line count correct across the newlines it spans. */ }

"array"            { return ARRAY;  }
"scalar"           { return SCALAR; }
"int"              { return INT;    }
"float"            { return FLOAT;  }
"kernel"           { return KERNEL; }
"host"             { return HOST;   }
"for"              { return FOR;    }
"if"               { return IF;     }
"else"             { return ELSE;   }
"call"             { return CALL;   }
"print"            { return PRINT;  }

{DIGIT}+"."{DIGIT}*  { yylval.dval = atof(yytext); return FLOAT_LIT; }
"."{DIGIT}+         { yylval.dval = atof(yytext); return FLOAT_LIT; }
{DIGIT}+            { yylval.ival = strtol(yytext, 0, 10); return INT_LIT; }

{IDSTART}{IDCONT}*  { yylval.sval = dup_lexeme(yytext); return IDENT; }

"+="               { return PLUSEQ; }
"<="               { return LE; }
">="               { return GE; }
"=="               { return EQ; }
"!="               { return NE; }

"+"                { return '+'; }
"-"                { return '-'; }
"*"                { return '*'; }
"/"                { return '/'; }
"%"                { return '%'; }
"="                { return '='; }
"<"                { return '<'; }
">"                { return '>'; }
"("                { return '('; }
")"                { return ')'; }
"{"                { return '{'; }
"}"                { return '}'; }
"["                { return '['; }
"]"                { return ']'; }
";"                { return ';'; }
","                { return ','; }

.                  {
	char msg[64];
	snprintf(msg, sizeof msg, "stray '%c' (0x%02x) in program text",
	         yytext[0], (unsigned char)yytext[0]);
	yyerror(msg);
}

%%
