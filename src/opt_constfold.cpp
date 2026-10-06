/* ============================================================================
 * opt_constfold.cpp  --  constant propagation and constant folding
 *
 * Compiler-theory concept: constant propagation as a FORWARD dataflow analysis
 * over the control-flow graph (Aho/Lam/Sethi/Ullman 2nd ed., section 9.4,
 * "Constant Propagation"; Kildall 1973).
 *
 *   lattice   per variable:   UNDEF  (top: no definition seen yet)
 *                             CONST c (exactly one known value)
 *                             NAC     (bottom: "not a constant")
 *   meet      UNDEF meet x = x;  CONST c meet CONST c = CONST c;  anything else = NAC
 *   transfer  x = literal            -> x is CONST
 *             x = a op b, a,b CONST  -> x is CONST (folded)
 *             x = a op b, otherwise  -> x is NAC;  x = A[..] -> x is NAC
 *             call k()               -> every global scalar becomes NAC
 *             loop header            -> the loop index becomes NAC
 *
 * The analysis iterates over the blocks until no IN-state changes (a monotone
 * framework over a finite-height lattice, so it terminates).  Then a rewrite
 * walk replaces each use of a CONST variable by its literal (propagation) and
 * turns an operation whose operands are all literals into a copy of its result
 * (folding).  A condition that folds to a literal removes the dead branch.
 *
 * Arithmetic uses eval_binop() from ir.cpp -- the very function the interpreter
 * runs -- so a folded result is identical to the executed one by construction.
 * ==========================================================================*/
#include <cstdio>
#include <map>
#include <string>
#include <vector>

#include "ir.h"
#include "cfg.h"
#include "opt.h"

enum CKind { C_UNDEF, C_CONST, C_NAC };

struct CVal {
	CKind kind;
	Value v;
};

typedef std::map<std::string, CVal> CState;

static CVal cval_undef(void)
{
	CVal c;
	c.kind = C_UNDEF;
	c.v = mk_val_int(0);
	return c;
}

static CVal cval_nac(void)
{
	CVal c;
	c.kind = C_NAC;
	c.v = mk_val_int(0);
	return c;
}

static CVal cval_const(Value v)
{
	CVal c;
	c.kind = C_CONST;
	c.v = v;
	return c;
}

static CVal cstate_get(CState &s, const std::string &n)
{
	CState::iterator it = s.find(n);
	if (it == s.end())
		return cval_undef();
	return it->second;
}

static CVal cval_meet(CVal a, CVal b)
{
	if (a.kind == C_UNDEF) return b;
	if (b.kind == C_UNDEF) return a;
	if (a.kind == C_NAC || b.kind == C_NAC) return cval_nac();
	if (value_equal_bits(a.v, b.v)) return a;
	return cval_nac();
}

static bool cval_same(CVal a, CVal b)
{
	if (a.kind != b.kind) return false;
	if (a.kind == C_CONST) return value_equal_bits(a.v, b.v);
	return true;
}

/* abstract value of an operand under a state; literals are always constant */
static CVal opd_cval(CState &s, const Operand &o)
{
	if (o.kind == OPD_NAME)
		return cstate_get(s, o.name);
	return cval_const(value_of_operand(o));
}

static Operand opd_from_value(Value v)
{
	if (v.type == TY_FLOAT)
		return mk_opd_float((double)v.f);
	return mk_opd_int(v.i);
}

/* ---- transfer function for one statement ---------------------------------- */
static void transfer_stmt(IRProgram *p, IRStmt *s, CState &st)
{
	switch (s->kind) {
	case IR_COPY: {
		CVal a = opd_cval(st, s->src);
		if (a.kind == C_CONST)
			st[s->dst] = cval_const(value_convert(a.v, s->dst_type));
		else if (a.kind == C_NAC)
			st[s->dst] = cval_nac();
		/* UNDEF source: leave the destination as it was (optimistic) */
		break;
	}
	case IR_BINOP: {
		CVal a = opd_cval(st, s->lhs);
		CVal b = opd_cval(st, s->rhs);
		if (a.kind == C_CONST && b.kind == C_CONST)
			st[s->dst] = cval_const(value_convert(eval_binop(s->op, a.v, b.v),
			                                       s->dst_type));
		else if (a.kind == C_NAC || b.kind == C_NAC)
			st[s->dst] = cval_nac();
		break;
	}
	case IR_ARRAY_LOAD:
		st[s->dst] = cval_nac();
		break;
	case IR_CALL: {
		std::map<std::string, TypeTag>::iterator it;
		for (it = p->scalars.begin(); it != p->scalars.end(); it++)
			st[it->first] = cval_nac();
		break;
	}
	default:
		break;
	}
}

static void transfer_block(IRProgram *p, BasicBlock *bb, CState &st)
{
	size_t i;
	if (bb->loop_header != 0)
		st[bb->loop_header->index_var] = cval_nac();
	for (i = 0; i < bb->stmts.size(); i++)
		transfer_stmt(p, bb->stmts[i], st);
}

static CState state_meet(CState &a, CState &b)
{
	CState r = a;
	CState::iterator it;
	for (it = b.begin(); it != b.end(); it++)
		r[it->first] = cval_meet(cstate_get(a, it->first), it->second);
	return r;
}

static bool state_same(CState &a, CState &b)
{
	CState::iterator it;
	for (it = a.begin(); it != a.end(); it++)
		if (!cval_same(it->second, cstate_get(b, it->first)))
			return false;
	for (it = b.begin(); it != b.end(); it++)
		if (!cval_same(it->second, cstate_get(a, it->first)))
			return false;
	return true;
}

/* ---- rewrite one operand: propagate a known constant ------------------------ */
static int rewrite_operand(CState &st, Operand *o)
{
	if (o->kind != OPD_NAME)
		return 0;
	CVal c = cstate_get(st, o->name);
	if (c.kind != C_CONST)
		return 0;
	Value v = c.v;
	/* keep the operand's declared type: a float variable holding 3 reads as 3.0 */
	v = value_convert(v, o->type);
	*o = opd_from_value(v);
	return 1;
}

static int rewrite_block(IRProgram *p, BasicBlock *bb, CState st)
{
	int changes = 0;
	size_t i, k;

	if (bb->loop_header != 0)
		st[bb->loop_header->index_var] = cval_nac();

	for (i = 0; i < bb->stmts.size(); i++) {
		IRStmt *s = bb->stmts[i];

		switch (s->kind) {
		case IR_ARRAY_LOAD:
		case IR_ARRAY_STORE:
			for (k = 0; k < s->idx.size(); k++)
				changes += rewrite_operand(st, &s->idx[k].value);
			if (s->kind == IR_ARRAY_STORE)
				changes += rewrite_operand(st, &s->src);
			break;
		case IR_BINOP:
			changes += rewrite_operand(st, &s->lhs);
			changes += rewrite_operand(st, &s->rhs);
			break;
		case IR_COPY:
		case IR_PRINT:
			changes += rewrite_operand(st, &s->src);
			break;
		default:
			break;
		}

		/* fold: a binop with two literal operands becomes a copy of the result */
		if (s->kind == IR_BINOP && s->lhs.kind != OPD_NAME && s->rhs.kind != OPD_NAME) {
			Value r = value_convert(eval_binop(s->op, value_of_operand(s->lhs),
			                                   value_of_operand(s->rhs)),
			                        s->dst_type);
			s->kind = IR_COPY;
			s->src  = opd_from_value(r);
			changes++;
		}

		transfer_stmt(p, s, st);
	}

	if (bb->branch != 0)
		changes += rewrite_operand(st, &bb->branch->cond);
	return changes;
}

/* ---- remove a branch whose condition folded to a literal ---------------------- */
static int fold_branches(std::vector<BodyItem> &body)
{
	int changes = 0;
	std::vector<BodyItem> out;
	size_t i, j;

	for (i = 0; i < body.size(); i++) {
		BodyItem it = body[i];
		if (it.kind == BK_LOOP) {
			changes += fold_branches(it.loop->body);
			out.push_back(it);
		} else if (it.kind == BK_IF) {
			changes += fold_branches(it.ifn->then_body);
			changes += fold_branches(it.ifn->else_body);
			if (it.ifn->cond.kind != OPD_NAME) {
				bool taken = value_of_operand(it.ifn->cond).type == TY_INT
				                 ? value_of_operand(it.ifn->cond).i != 0
				                 : value_of_operand(it.ifn->cond).f != 0.0f;
				std::vector<BodyItem> &chosen =
				    taken ? it.ifn->then_body : it.ifn->else_body;
				for (j = 0; j < chosen.size(); j++)
					out.push_back(chosen[j]);
				changes++;
			} else {
				out.push_back(it);
			}
		} else {
			out.push_back(it);
		}
	}
	body = out;
	return changes;
}

/* ---- one kernel / host body ---------------------------------------------------- */
static int constfold_kernel(IRProgram *p, Kernel *k, int verbose)
{
	CFG *g = build_cfg(k);
	size_t n = g->blocks.size();
	std::vector<CState> in(n), out(n);
	std::vector<bool>   reached(n, false);
	bool changed = true;
	size_t b, j;

	CState entry;
	std::map<std::string, TypeTag>::iterator sc;
	for (sc = p->scalars.begin(); sc != p->scalars.end(); sc++)
		entry[sc->first] = cval_nac();       /* a global scalar's value is unknown on entry */

	reached[g->entry] = true;
	while (changed) {
		changed = false;
		for (b = 0; b < n; b++) {
			BasicBlock *bb = g->blocks[b];
			CState new_in;
			bool have = false;

			if ((int)b == g->entry) {
				new_in = entry;
				have = true;
			}
			for (j = 0; j < bb->pred.size(); j++) {
				int pr = bb->pred[j];
				if (!reached[pr])
					continue;
				new_in = have ? state_meet(new_in, out[pr]) : out[pr];
				have = true;
			}
			if (!have)
				continue;

			CState new_out = new_in;
			transfer_block(p, bb, new_out);
			if (!reached[b] || !state_same(new_in, in[b]) || !state_same(new_out, out[b])) {
				in[b] = new_in;
				out[b] = new_out;
				reached[b] = true;
				changed = true;
			}
		}
	}

	if (verbose) {
		printf("  [%s %s] constants known on entry to each block:\n",
		       k->is_host ? "host" : "kernel", k->name.c_str());
		for (b = 0; b < n; b++) {
			CState::iterator it;
			printf("    B%-2d %-16s {", (int)b, g->blocks[b]->label.c_str());
			int first = 1;
			for (it = in[b].begin(); it != in[b].end(); it++) {
				if (it->second.kind != C_CONST)
					continue;
				printf("%s%s=%s", first ? "" : ", ", it->first.c_str(),
				       value_text(it->second.v).c_str());
				first = 0;
			}
			printf("}\n");
		}
	}

	int changes = 0;
	for (b = 0; b < n; b++)
		if (reached[b])
			changes += rewrite_block(p, g->blocks[b], in[b]);
	changes += fold_branches(k->body);
	return changes;
}

int opt_constfold(IRProgram *p, int verbose)
{
	int total = 0;
	size_t k;
	for (k = 0; k < p->kernels.size(); k++)
		total += constfold_kernel(p, p->kernels[k], verbose);
	for (k = 0; k < p->hosts.size(); k++)
		total += constfold_kernel(p, p->hosts[k], verbose);
	return total;
}
