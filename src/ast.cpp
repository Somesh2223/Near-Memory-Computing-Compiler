/* ============================================================================
 * ast.cpp  --  AST node constructors and the tree pretty-printer
 *
 * Compiler-theory concept: syntax-directed translation.  Each grammar
 * production in parser.y has a semantic action that calls one constructor
 * here, so the AST is built bottom-up as the LALR parser reduces
 * (Aho/Lam/Sethi/Ullman 2nd ed., section 5.4, "Syntax-Directed Translation
 * Schemes").  The pretty-printer is a straightforward recursive pre-order
 * walk used by the `--dump ast` flag and by the Phase A demo.
 * ==========================================================================*/
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include "ast.h"

/* --------------------------------------------------------------------------
 * constructors
 * ------------------------------------------------------------------------*/
Expr *mk_int(long v, Loc l)
{
	Expr *e = new Expr();
	e->kind = EK_INT;
	e->loc  = l;
	e->ival = v;
	return e;
}

Expr *mk_float(double v, Loc l)
{
	Expr *e = new Expr();
	e->kind = EK_FLOAT;
	e->loc  = l;
	e->dval = v;
	return e;
}

Expr *mk_ident(const char *n, Loc l)
{
	Expr *e = new Expr();
	e->kind = EK_IDENT;
	e->loc  = l;
	e->name = n;
	return e;
}

Expr *mk_index(const char *n, std::vector<Expr*> *idx, Loc l)
{
	Expr *e = new Expr();
	e->kind  = EK_INDEX;
	e->loc   = l;
	e->name  = n;
	e->index = *idx;            /* copy the element pointers out of the list */
	delete idx;
	return e;
}

Expr *mk_binop(int op, Expr *a, Expr *b, Loc l)
{
	Expr *e = new Expr();
	e->kind = EK_BINOP;
	e->loc  = l;
	e->op   = (OpKind)op;
	e->lhs  = a;
	e->rhs  = b;
	return e;
}

Expr *mk_unop(int op, Expr *a, Loc l)
{
	Expr *e = new Expr();
	e->kind = EK_UNOP;
	e->loc  = l;
	e->op   = (OpKind)op;
	e->lhs  = a;
	e->rhs  = 0;
	return e;
}

Stmt *mk_assign(Expr *target, Expr *value, Loc l)
{
	Stmt *s = new Stmt();
	s->kind   = SK_ASSIGN;
	s->loc    = l;
	s->target = target;
	s->value  = value;
	return s;
}

Stmt *mk_for(const char *iv_set, Expr *lo,
             const char *iv_test, int rel, Expr *hi,
             const char *iv_incr, long step,
             std::vector<Stmt*> *body, Loc l)
{
	Stmt *s = new Stmt();
	s->kind      = SK_FOR;
	s->loc       = l;
	s->ivar      = iv_set;
	s->ivar_test = iv_test;
	s->ivar_incr = iv_incr;
	s->lo        = lo;
	s->rel       = (OpKind)rel;
	s->hi        = hi;
	s->step      = step;
	s->body      = *body;
	delete body;
	return s;
}

Stmt *mk_if(Expr *cond, std::vector<Stmt*> *then_body,
            std::vector<Stmt*> *else_body, bool has_else, Loc l)
{
	Stmt *s = new Stmt();
	s->kind      = SK_IF;
	s->loc       = l;
	s->cond      = cond;
	s->then_body = *then_body;
	s->else_body = *else_body;
	s->has_else  = has_else;
	delete then_body;
	delete else_body;
	return s;
}

Stmt *mk_call(const char *callee, Loc l)
{
	Stmt *s = new Stmt();
	s->kind   = SK_CALL;
	s->loc    = l;
	s->callee = callee;
	return s;
}

Stmt *mk_print(Expr *arg, Loc l)
{
	Stmt *s = new Stmt();
	s->kind = SK_PRINT;
	s->loc  = l;
	s->arg  = arg;
	return s;
}

Decl *mk_array(int type, const char *n, std::vector<int> *dims, Loc l)
{
	Decl *d = new Decl();
	d->kind = DK_ARRAY;
	d->loc  = l;
	d->type = (TypeTag)type;
	d->name = n;
	d->dims = *dims;
	delete dims;
	return d;
}

Decl *mk_scalar(int type, const char *n, Loc l)
{
	Decl *d = new Decl();
	d->kind = DK_SCALAR;
	d->loc  = l;
	d->type = (TypeTag)type;
	d->name = n;
	return d;
}

Item *mk_item_decl(Decl *d)
{
	Item *it = new Item();
	it->kind = IK_DECL;
	it->loc  = d->loc;
	it->decl = d;
	return it;
}

Item *mk_item_kernel(const char *n, std::vector<Stmt*> *body, Loc l)
{
	Item *it = new Item();
	it->kind = IK_KERNEL;
	it->loc  = l;
	it->name = n;
	it->body = *body;
	delete body;
	return it;
}

Item *mk_item_host(const char *n, std::vector<Stmt*> *body, Loc l)
{
	Item *it = new Item();
	it->kind = IK_HOST;
	it->loc  = l;
	it->name = n;
	it->body = *body;
	delete body;
	return it;
}

/* --------------------------------------------------------------------------
 * helpers
 * ------------------------------------------------------------------------*/
const char *op_str(int op)
{
	switch ((OpKind)op) {
	case OP_ADD: return "+";
	case OP_SUB: return "-";
	case OP_MUL: return "*";
	case OP_DIV: return "/";
	case OP_MOD: return "%";
	case OP_LT:  return "<";
	case OP_LE:  return "<=";
	case OP_GT:  return ">";
	case OP_GE:  return ">=";
	case OP_EQ:  return "==";
	case OP_NE:  return "!=";
	case OP_NEG: return "neg";
	}
	return "?";
}

const char *type_str(int t)
{
	switch ((TypeTag)t) {
	case TY_INT:   return "int";
	case TY_FLOAT: return "float";
	}
	return "?";
}

/* --------------------------------------------------------------------------
 * pretty printer -- recursive pre-order walk
 * ------------------------------------------------------------------------*/
static void indent(int depth)
{
	int i;
	for (i = 0; i < depth; i++)
		printf("  ");
}

static void print_expr(Expr *e, int depth)
{
	indent(depth);
	switch (e->kind) {
	case EK_INT:
		printf("int %ld\n", e->ival);
		break;
	case EK_FLOAT:
		printf("float %g\n", e->dval);
		break;
	case EK_IDENT:
		printf("ident %s\n", e->name.c_str());
		break;
	case EK_INDEX:
		printf("index %s  (%d subscript%s)\n",
		       e->name.c_str(), (int)e->index.size(),
		       e->index.size() == 1 ? "" : "s");
		for (size_t i = 0; i < e->index.size(); i++)
			print_expr(e->index[i], depth + 1);
		break;
	case EK_BINOP:
		printf("binop '%s'\n", op_str(e->op));
		print_expr(e->lhs, depth + 1);
		print_expr(e->rhs, depth + 1);
		break;
	case EK_UNOP:
		printf("unop '%s'\n", op_str(e->op));
		print_expr(e->lhs, depth + 1);
		break;
	}
}

static void print_stmt(Stmt *s, int depth)
{
	indent(depth);
	switch (s->kind) {
	case SK_ASSIGN:
		printf("assign  @%d:%d\n", s->loc.line, s->loc.col);
		indent(depth + 1); printf("target:\n");
		print_expr(s->target, depth + 2);
		indent(depth + 1); printf("value:\n");
		print_expr(s->value, depth + 2);
		break;

	case SK_FOR:
		printf("for  %s = ..  ;  %s '%s' ..  ;  %s += %ld   @%d:%d\n",
		       s->ivar.c_str(), s->ivar_test.c_str(), op_str(s->rel),
		       s->ivar_incr.c_str(), s->step, s->loc.line, s->loc.col);
		indent(depth + 1); printf("lower:\n");
		print_expr(s->lo, depth + 2);
		indent(depth + 1); printf("upper:\n");
		print_expr(s->hi, depth + 2);
		indent(depth + 1); printf("body:\n");
		for (size_t i = 0; i < s->body.size(); i++)
			print_stmt(s->body[i], depth + 2);
		break;

	case SK_IF:
		printf("if   @%d:%d\n", s->loc.line, s->loc.col);
		indent(depth + 1); printf("cond:\n");
		print_expr(s->cond, depth + 2);
		indent(depth + 1); printf("then:\n");
		for (size_t i = 0; i < s->then_body.size(); i++)
			print_stmt(s->then_body[i], depth + 2);
		if (s->has_else) {
			indent(depth + 1); printf("else:\n");
			for (size_t i = 0; i < s->else_body.size(); i++)
				print_stmt(s->else_body[i], depth + 2);
		}
		break;

	case SK_CALL:
		printf("call %s()   @%d:%d\n", s->callee.c_str(),
		       s->loc.line, s->loc.col);
		break;

	case SK_PRINT:
		printf("print   @%d:%d\n", s->loc.line, s->loc.col);
		print_expr(s->arg, depth + 1);
		break;
	}
}

static void print_decl(Decl *d, int depth)
{
	indent(depth);
	if (d->kind == DK_ARRAY) {
		printf("array %s %s[", type_str(d->type), d->name.c_str());
		for (size_t i = 0; i < d->dims.size(); i++)
			printf("%s%d", i ? ", " : "", d->dims[i]);
		printf("]   @%d:%d\n", d->loc.line, d->loc.col);
	} else {
		printf("scalar %s %s   @%d:%d\n", type_str(d->type),
		       d->name.c_str(), d->loc.line, d->loc.col);
	}
}

void ast_print_program(Program *p)
{
	int n_decl = 0, n_kernel = 0, n_host = 0;
	size_t i;

	printf("program\n");
	for (i = 0; i < p->items.size(); i++) {
		Item *it = p->items[i];
		switch (it->kind) {
		case IK_DECL:
			print_decl(it->decl, 1);
			n_decl++;
			break;
		case IK_KERNEL:
			indent(1);
			printf("kernel %s()   @%d:%d\n", it->name.c_str(),
			       it->loc.line, it->loc.col);
			for (size_t j = 0; j < it->body.size(); j++)
				print_stmt(it->body[j], 2);
			n_kernel++;
			break;
		case IK_HOST:
			indent(1);
			printf("host %s()   @%d:%d\n", it->name.c_str(),
			       it->loc.line, it->loc.col);
			for (size_t j = 0; j < it->body.size(); j++)
				print_stmt(it->body[j], 2);
			n_host++;
			break;
		}
	}
	printf("--- %d declaration%s, %d kernel%s, %d host block%s ---\n",
	       n_decl,   n_decl   == 1 ? "" : "s",
	       n_kernel, n_kernel == 1 ? "" : "s",
	       n_host,   n_host   == 1 ? "" : "s");
}
