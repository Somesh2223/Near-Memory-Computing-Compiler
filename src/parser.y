/* ============================================================================
 * parser.y  --  NEMO-C syntax analyser (bison, LALR(1))
 *
 * Compiler-theory concept: bottom-up shift/reduce parsing with an LALR(1)
 * table.  Bison builds the canonical LR(0) automaton, merges states with
 * identical cores to get the LALR(1) table, and drives it with one token of
 * lookahead (Aho/Lam/Sethi/Ullman 2nd ed., sections 4.6-4.7).  Each reduction
 * runs a syntax-directed action that builds one AST node (ast.cpp).
 *
 * ---------------------------------------------------------------------------
 * GRAMMAR CONFLICTS AND THEIR RESOLUTIONS
 * ---------------------------------------------------------------------------
 * 0. This grammar has ZERO shift/reduce and ZERO reduce/reduce conflicts
 *    (`bison` reports none; `%expect 0` makes any future conflict fail the
 *    build).  The two places a hand-written imperative grammar normally
 *    conflicts are handled as follows.
 *
 * 1. "Dangling else" -- NOT a conflict here, on purpose.  The classic
 *    ambiguity needs an unbraced then-branch: `if a then if b then s else t`.
 *    NEMO-C forces braces on both arms:
 *        IF '(' expr ')' '{' stmt_list '}'
 *      | IF '(' expr ')' '{' stmt_list '}' ELSE '{' stmt_list '}'
 *    After `IF ( expr ) { stmt_list }` the inner block is already reduced and
 *    the lookahead that could follow the else-less reduction is always '}'
 *    (the brace of the enclosing block), never ELSE -- ELSE only appears
 *    *after* that '}'.  So ELSE is not in the reduce lookahead set and there
 *    is nothing to resolve.  Mandatory braces buy LALR(1)-cleanliness, not
 *    just unambiguity.  (Contrast ALSU section 4.8.2, the dangling-else
 *    grammar, which needs an explicit shift preference.)
 *
 * 2. Arithmetic / relational operator precedence in `expr`.  Left-recursive
 *    `expr : expr OP expr` is ambiguous on its own.  Resolved explicitly by the
 *    %left / %nonassoc precedence declarations below (classic operator-
 *    precedence disambiguation, ALSU section 4.8.1), not by rewriting the
 *    grammar into layered non-terminals -- the flat rules keep the AST-building
 *    actions readable.  Relationals are %nonassoc: `a < b < c` is a syntax
 *    error by design.  Unary minus uses the %prec UMINUS pseudo-token.
 *
 * 3. Error productions.  `stmt : error ';'` lets the parser discard the rest of
 *    a broken statement and resynchronise at the next ';' instead of dying on
 *    the first mistake.  bison's special `error` token does not add to the
 *    conflict count.
 *
 * ---------------------------------------------------------------------------
 * NOTE ON THE kernel/host STATEMENT DISTINCTION
 * ---------------------------------------------------------------------------
 * The course grammar splits statements into `stmt` (for/assign/if) and `hstmt`
 * (stmt + call + print), i.e. `call` and `print` are meant to be host-only.
 * That distinction is context-sensitive once you nest it inside `if`/`for`
 * bodies (the spec's own example puts `print` inside a host `if`, which the
 * written `ifstmt := ... "{" stmt* "}"` rule cannot derive).  A context-free
 * grammar cannot express "call/print only inside a host block", so this parser
 * accepts the superset -- one `stmt` non-terminal that includes call/print --
 * and semantic.cpp (Phase A, next step) rejects `call`/`print` that appear in
 * kernel scope.  This is the standard split: CFG accepts a superset, the
 * semantic pass enforces the context-sensitive rule.
 * ==========================================================================*/

%locations
%define parse.error verbose
%expect 0

%code requires {
	#include "ast.h"
}

%{
#include <cstdio>
#include <cstdlib>
#include <vector>
#include <string>

#include "ast.h"

extern int  yylex(void);
void        yyerror(const char *s);

/* front-end globals, defined here, referenced by main.cpp */
int         g_error_count = 0;
const char *g_srcfile     = "<stdin>";
Program    *g_program     = 0;

static Loc make_loc(int ln, int cl) { Loc l; l.line = ln; l.col = cl; return l; }
#define AT(y)  make_loc((y).first_line, (y).first_column)
%}

%union {
	long                ival;
	double              dval;
	char               *sval;
	int                 itype;   /* TypeTag as int */
	int                 op;      /* OpKind  as int */
	Expr               *expr;
	Stmt               *stmt;
	Decl               *decl;
	Item               *item;
	std::vector<Expr*> *elist;
	std::vector<Stmt*> *slist;
	std::vector<int>   *dlist;
}

%token <ival> INT_LIT    "integer literal"
%token <dval> FLOAT_LIT  "float literal"
%token <sval> IDENT      "identifier"

%token ARRAY  "array"   SCALAR "scalar"
%token INT    "int"     FLOAT  "float"
%token KERNEL "kernel"  HOST   "host"
%token FOR    "for"     IF     "if"      ELSE "else"
%token CALL   "call"    PRINT  "print"
%token PLUSEQ "+="      LE     "<="      GE   ">="
%token EQ     "=="      NE     "!="

%type <itype> type
%type <decl>  decl
%type <item>  toplevel kernel hostblock
%type <stmt>  stmt assign forstmt ifstmt callstmt printstmt
%type <expr>  expr lvalue
%type <elist> exprlist
%type <slist> stmt_list
%type <dlist> intlist
%type <op>    relop

/* precedence: lowest line first, highest last (ALSU fig. 4.8.1 style) */
%left  EQ NE
%nonassoc '<' '>' LE GE
%left  '+' '-'
%left  '*' '/' '%'
%right UMINUS

%start program
%%

/* ======================= top level ================================== */
program
	: /* empty */             { g_program = new Program(); }
	| program toplevel        { g_program->items.push_back($2); }
	;

toplevel
	: decl                    { $$ = mk_item_decl($1); }
	| kernel                  { $$ = $1; }
	| hostblock               { $$ = $1; }
	;

/* ======================= declarations =============================== */
decl
	: ARRAY type IDENT '[' intlist ']' ';'
	    { $$ = mk_array($2, $3, $5, AT(@$)); free($3); }
	| SCALAR type IDENT ';'
	    { $$ = mk_scalar($2, $3, AT(@$)); free($3); }
	;

intlist
	: INT_LIT                 { $$ = new std::vector<int>(); $$->push_back((int)$1); }
	| intlist ',' INT_LIT     { $1->push_back((int)$3); $$ = $1; }
	;

type
	: INT                     { $$ = TY_INT; }
	| FLOAT                   { $$ = TY_FLOAT; }
	;

/* ======================= kernels / host blocks ===================== */
kernel
	: KERNEL IDENT '(' ')' '{' stmt_list '}'
	    { $$ = mk_item_kernel($2, $6, AT(@$)); free($2); }
	;

hostblock
	: HOST IDENT '(' ')' '{' stmt_list '}'
	    { $$ = mk_item_host($2, $6, AT(@$)); free($2); }
	;

/* ======================= statements ================================ */
stmt_list
	: /* empty */             { $$ = new std::vector<Stmt*>(); }
	| stmt_list stmt          { if ($2) $1->push_back($2); $$ = $1; }
	;

stmt
	: assign                  { $$ = $1; }
	| forstmt                 { $$ = $1; }
	| ifstmt                  { $$ = $1; }
	| callstmt                { $$ = $1; }
	| printstmt               { $$ = $1; }
	| error ';'               { $$ = 0; yyerrok; }   /* resync at ';' */
	;

assign
	: lvalue '=' expr ';'     { $$ = mk_assign($1, $3, AT(@$)); }
	;

lvalue
	: IDENT                   { $$ = mk_ident($1, AT(@$)); free($1); }
	| IDENT '[' exprlist ']'  { $$ = mk_index($1, $3, AT(@$)); free($1); }
	;

/* for (i = lo; i <rel> hi; i += step) { body }
 * The three index-variable spellings are kept separately in the AST; semantic
 * analysis verifies they are the same name and that the body never reassigns
 * it, and that lo/hi are constant. */
forstmt
	: FOR '(' IDENT '=' expr ';' IDENT relop expr ';' IDENT PLUSEQ INT_LIT ')' '{' stmt_list '}'
	    {
	        $$ = mk_for($3, $5, $7, $8, $9, $11, $13, $16, AT(@$));
	        free($3); free($7); free($11);
	    }
	;

relop
	: '<'                     { $$ = OP_LT; }
	| LE                      { $$ = OP_LE; }
	| '>'                     { $$ = OP_GT; }
	| GE                      { $$ = OP_GE; }
	| EQ                      { $$ = OP_EQ; }
	| NE                      { $$ = OP_NE; }
	;

ifstmt
	: IF '(' expr ')' '{' stmt_list '}'
	    { $$ = mk_if($3, $6, new std::vector<Stmt*>(), false, AT(@$)); }
	| IF '(' expr ')' '{' stmt_list '}' ELSE '{' stmt_list '}'
	    { $$ = mk_if($3, $6, $10, true, AT(@$)); }
	;

callstmt
	: CALL IDENT '(' ')' ';'  { $$ = mk_call($2, AT(@$)); free($2); }
	;

printstmt
	: PRINT lvalue ';'        { $$ = mk_print($2, AT(@$)); }
	;

/* ======================= expressions ============================== */
exprlist
	: expr                    { $$ = new std::vector<Expr*>(); $$->push_back($1); }
	| exprlist ',' expr       { $1->push_back($3); $$ = $1; }
	;

expr
	: expr '+' expr           { $$ = mk_binop(OP_ADD, $1, $3, AT(@$)); }
	| expr '-' expr           { $$ = mk_binop(OP_SUB, $1, $3, AT(@$)); }
	| expr '*' expr           { $$ = mk_binop(OP_MUL, $1, $3, AT(@$)); }
	| expr '/' expr           { $$ = mk_binop(OP_DIV, $1, $3, AT(@$)); }
	| expr '%' expr           { $$ = mk_binop(OP_MOD, $1, $3, AT(@$)); }
	| expr '<' expr           { $$ = mk_binop(OP_LT, $1, $3, AT(@$)); }
	| expr LE  expr           { $$ = mk_binop(OP_LE, $1, $3, AT(@$)); }
	| expr '>' expr           { $$ = mk_binop(OP_GT, $1, $3, AT(@$)); }
	| expr GE  expr           { $$ = mk_binop(OP_GE, $1, $3, AT(@$)); }
	| expr EQ  expr           { $$ = mk_binop(OP_EQ, $1, $3, AT(@$)); }
	| expr NE  expr           { $$ = mk_binop(OP_NE, $1, $3, AT(@$)); }
	| '-' expr %prec UMINUS   { $$ = mk_unop(OP_NEG, $2, AT(@$)); }
	| '(' expr ')'            { $$ = $2; }
	| IDENT '[' exprlist ']'  { $$ = mk_index($1, $3, AT(@$)); free($1); }
	| IDENT                   { $$ = mk_ident($1, AT(@$)); free($1); }
	| INT_LIT                 { $$ = mk_int($1, AT(@$)); }
	| FLOAT_LIT               { $$ = mk_float($1, AT(@$)); }
	;

%%

/* --------------------------------------------------------------------------
 * diagnostics.  bison calls yyerror on a syntax error (message already
 * formatted by `parse.error verbose`); we prefix it with file:line:col taken
 * from the location of the offending lookahead token.
 * ------------------------------------------------------------------------*/
void yyerror(const char *s)
{
	g_error_count++;
	fprintf(stderr, "%s:%d:%d: error: %s\n",
	        g_srcfile, yylloc.first_line, yylloc.first_column, s);
}
