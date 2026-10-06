/* ============================================================================
 * pass_affine.cpp  --  affine access analysis
 *
 * Compiler-theory concept: affine array-subscript analysis, the foundation of
 * dependence testing (Aho/Lam/Sethi/Ullman 2nd ed., section 11.3 "Affine Array
 * Indexes"; Wolf & Lam 1991).  A reference to an m-dimensional array inside a
 * loop nest with index vector  i = (i_1 ... i_n)  is AFFINE when every
 * subscript is a linear function of the loop indices plus a constant, so the
 * whole reference is
 *
 *                       f(i) = F * i + c
 *
 * with F an (m x n) integer ACCESS MATRIX (row = array dimension, column =
 * enclosing loop) and c an integer OFFSET VECTOR.  Everything downstream --
 * the GCD and Banerjee tests, reuse classification, stride computation,
 * partitioning -- reads F and c.
 *
 * lower.cpp leaves each subscript as a named operand (a loop index, a temp, a
 * constant).  This pass recovers the affine form by following definitions
 * backwards: a temp defined by  t = u + v,  u - v,  u * const,  or a copy is
 * affine if its inputs are; a loop index contributes coefficient 1; a derived
 * induction variable created by strength reduction is looked up in
 * IRProgram::ivs.  A subscript that depends on a loaded value (A[B[i]]), on a
 * global scalar, or on a product of two loop indices is NOT affine, and the
 * reference is marked so -- which makes the enclosing nest "not analysable".
 * ==========================================================================*/
#include <cstdio>
#include <map>
#include <string>
#include <vector>

#include "ir.h"
#include "analysis.h"

long loop_trips(LoopNest *l)
{
	if (l->step <= 0)
		return 0;
	if (l->rel == OP_LT) {
		if (l->upper <= l->lower)
			return 0;
		return (l->upper - l->lower + l->step - 1) / l->step;
	}
	if (l->upper < l->lower)
		return 0;
	return (l->upper - l->lower) / l->step + 1;
}

/* an affine form under construction: sum of co[var]*var + k */
struct Aff {
	bool                       ok;
	std::map<std::string,long> co;
	long                       k;
};

static Aff aff_fail(void)
{
	Aff a;
	a.ok = false;
	a.k = 0;
	return a;
}

static Aff aff_const(long k)
{
	Aff a;
	a.ok = true;
	a.k = k;
	return a;
}

static Aff aff_var(const std::string &v, long coeff, long k)
{
	Aff a;
	a.ok = true;
	a.k = k;
	a.co[v] = coeff;
	return a;
}

static Aff aff_add(Aff x, Aff y, long sign)
{
	if (!x.ok || !y.ok)
		return aff_fail();
	std::map<std::string,long>::iterator it;
	for (it = y.co.begin(); it != y.co.end(); it++)
		x.co[it->first] += sign * it->second;
	x.k += sign * y.k;
	return x;
}

static bool aff_is_const(Aff &a)
{
	std::map<std::string,long>::iterator it;
	if (!a.ok)
		return false;
	for (it = a.co.begin(); it != a.co.end(); it++)
		if (it->second != 0)
			return false;
	return true;
}

static Aff aff_scale(Aff x, long c)
{
	if (!x.ok)
		return x;
	std::map<std::string,long>::iterator it;
	for (it = x.co.begin(); it != x.co.end(); it++)
		it->second *= c;
	x.k *= c;
	return x;
}

struct AffCtx {
	IRProgram                     *p;
	NestAnalysis                  *na;
	std::vector<LoopNest*>        *enclosing;
};

static bool is_enclosing(AffCtx *c, const std::string &name)
{
	size_t i;
	for (i = 0; i < c->enclosing->size(); i++)
		if ((*c->enclosing)[i]->index_var == name)
			return true;
	return false;
}

static Aff resolve_operand(AffCtx *c, const Operand &o, int depth);

static Aff resolve_def(AffCtx *c, IRStmt *s, int depth)
{
	if (s->kind == IR_COPY)
		return resolve_operand(c, s->src, depth + 1);

	if (s->kind == IR_BINOP) {
		Aff l = resolve_operand(c, s->lhs, depth + 1);
		Aff r = resolve_operand(c, s->rhs, depth + 1);
		if (!l.ok || !r.ok)
			return aff_fail();
		if (s->op == OP_ADD)
			return aff_add(l, r, 1);
		if (s->op == OP_SUB)
			return aff_add(l, r, -1);
		if (s->op == OP_MUL) {
			if (aff_is_const(r))
				return aff_scale(l, r.k);
			if (aff_is_const(l))
				return aff_scale(r, l.k);
		}
	}
	return aff_fail();         /* a load, a division, a product of indices ... */
}

static Aff resolve_operand(AffCtx *c, const Operand &o, int depth)
{
	if (depth > 40)
		return aff_fail();
	if (o.kind == OPD_INT)
		return aff_const(o.ival);
	if (o.kind == OPD_FLOAT)
		return aff_fail();

	if (is_enclosing(c, o.name))
		return aff_var(o.name, 1, 0);

	/* a derived IV introduced by strength reduction */
	std::map<std::string, IVInfo>::iterator iv = c->p->ivs.find(o.name);
	if (iv != c->p->ivs.end()) {
		if (!is_enclosing(c, iv->second.basic))
			return aff_fail();
		return aff_var(iv->second.basic, iv->second.coeff, iv->second.offset);
	}

	std::map<std::string, IRStmt*>::iterator d = c->na->defs.find(o.name);
	if (d != c->na->defs.end())
		return resolve_def(c, d->second, depth);
	return aff_fail();
}

/* ---- reference collection ---------------------------------------------------- */
struct CollectCtx {
	IRProgram    *p;
	NestAnalysis *na;
	int           seq;
};

static bool is_global_scalar(IRProgram *p, const std::string &n)
{
	return p->scalars.find(n) != p->scalars.end();
}

static void add_scalar_ref(CollectCtx *cc, IRStmt *s, const std::string &name,
                           bool is_write, std::vector<LoopNest*> &stack)
{
	RefInfo r;
	r.stmt      = s;
	r.is_write  = is_write;
	r.is_scalar = true;
	r.name      = name;
	r.loops     = stack;
	r.affine    = true;
	r.order     = 2 * cc->seq + (is_write ? 1 : 0);
	r.red_group = -1;
	cc->na->refs.push_back(r);
}

static void add_array_ref(CollectCtx *cc, IRStmt *s, bool is_write,
                          std::vector<LoopNest*> &stack)
{
	RefInfo r;
	size_t k, j;
	AffCtx ac;
	ac.p = cc->p;
	ac.na = cc->na;
	ac.enclosing = &stack;

	r.stmt      = s;
	r.is_write  = is_write;
	r.is_scalar = false;
	r.name      = s->array;
	r.loops     = stack;
	r.affine    = true;
	r.order     = 2 * cc->seq + (is_write ? 1 : 0);
	r.red_group = -1;

	for (k = 0; k < s->idx.size(); k++) {
		Aff a = resolve_operand(&ac, s->idx[k].value, 0);
		std::vector<long> row(stack.size(), 0);
		if (!a.ok) {
			r.affine = false;
			s->idx[k].affine = mk_index_non_affine();
		} else {
			IndexExpr ix;
			ix.kind  = IDX_AFFINE;
			ix.konst = (int)a.k;
			for (j = 0; j < stack.size(); j++) {
				std::map<std::string,long>::iterator it = a.co.find(stack[j]->index_var);
				long v = (it == a.co.end()) ? 0 : it->second;
				row[j] = v;
				if (v != 0)
					ix.coeffs[stack[j]->index_var] = (int)v;
			}
			s->idx[k].affine = ix;
		}
		r.F.push_back(row);
		r.c.push_back(a.ok ? a.k : 0);
	}
	cc->na->refs.push_back(r);
}

static void collect_body(CollectCtx *cc, std::vector<BodyItem> &body,
                         std::vector<LoopNest*> &stack)
{
	size_t i, k;
	for (i = 0; i < body.size(); i++) {
		BodyItem &it = body[i];
		if (it.kind == BK_LOOP) {
			cc->na->loops.push_back(it.loop);
			stack.push_back(it.loop);
			collect_body(cc, it.loop->body, stack);
			stack.pop_back();
		} else if (it.kind == BK_IF) {
			collect_body(cc, it.ifn->then_body, stack);
			collect_body(cc, it.ifn->else_body, stack);
		} else {
			IRStmt *s = it.stmt;
			cc->seq++;

			std::vector<std::string> uses;
			ir_stmt_uses(s, &uses);
			for (k = 0; k < uses.size(); k++)
				if (is_global_scalar(cc->p, uses[k]))
					add_scalar_ref(cc, s, uses[k], false, stack);

			if (s->kind == IR_ARRAY_LOAD)
				add_array_ref(cc, s, false, stack);
			if (s->kind == IR_ARRAY_STORE)
				add_array_ref(cc, s, true, stack);

			std::string d = ir_stmt_def(s);
			if (!d.empty() && is_global_scalar(cc->p, d))
				add_scalar_ref(cc, s, d, true, stack);
		}
	}
}

void affine_analyse(IRProgram *p, Kernel *k, LoopNest *root, NestAnalysis *out)
{
	std::vector<IRStmt*> all;
	std::map<std::string, int> count;
	size_t i;

	out->kernel = k;
	out->root = root;
	out->refs.clear();
	out->loops.clear();
	out->defs.clear();

	/* single-definition temps of the whole kernel, for def-chain resolution */
	ir_collect_stmts(k->body, &all);
	for (i = 0; i < all.size(); i++) {
		std::string d = ir_stmt_def(all[i]);
		if (is_temp_name(d))
			count[d]++;
	}
	for (i = 0; i < all.size(); i++) {
		std::string d = ir_stmt_def(all[i]);
		if (is_temp_name(d) && count[d] == 1)
			out->defs[d] = all[i];
	}

	CollectCtx cc;
	cc.p = p;
	cc.na = out;
	cc.seq = 0;

	std::vector<LoopNest*> stack;
	out->loops.push_back(root);
	stack.push_back(root);
	collect_body(&cc, root->body, stack);
}
