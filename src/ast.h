/* ============================================================================
 * ast.h  --  Abstract Syntax Tree for the NEMO-C source language
 *
 * Compiler-theory concept: the AST is the parser's output, a tree that mirrors
 * the derivation of the input under the context-free grammar with the
 * operator-precedence disambiguation already applied (Aho, Lam, Sethi, Ullman,
 * "Compilers: Principles, Techniques, and Tools", 2nd ed., section 2.5 and
 * section 5.3 -- syntax-directed construction of syntax trees).
 *
 * Representation choice: every node category (Expr, Stmt, Decl, Item) is a
 * single *tagged struct* -- a `Kind` enum plus the superset of fields that any
 * variant needs.  This is a discriminated union.  A real C `union` cannot hold
 * std::string / std::vector (they have non-trivial constructors), so the fields
 * sit side by side and the `kind` tag says which ones are live.  This is the
 * representation a hand-written C compiler uses; it has no virtual dispatch and
 * every variant is visible in one place, which is what makes it defensible in a
 * viva.  No inheritance, no visitor pattern, no smart pointers -- nodes are
 * allocated with `new` and never freed (the compiler is a short-lived process).
 * ==========================================================================*/
#ifndef NEMOC_AST_H
#define NEMOC_AST_H

#include <vector>
#include <string>

/* ---- source position, threaded through for diagnostics ------------------ */
struct Loc {
	int line;
	int col;
};

/* ---- scalar/element types --------------------------------------------- */
enum TypeTag {
	TY_INT,
	TY_FLOAT
};

/* ---- operators (binary + the one unary) ------------------------------- */
enum OpKind {
	OP_ADD, OP_SUB, OP_MUL, OP_DIV, OP_MOD,
	OP_LT, OP_LE, OP_GT, OP_GE, OP_EQ, OP_NE,
	OP_NEG                       /* unary minus */
};

/* ======================= expressions ================================== */
enum ExprKind {
	EK_INT,      /* integer literal            -> ival                     */
	EK_FLOAT,    /* float literal              -> dval                     */
	EK_IDENT,    /* scalar / loop-var name     -> name                    */
	EK_INDEX,    /* array reference name[i,..] -> name, index[]           */
	EK_BINOP,    /* lhs <op> rhs               -> op, lhs, rhs            */
	EK_UNOP      /* <op> lhs                   -> op, lhs                 */
};

struct Expr {
	ExprKind kind;
	Loc      loc;

	long              ival;     /* EK_INT                                  */
	double            dval;     /* EK_FLOAT                                */
	std::string       name;     /* EK_IDENT, EK_INDEX                      */
	std::vector<Expr*> index;   /* EK_INDEX : one Expr per subscript       */
	OpKind            op;       /* EK_BINOP, EK_UNOP                       */
	Expr             *lhs;      /* EK_BINOP, EK_UNOP                       */
	Expr             *rhs;      /* EK_BINOP only                           */
};

/* ======================= statements ================================== */
enum StmtKind {
	SK_ASSIGN,   /* target = value ;                                       */
	SK_FOR,      /* for (ivar = lo; ivar <rel> hi; ivar += step) { body }  */
	SK_IF,       /* if (cond) { then_body } [else { else_body }]           */
	SK_CALL,     /* call callee () ;                                       */
	SK_PRINT     /* print arg ;                                            */
};

struct Stmt {
	StmtKind kind;
	Loc      loc;

	/* SK_ASSIGN */
	Expr *target;               /* EK_IDENT or EK_INDEX                    */
	Expr *value;

	/* SK_FOR -- the three header slots each carry their own identifier so
	 * semantic analysis can later verify they name the same variable and
	 * that the body never reassigns it.                                   */
	std::string ivar;           /* name in the "ivar = lo" slot           */
	std::string ivar_test;      /* name in the "ivar <rel> hi" slot       */
	std::string ivar_incr;      /* name in the "ivar += step" slot        */
	Expr   *lo;
	OpKind  rel;
	Expr   *hi;
	long    step;
	std::vector<Stmt*> body;

	/* SK_IF */
	Expr *cond;
	std::vector<Stmt*> then_body;
	std::vector<Stmt*> else_body;
	bool  has_else;

	/* SK_CALL */
	std::string callee;

	/* SK_PRINT */
	Expr *arg;                  /* EK_IDENT or EK_INDEX                    */
};

/* ======================= declarations =============================== */
enum DeclKind {
	DK_ARRAY,    /* array <type> name [d0, d1, ...] ;                      */
	DK_SCALAR    /* scalar <type> name ;                                  */
};

struct Decl {
	DeclKind kind;
	Loc      loc;
	TypeTag  type;
	std::string name;
	std::vector<int> dims;      /* row-major extents; empty for DK_SCALAR  */
};

/* ======================= top-level items ============================ */
enum ItemKind {
	IK_DECL,
	IK_KERNEL,
	IK_HOST
};

struct Item {
	ItemKind kind;
	Loc      loc;
	Decl    *decl;              /* IK_DECL                                 */
	std::string name;          /* IK_KERNEL / IK_HOST                     */
	std::vector<Stmt*> body;   /* IK_KERNEL / IK_HOST                     */
};

struct Program {
	std::vector<Item*> items;   /* declarations, kernels, hosts in source order */
};

/* ---- node constructors (free functions -- no methods on the structs) --- */
Expr *mk_int   (long v, Loc l);
Expr *mk_float (double v, Loc l);
Expr *mk_ident (const char *n, Loc l);
Expr *mk_index (const char *n, std::vector<Expr*> *idx, Loc l);
Expr *mk_binop (int op, Expr *a, Expr *b, Loc l);
Expr *mk_unop  (int op, Expr *a, Loc l);

Stmt *mk_assign(Expr *target, Expr *value, Loc l);
Stmt *mk_for   (const char *iv_set, Expr *lo,
                const char *iv_test, int rel, Expr *hi,
                const char *iv_incr, long step,
                std::vector<Stmt*> *body, Loc l);
Stmt *mk_if    (Expr *cond, std::vector<Stmt*> *then_body,
                std::vector<Stmt*> *else_body, bool has_else, Loc l);
Stmt *mk_call  (const char *callee, Loc l);
Stmt *mk_print (Expr *arg, Loc l);

Decl *mk_array (int type, const char *n, std::vector<int> *dims, Loc l);
Decl *mk_scalar(int type, const char *n, Loc l);

Item *mk_item_decl  (Decl *d);
Item *mk_item_kernel(const char *n, std::vector<Stmt*> *body, Loc l);
Item *mk_item_host  (const char *n, std::vector<Stmt*> *body, Loc l);

/* ---- pretty printer (--dump ast) ------------------------------------- */
void ast_print_program(Program *p);

/* ---- small helpers -------------------------------------------------- */
const char *op_str  (int op);
const char *type_str(int t);

#endif /* NEMOC_AST_H */
