/* ============================================================================
 * ir.h  --  intermediate representation
 *
 * Compiler-theory concept: this IR deliberately mixes two levels of
 * representation.
 *
 *   - STATEMENT level: three-address code (3AC) (Aho/Lam/Sethi/Ullman 2nd ed.,
 *     section 6.2).  Every IRStmt does at most one operation -- one load, one
 *     store, one binary op, one copy -- and every operand is either a literal
 *     or a name (a declared scalar, a loop index, or a compiler-generated
 *     temporary like "%t3"). lower.cpp builds these by walking each source
 *     expression bottom-up and introducing a fresh temporary per subexpression
 *     (ALSU 6.2.1, "Three-Address Code").
 *
 *   - CONTROL level: kept STRUCTURED, not flattened.  A `for` loop is a
 *     LoopNest holding its own nested body, and `if`/`else` is an IfNode
 *     holding two nested bodies -- unlike a textbook compiler, which would
 *     dissolve both into a flat basic-block graph with gotos right here.  The
 *     reason is the rest of this project: dependence analysis, the offload
 *     cost model, loop fission/interchange/fusion and partitioning (Phases
 *     D-G) all need a real loop nest to walk, not a graph with the nesting
 *     information thrown away.  cfg.cpp builds a genuine flat control-flow
 *     graph FROM this same tree, on demand, for the passes that need one
 *     (classical dataflow analysis, Phase C and Phase G) -- so no information
 *     is lost, it is just not the primary representation.
 *
 * IndexExpr is the one piece of this header that phase B does not populate.
 * It exists here because it is part of the IR's vocabulary, but classifying a
 * subscript as affine (and computing its coefficients) is pass_affine.cpp's
 * job in Phase D.  Every subscript lower.cpp emits starts out IDX_NON_AFFINE.
 * ==========================================================================*/
#ifndef NEMOC_IR_H
#define NEMOC_IR_H

#include <string>
#include <vector>
#include <map>

#include "ast.h"   /* Loc, TypeTag, OpKind, Program (the AST lower.cpp reads) */

/* ---- three-address operands -------------------------------------------- */
enum OperandKind {
	OPD_INT,     /* a compile-time integer literal   */
	OPD_FLOAT,   /* a compile-time float literal      */
	OPD_NAME     /* a scalar, a loop index, or a temp */
};

struct Operand {
	OperandKind kind;
	TypeTag     type;    /* the value's type, tracked through lowering */
	long        ival;    /* OPD_INT   */
	double      dval;    /* OPD_FLOAT */
	std::string name;    /* OPD_NAME  */
};

Operand mk_opd_int  (long v);
Operand mk_opd_float(double v);
Operand mk_opd_name (const std::string &n, TypeTag t);

/* ---- affine classification of one array subscript ----------------------- *
 * IDX_AFFINE means the subscript equals  (sum of coeffs[loopvar] * loopvar)
 * + konst  for the loop variables enclosing this reference -- the access
 * matrix row Phase D's pass_affine.cpp and pass_dependence.cpp need. */
enum IndexKind {
	IDX_NON_AFFINE,
	IDX_AFFINE
};

struct IndexExpr {
	IndexKind                  kind;
	std::map<std::string, int> coeffs;   /* loop-var name -> coefficient, IDX_AFFINE only */
	int                        konst;    /* constant term, IDX_AFFINE only                */
};

IndexExpr mk_index_non_affine(void);

struct Subscript {
	Operand   value;    /* the runtime value used as this dimension's index */
	IndexExpr affine;   /* Phase D's classification of `value`              */
};

Subscript mk_subscript(Operand v);

/* ---- three-address statements ------------------------------------------ */
enum IRStmtKind {
	IR_ARRAY_LOAD,   /* dst            = array[idx...]  */
	IR_ARRAY_STORE,  /* array[idx...]  = src            */
	IR_BINOP,        /* dst            = lhs op rhs     */
	IR_COPY,         /* dst            = src            */
	IR_CALL,         /* call callee()                   */
	IR_PRINT         /* print src                        */
};

struct IRStmt {
	IRStmtKind kind;
	Loc        loc;

	std::string            dst;      /* IR_ARRAY_LOAD, IR_BINOP, IR_COPY  */
	std::string            array;    /* IR_ARRAY_LOAD, IR_ARRAY_STORE     */
	std::vector<Subscript> idx;      /* IR_ARRAY_LOAD, IR_ARRAY_STORE     */
	OpKind                 op;       /* IR_BINOP                          */
	Operand                lhs;      /* IR_BINOP                          */
	Operand                rhs;      /* IR_BINOP                          */
	Operand                src;      /* IR_ARRAY_STORE, IR_COPY, IR_PRINT */
	std::string            callee;   /* IR_CALL                           */
};

IRStmt *mk_load      (const std::string &dst, const std::string &array,
                      const std::vector<Subscript> &idx, Loc l);
IRStmt *mk_store     (const std::string &array, const std::vector<Subscript> &idx,
                      Operand src, Loc l);
IRStmt *mk_binop_stmt(const std::string &dst, int op, Operand lhs, Operand rhs, Loc l);
IRStmt *mk_copy      (const std::string &dst, Operand src, Loc l);
IRStmt *mk_call_stmt (const std::string &callee, Loc l);
IRStmt *mk_print_stmt(Operand src, Loc l);

std::string ir_stmt_text(IRStmt *s);   /* one-line rendering, shared by the
                                         * pretty printer and the .dot export */

/* ---- structured control: loop nests and if/else stay nested ------------ */
enum BodyKind {
	BK_STMT,
	BK_LOOP,
	BK_IF
};

struct LoopNest;
struct IfNode;

struct BodyItem {
	BodyKind  kind;
	IRStmt   *stmt;   /* BK_STMT */
	LoopNest *loop;   /* BK_LOOP */
	IfNode   *ifn;    /* BK_IF   */
};

BodyItem mk_bi_stmt(IRStmt *s);
BodyItem mk_bi_loop(LoopNest *l);
BodyItem mk_bi_if  (IfNode *n);

struct LoopNest {
	std::string           index_var;
	long                  lower;      /* constant -- semantic analysis requires it */
	OpKind                rel;        /* OP_LT or OP_LE                            */
	long                  upper;      /* constant                                  */
	long                  step;
	std::vector<BodyItem> body;
	Loc                   loc;
};

struct IfNode {
	Operand               cond;       /* a temp holding a prior comparison's result */
	std::vector<BodyItem> then_body;
	std::vector<BodyItem> else_body;
	bool                  has_else;
	Loc                   loc;
};

/* ---- kernels / host blocks / whole program ------------------------------ */
struct Kernel {
	std::string           name;
	bool                  is_host;    /* false: kernel (offload candidate); true: host */
	std::vector<BodyItem> body;
	Loc                   loc;
};

struct ArrayInfo {
	TypeTag          elem_type;
	int              elem_bytes;    /* 4: both int and float are 32-bit here */
	std::vector<int> dims;
};

struct IRProgram {
	std::map<std::string, ArrayInfo> arrays;
	std::map<std::string, TypeTag>   scalars;
	std::vector<Kernel*>             kernels;   /* offload candidates          */
	std::vector<Kernel*>             hosts;     /* definitely-CPU code         */
};

/* ---- construction (lower.cpp) ------------------------------------------ */
IRProgram *lower_program(Program *ast);   /* assumes semantic_check(ast) == 0 */

/* ---- pretty printer (ir.cpp), used by --dump ir ------------------------- */
void ir_print_program(IRProgram *p);

#endif /* NEMOC_IR_H */
