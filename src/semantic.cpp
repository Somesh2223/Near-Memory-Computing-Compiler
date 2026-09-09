/* ============================================================================
 * semantic.cpp  --  semantic analysis / static checking
 *
 * Compiler-theory concept: the semantic pass enforces the context-sensitive
 * rules a context-free grammar cannot (Aho/Lam/Sethi/Ullman 2nd ed., 2.7-2.8
 * and chapter 6).  It is a syntax-directed traversal of the AST that does two
 * things at once:
 *
 *   - identification / name resolution: every identifier use is matched to a
 *     declaration in the scoped symbol table (symtab.cpp); an unmatched use is
 *     an "undeclared identifier" error;
 *
 *   - type checking by bottom-up type synthesis (ALSU 6.3): each expression's
 *     type is computed from its operands' types.  The one implicit conversion
 *     is int -> float promotion; float -> int on assignment is rejected.
 *
 * Two-pass scheme: pass 1 registers every top-level name (so a host block may
 * `call` a kernel declared later); pass 2 walks the kernel and host bodies.
 *
 * Errors are accumulated, never fatal -- the walk recovers with a sane default
 * type and keeps going, so one run reports as many problems as possible
 * (ALSU 4.1.3, error recovery in the analysis phases).
 *
 * NEMO-C-specific checks, and why each one exists:
 *   - loop bounds must be compile-time integer constants  -> Phase E needs an
 *     exact trip count for the cost model;
 *   - the three for-header slots must name the same variable, the step must be
 *     positive and the test must be '<' or '<=' -> Phase D dependence analysis
 *     assumes a normalised, monotonically increasing loop;
 *   - a loop index may not be reassigned in the body (nor reused by a nested
 *     loop) -> it must stay an affine function of the iteration number;
 *   - `call` / `print` may appear only in host code -> the kernel/host split
 *     the parser could not enforce (see the note in parser.y).
 * ==========================================================================*/
#include <cstdio>
#include <cstdarg>
#include <string>
#include <vector>

#include "ast.h"
#include "symtab.h"
#include "semantic.h"

extern const char *g_srcfile;

/* ---- state carried through the traversal ------------------------------- */
struct SemCtx {
	SymTab                   st;
	int                      errors;
	bool                     in_kernel;   /* true: inside a kernel body      */
	std::vector<std::string> loopvars;    /* active enclosing loop indices   */
};

static void serr(SemCtx *c, Loc loc, const char *fmt, ...)
	__attribute__((format(printf, 3, 4)));

static void serr(SemCtx *c, Loc loc, const char *fmt, ...)
{
	va_list ap;
	va_start(ap, fmt);
	fprintf(stderr, "%s:%d:%d: error: ", g_srcfile, loc.line, loc.col);
	vfprintf(stderr, fmt, ap);
	fputc('\n', stderr);
	va_end(ap);
	c->errors++;
}

static bool is_active_loopvar(SemCtx *c, const std::string &name)
{
	size_t i;
	for (i = 0; i < c->loopvars.size(); i++)
		if (c->loopvars[i] == name)
			return true;
	return false;
}

/* ---- compile-time integer evaluation, for loop bounds ------------------ *
 * A NEMO-C loop bound is "constant" iff it is built only from integer
 * literals, unary minus and + - * / % -- exactly the fragment whose value the
 * compiler can compute now.  Anything else (a scalar, an array element, a
 * float) is not constant. */
static bool const_int(Expr *e, long *out)
{
	if (e->kind == EK_INT) {
		*out = e->ival;
		return true;
	}
	if (e->kind == EK_UNOP && e->op == OP_NEG) {
		long v;
		if (!const_int(e->lhs, &v))
			return false;
		*out = -v;
		return true;
	}
	if (e->kind == EK_BINOP) {
		long a, b;
		if (!const_int(e->lhs, &a) || !const_int(e->rhs, &b))
			return false;
		switch (e->op) {
		case OP_ADD: *out = a + b; return true;
		case OP_SUB: *out = a - b; return true;
		case OP_MUL: *out = a * b; return true;
		case OP_DIV: if (b == 0) return false; *out = a / b; return true;
		case OP_MOD: if (b == 0) return false; *out = a % b; return true;
		default:     return false;   /* a comparison is not an integer const */
		}
	}
	return false;
}

/* ---- expression type synthesis --------------------------------------- *
 * Returns the type of the expression.  On any error it reports once and
 * returns a plausible type (TY_INT) so the rest of the walk does not
 * cascade. */
static TypeTag check_expr(SemCtx *c, Expr *e)
{
	switch (e->kind) {
	case EK_INT:
		return TY_INT;

	case EK_FLOAT:
		return TY_FLOAT;

	case EK_IDENT: {
		Sym *s = st_lookup(&c->st, e->name);
		if (s == 0) {
			serr(c, e->loc, "undeclared identifier '%s'", e->name.c_str());
			return TY_INT;
		}
		if (s->kind == SYM_ARRAY) {
			serr(c, e->loc, "array '%s' used without a subscript",
			     e->name.c_str());
			return s->type;
		}
		if (s->kind == SYM_KERNEL || s->kind == SYM_HOST) {
			serr(c, e->loc, "'%s' names a %s, not a value", e->name.c_str(),
			     s->kind == SYM_KERNEL ? "kernel" : "host block");
			return TY_INT;
		}
		return s->type;                  /* SYM_SCALAR or SYM_LOOPVAR */
	}

	case EK_INDEX: {
		Sym *s = st_lookup(&c->st, e->name);
		size_t i;
		if (s == 0) {
			serr(c, e->loc, "undeclared identifier '%s'", e->name.c_str());
			for (i = 0; i < e->index.size(); i++)
				check_expr(c, e->index[i]);
			return TY_INT;
		}
		if (s->kind != SYM_ARRAY) {
			serr(c, e->loc, "'%s' is not an array but is subscripted",
			     e->name.c_str());
			for (i = 0; i < e->index.size(); i++)
				check_expr(c, e->index[i]);
			return s->type;
		}
		if (e->index.size() != s->dims.size()) {
			serr(c, e->loc,
			     "'%s' has %d dimension%s but is used with %d subscript%s",
			     e->name.c_str(),
			     (int)s->dims.size(), s->dims.size() == 1 ? "" : "s",
			     (int)e->index.size(), e->index.size() == 1 ? "" : "s");
		}
		for (i = 0; i < e->index.size(); i++) {
			TypeTag it = check_expr(c, e->index[i]);
			if (it == TY_FLOAT)
				serr(c, e->index[i]->loc,
				     "array subscript must be an integer expression");
		}
		return s->type;                  /* the element type */
	}

	case EK_UNOP:
		return check_expr(c, e->lhs);

	case EK_BINOP: {
		TypeTag lt = check_expr(c, e->lhs);
		TypeTag rt = check_expr(c, e->rhs);
		switch (e->op) {
		case OP_LT: case OP_LE: case OP_GT:
		case OP_GE: case OP_EQ: case OP_NE:
			return TY_INT;                       /* comparison yields int  */
		case OP_MOD:
			if (lt == TY_FLOAT || rt == TY_FLOAT)
				serr(c, e->loc, "'%%' requires integer operands");
			return TY_INT;
		default:                                    /* + - * /             */
			if (lt == TY_FLOAT || rt == TY_FLOAT)
				return TY_FLOAT;                /* int operand promoted    */
			return TY_INT;
		}
	}
	}
	return TY_INT;
}

/* ---- assignment target ---------------------------------------------- *
 * On success sets *dst_type and *ok; otherwise reports and leaves *ok false. */
static void check_target(SemCtx *c, Expr *t, TypeTag *dst_type, bool *ok)
{
	*ok = false;
	*dst_type = TY_INT;

	if (t->kind == EK_IDENT) {
		if (is_active_loopvar(c, t->name)) {
			serr(c, t->loc,
			     "loop index variable '%s' must not be reassigned in the loop body",
			     t->name.c_str());
			return;
		}
		Sym *s = st_lookup(&c->st, t->name);
		if (s == 0) {
			serr(c, t->loc, "assignment to undeclared identifier '%s'",
			     t->name.c_str());
			return;
		}
		if (s->kind == SYM_ARRAY) {
			serr(c, t->loc, "cannot assign to array '%s' without a subscript",
			     t->name.c_str());
			return;
		}
		if (s->kind != SYM_SCALAR) {
			serr(c, t->loc, "'%s' is not assignable", t->name.c_str());
			return;
		}
		*dst_type = s->type;
		*ok = true;
		return;
	}

	/* EK_INDEX target: let check_expr report array-kind / subscript errors */
	check_expr(c, t);
	Sym *s = st_lookup(&c->st, t->name);
	if (s != 0 && s->kind == SYM_ARRAY && t->index.size() == s->dims.size()) {
		*dst_type = s->type;
		*ok = true;
	}
}

/* ---- statements ---------------------------------------------------- */
static void check_block(SemCtx *c, std::vector<Stmt*> &body);

static void check_stmt(SemCtx *c, Stmt *s)
{
	switch (s->kind) {

	case SK_ASSIGN: {
		TypeTag dt;
		bool    ok;
		check_target(c, s->target, &dt, &ok);
		TypeTag vt = check_expr(c, s->value);
		if (ok && dt == TY_INT && vt == TY_FLOAT)
			serr(c, s->loc,
			     "cannot assign a float value to an integer location "
			     "(no implicit narrowing)");
		break;
	}

	case SK_FOR: {
		long tmp;

		if (s->ivar != s->ivar_test || s->ivar != s->ivar_incr)
			serr(c, s->loc,
			     "for-header names inconsistent index variables "
			     "('%s', '%s', '%s')",
			     s->ivar.c_str(), s->ivar_test.c_str(), s->ivar_incr.c_str());

		if (!const_int(s->lo, &tmp)) {
			serr(c, s->lo->loc,
			     "loop lower bound must be a constant integer expression");
			check_expr(c, s->lo);
		}
		if (!const_int(s->hi, &tmp)) {
			serr(c, s->hi->loc,
			     "loop upper bound must be a constant integer expression");
			check_expr(c, s->hi);
		}

		if (s->step <= 0)
			serr(c, s->loc, "loop step must be positive (got %ld)", s->step);
		if (s->rel != OP_LT && s->rel != OP_LE)
			serr(c, s->loc,
			     "loop test must be '<' or '<=' (the loop increases by a "
			     "positive step)");

		if (is_active_loopvar(c, s->ivar))
			serr(c, s->loc,
			     "loop index '%s' shadows an enclosing loop index",
			     s->ivar.c_str());

		st_push(&c->st);
		st_declare(&c->st, SYM_LOOPVAR, s->ivar, TY_INT,
		           std::vector<int>(), s->loc);
		c->loopvars.push_back(s->ivar);

		check_block(c, s->body);

		c->loopvars.pop_back();
		st_pop(&c->st);
		break;
	}

	case SK_IF: {
		check_expr(c, s->cond);

		st_push(&c->st);
		check_block(c, s->then_body);
		st_pop(&c->st);

		if (s->has_else) {
			st_push(&c->st);
			check_block(c, s->else_body);
			st_pop(&c->st);
		}
		break;
	}

	case SK_CALL: {
		if (c->in_kernel)
			serr(c, s->loc,
			     "'call' is not allowed inside a kernel (host-only statement)");
		Sym *k = st_lookup(&c->st, s->callee);
		if (k == 0)
			serr(c, s->loc, "call to undeclared kernel '%s'", s->callee.c_str());
		else if (k->kind != SYM_KERNEL)
			serr(c, s->loc, "'%s' is not a kernel", s->callee.c_str());
		break;
	}

	case SK_PRINT: {
		if (c->in_kernel)
			serr(c, s->loc,
			     "'print' is not allowed inside a kernel (host-only statement)");
		check_expr(c, s->arg);
		break;
	}
	}
}

static void check_block(SemCtx *c, std::vector<Stmt*> &body)
{
	size_t i;
	for (i = 0; i < body.size(); i++)
		check_stmt(c, body[i]);
}

/* ---- driver: two passes over the top-level items -------------------- */
int semantic_check(Program *p)
{
	SemCtx c;
	size_t i, j;

	c.errors    = 0;
	c.in_kernel = false;
	st_init(&c.st);

	/* pass 1 -- register every top-level name in the global scope */
	for (i = 0; i < p->items.size(); i++) {
		Item *it = p->items[i];

		if (it->kind == IK_DECL) {
			Decl *d = it->decl;
			for (j = 0; j < d->dims.size(); j++)
				if (d->dims[j] < 1)
					serr(&c, d->loc,
					     "array '%s' dimension %d must be positive",
					     d->name.c_str(), (int)j + 1);

			SymKind k = (d->kind == DK_ARRAY) ? SYM_ARRAY : SYM_SCALAR;
			Sym *clash = st_declare(&c.st, k, d->name, d->type,
			                        d->dims, d->loc);
			if (clash != 0)
				serr(&c, d->loc,
				     "redeclaration of '%s' (first declared at %d:%d)",
				     d->name.c_str(), clash->loc.line, clash->loc.col);
		} else {
			SymKind k = (it->kind == IK_KERNEL) ? SYM_KERNEL : SYM_HOST;
			Sym *clash = st_declare(&c.st, k, it->name, TY_INT,
			                        std::vector<int>(), it->loc);
			if (clash != 0)
				serr(&c, it->loc,
				     "redeclaration of '%s' (first declared at %d:%d)",
				     it->name.c_str(), clash->loc.line, clash->loc.col);
		}
	}

	/* pass 2 -- walk kernel and host bodies */
	for (i = 0; i < p->items.size(); i++) {
		Item *it = p->items[i];
		if (it->kind != IK_KERNEL && it->kind != IK_HOST)
			continue;

		c.in_kernel = (it->kind == IK_KERNEL);
		st_push(&c.st);
		check_block(&c, it->body);
		st_pop(&c.st);
	}

	return c.errors;
}
