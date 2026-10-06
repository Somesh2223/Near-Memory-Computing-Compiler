/* ============================================================================
 * lower.cpp  --  AST -> IR lowering
 *
 * Compiler-theory concept: translating expressions into three-address code by
 * introducing a fresh temporary for every subexpression's result, bottom-up
 * (Aho/Lam/Sethi/Ullman 2nd ed., section 6.2.1, "Three-Address Code" and 6.4,
 * "Translation of Expressions").  For example
 *
 *     C[i] = alpha * A[i] + B[i];
 *
 * lowers to
 *
 *     %t0 = A[i]
 *     %t1 = alpha * %t0
 *     %t2 = B[i]
 *     %t3 = %t1 + %t2
 *     C[i] = %t3
 *
 * Only expressions are flattened this way.  Loop and if/else STRUCTURE is
 * copied straight across into LoopNest / IfNode, unflattened -- see the long
 * comment at the top of ir.h for why.  A loop's bounds are folded to plain
 * integers here (not left as expressions) because semantic.cpp already proved
 * they are compile-time constants; folding them now means every later pass
 * gets a `long`, not an expression tree to re-evaluate.
 *
 * This pass trusts its input: it assumes `semantic_check(ast)` already
 * returned 0.  It does not re-check for undeclared names, dimension
 * mismatches, or non-constant bounds -- main.cpp enforces that ordering.
 * ==========================================================================*/
#include <cstdio>
#include <vector>
#include <string>

#include "ast.h"
#include "ir.h"

/* ---- lowering state ----------------------------------------------------- */
struct LowerCtx {
	IRProgram                *prog;
	int                        temp_no;
	std::vector<std::string>  loopvars;   /* active enclosing loop indices, all TY_INT */
};

static std::string new_temp(LowerCtx *c)
{
	char buf[32];
	snprintf(buf, sizeof buf, "%%t%d", c->temp_no++);
	return std::string(buf);
}

static bool is_loopvar(LowerCtx *c, const std::string &name)
{
	size_t i;
	for (i = 0; i < c->loopvars.size(); i++)
		if (c->loopvars[i] == name)
			return true;
	return false;
}

/* type of a scalar-ish name: an active loop index is always int; otherwise
 * look it up in the global scalar table semantic analysis already validated */
static TypeTag name_type(LowerCtx *c, const std::string &name)
{
	if (is_loopvar(c, name))
		return TY_INT;
	std::map<std::string, TypeTag>::iterator it = c->prog->scalars.find(name);
	if (it != c->prog->scalars.end())
		return it->second;
	return TY_INT;   /* unreachable once semantic analysis has passed */
}

/* re-evaluate a loop bound that semantic.cpp already proved constant */
static long fold_const_int(Expr *e)
{
	if (e->kind == EK_INT)
		return e->ival;
	if (e->kind == EK_UNOP && e->op == OP_NEG)
		return -fold_const_int(e->lhs);
	if (e->kind == EK_BINOP) {
		long a = fold_const_int(e->lhs);
		long b = fold_const_int(e->rhs);
		switch (e->op) {
		case OP_ADD: return a + b;
		case OP_SUB: return a - b;
		case OP_MUL: return a * b;
		case OP_DIV: return b != 0 ? a / b : 0;
		case OP_MOD: return b != 0 ? a % b : 0;
		default:     return 0;
		}
	}
	return 0;   /* unreachable: semantic.cpp rejected anything else */
}

static Operand lower_expr(LowerCtx *c, Expr *e, std::vector<BodyItem> *out);

static std::vector<Subscript> lower_subscripts(LowerCtx *c, std::vector<Expr*> &idx,
                                               std::vector<BodyItem> *out)
{
	std::vector<Subscript> subs;
	size_t i;
	for (i = 0; i < idx.size(); i++)
		subs.push_back(mk_subscript(lower_expr(c, idx[i], out)));
	return subs;
}

static Operand lower_expr(LowerCtx *c, Expr *e, std::vector<BodyItem> *out)
{
	switch (e->kind) {
	case EK_INT:
		return mk_opd_int(e->ival);

	case EK_FLOAT:
		return mk_opd_float(e->dval);

	case EK_IDENT:
		return mk_opd_name(e->name, name_type(c, e->name));

	case EK_INDEX: {
		std::vector<Subscript> idx = lower_subscripts(c, e->index, out);
		TypeTag elem_type = TY_INT;
		std::map<std::string, ArrayInfo>::iterator it = c->prog->arrays.find(e->name);
		if (it != c->prog->arrays.end())
			elem_type = it->second.elem_type;

		std::string t = new_temp(c);
		out->push_back(mk_bi_stmt(mk_load(t, e->name, idx, elem_type, e->loc)));
		return mk_opd_name(t, elem_type);
	}

	case EK_UNOP: {
		Operand v = lower_expr(c, e->lhs, out);
		Operand zero = (v.type == TY_FLOAT) ? mk_opd_float(0.0) : mk_opd_int(0);
		std::string t = new_temp(c);
		out->push_back(mk_bi_stmt(mk_binop_stmt(t, OP_SUB, zero, v, e->loc)));
		return mk_opd_name(t, v.type);
	}

	case EK_BINOP: {
		Operand a = lower_expr(c, e->lhs, out);
		Operand b = lower_expr(c, e->rhs, out);
		TypeTag rt;
		switch (e->op) {
		case OP_LT: case OP_LE: case OP_GT:
		case OP_GE: case OP_EQ: case OP_NE:
			rt = TY_INT;                                  /* comparison -> int */
			break;
		default:
			rt = (a.type == TY_FLOAT || b.type == TY_FLOAT) ? TY_FLOAT : TY_INT;
		}
		std::string t = new_temp(c);
		out->push_back(mk_bi_stmt(mk_binop_stmt(t, e->op, a, b, e->loc)));
		return mk_opd_name(t, rt);
	}
	}
	return mk_opd_int(0);   /* unreachable */
}

static void lower_stmt(LowerCtx *c, Stmt *s, std::vector<BodyItem> *out)
{
	switch (s->kind) {

	case SK_ASSIGN: {
		if (s->target->kind == EK_IDENT) {
			Operand v = lower_expr(c, s->value, out);
			out->push_back(mk_bi_stmt(mk_copy(s->target->name, v,
			                                  name_type(c, s->target->name), s->loc)));
		} else {
			std::vector<Subscript> idx = lower_subscripts(c, s->target->index, out);
			Operand v = lower_expr(c, s->value, out);
			out->push_back(mk_bi_stmt(mk_store(s->target->name, idx, v, s->loc)));
		}
		break;
	}

	case SK_FOR: {
		LoopNest *ln = new LoopNest();
		ln->index_var = s->ivar;
		ln->lower     = fold_const_int(s->lo);
		ln->rel       = s->rel;
		ln->upper     = fold_const_int(s->hi);
		ln->step      = s->step;
		ln->loc       = s->loc;

		c->loopvars.push_back(s->ivar);
		for (size_t i = 0; i < s->body.size(); i++)
			lower_stmt(c, s->body[i], &ln->body);
		c->loopvars.pop_back();

		out->push_back(mk_bi_loop(ln));
		break;
	}

	case SK_IF: {
		Operand cond = lower_expr(c, s->cond, out);   /* comparison -> a temp */

		IfNode *ifn = new IfNode();
		ifn->cond     = cond;
		ifn->has_else = s->has_else;
		ifn->loc      = s->loc;

		for (size_t i = 0; i < s->then_body.size(); i++)
			lower_stmt(c, s->then_body[i], &ifn->then_body);
		if (s->has_else)
			for (size_t i = 0; i < s->else_body.size(); i++)
				lower_stmt(c, s->else_body[i], &ifn->else_body);

		out->push_back(mk_bi_if(ifn));
		break;
	}

	case SK_CALL:
		out->push_back(mk_bi_stmt(mk_call_stmt(s->callee, s->loc)));
		break;

	case SK_PRINT: {
		Operand v = lower_expr(c, s->arg, out);
		out->push_back(mk_bi_stmt(mk_print_stmt(v, s->loc)));
		break;
	}
	}
}

IRProgram *lower_program(Program *ast)
{
	IRProgram *p = new IRProgram();
	size_t i, j;

	/* arrays / scalars first, so lower_expr can look up element types */
	for (i = 0; i < ast->items.size(); i++) {
		Item *it = ast->items[i];
		if (it->kind != IK_DECL)
			continue;
		Decl *d = it->decl;
		if (d->kind == DK_ARRAY) {
			ArrayInfo ai;
			ai.elem_type  = d->type;
			ai.elem_bytes = 4;      /* both int and float are 32-bit here */
			ai.dims       = d->dims;
			p->arrays[d->name] = ai;
		} else {
			p->scalars[d->name] = d->type;
		}
	}

	LowerCtx c;
	c.prog = p;

	for (i = 0; i < ast->items.size(); i++) {
		Item *it = ast->items[i];
		if (it->kind != IK_KERNEL && it->kind != IK_HOST)
			continue;

		Kernel *k = new Kernel();
		k->name    = it->name;
		k->is_host = (it->kind == IK_HOST);
		k->loc     = it->loc;

		c.temp_no = 0;             /* temp names restart per kernel/host body */
		c.loopvars.clear();
		for (j = 0; j < it->body.size(); j++)
			lower_stmt(&c, it->body[j], &k->body);

		if (k->is_host)
			p->hosts.push_back(k);
		else
			p->kernels.push_back(k);
	}

	return p;
}
