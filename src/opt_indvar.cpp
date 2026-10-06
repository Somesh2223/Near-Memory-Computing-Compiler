/* ============================================================================
 * opt_indvar.cpp  --  induction variable recognition
 *
 * Compiler-theory concept: induction variables (Aho/Lam/Sethi/Ullman 2nd ed.,
 * section 9.1.8 "Induction Variables and Reduction in Strength" and 9.6.7).
 *
 *   BASIC induction variable   a variable whose only definition in the loop is
 *                              i := i + c with c loop-invariant.  In NEMO-C the
 *                              loop index is one by construction: the for-header
 *                              fixes  i := lower, i += step.
 *   DERIVED induction variable a variable defined in the loop as a linear
 *                              function of a basic one,  d = a*i + b  (a, b
 *                              compile-time constants).  Its family is built by
 *                              closing under the rules
 *                                  d = i*c, c*i, i+c, c+i, i-c   -> (c,0) / (1,+-c)
 *                                  e = d*c, d+c, d-c, c+d, copy   -> (a*c, b*c) ...
 *
 * Only statements directly in the loop body (not under an if or an inner loop)
 * are considered: those execute exactly once per iteration, so the linear
 * relation really holds at every use.  The result feeds opt_strength.cpp and,
 * through IRProgram::ivs, the affine analysis of Phase D.
 * ==========================================================================*/
#include <cstdio>
#include <map>
#include <string>
#include <vector>

#include "ir.h"
#include "opt.h"

struct Lin {
	long a;
	long b;
	bool via_mul;
};

static bool lin_of_operand(std::map<std::string, Lin> &fam, const Operand &o,
                           bool *is_const, long *cval, Lin *lin)
{
	*is_const = false;
	if (o.kind == OPD_INT) {
		*is_const = true;
		*cval = o.ival;
		return true;
	}
	if (o.kind == OPD_NAME) {
		std::map<std::string, Lin>::iterator it = fam.find(o.name);
		if (it != fam.end()) {
			*lin = it->second;
			return true;
		}
	}
	return false;
}

void indvar_analyse_loop(LoopNest *l, LoopIVs *out)
{
	std::map<std::string, Lin> fam;
	size_t i;

	out->loop  = l;
	out->basic = l->index_var;
	out->init  = l->lower;
	out->step  = l->step;
	out->derived.clear();

	Lin basic;
	basic.a = 1;
	basic.b = 0;
	basic.via_mul = false;
	fam[l->index_var] = basic;

	for (i = 0; i < l->body.size(); i++) {
		if (l->body[i].kind != BK_STMT)
			continue;
		IRStmt *s = l->body[i].stmt;
		if (!is_temp_name(s->dst))
			continue;
		if (s->kind != IR_BINOP && s->kind != IR_COPY)
			continue;
		if (s->dst_type != TY_INT)
			continue;

		Lin r;
		bool ok = false;
		if (s->kind == IR_COPY) {
			bool c1; long v1; Lin l1;
			if (lin_of_operand(fam, s->src, &c1, &v1, &l1) && !c1) {
				r = l1;
				ok = true;
			}
		} else {
			bool c1, c2; long v1 = 0, v2 = 0; Lin l1, l2;
			bool k1 = lin_of_operand(fam, s->lhs, &c1, &v1, &l1);
			bool k2 = lin_of_operand(fam, s->rhs, &c2, &v2, &l2);
			if (k1 && k2 && c1 != c2) {
				/* exactly one operand is a constant, the other is in the family */
				Lin d = c1 ? l2 : l1;
				long c = c1 ? v1 : v2;
				bool const_left = c1;
				r = d;
				if (s->op == OP_ADD) {
					r.b = d.b + c;
					ok = true;
				} else if (s->op == OP_SUB) {
					if (const_left) { r.a = -d.a; r.b = c - d.b; }
					else            { r.b = d.b - c; }
					ok = true;
				} else if (s->op == OP_MUL) {
					r.a = d.a * c;
					r.b = d.b * c;
					r.via_mul = true;
					ok = true;
				}
			}
		}
		if (ok) {
			fam[s->dst] = r;
			DerivedIV dv;
			dv.name    = s->dst;
			dv.coeff   = r.a;
			dv.offset  = r.b;
			dv.stmt    = s;
			dv.via_mul = (s->kind == IR_BINOP && s->op == OP_MUL);
			out->derived.push_back(dv);
		}
	}
}

static void analyse_body(std::vector<BodyItem> &body, std::vector<LoopIVs> *out)
{
	size_t i;
	for (i = 0; i < body.size(); i++) {
		if (body[i].kind == BK_LOOP) {
			LoopIVs iv;
			indvar_analyse_loop(body[i].loop, &iv);
			out->push_back(iv);
			analyse_body(body[i].loop->body, out);
		} else if (body[i].kind == BK_IF) {
			analyse_body(body[i].ifn->then_body, out);
			analyse_body(body[i].ifn->else_body, out);
		}
	}
}

void opt_indvar_analyse(IRProgram *p, std::vector<LoopIVs> *out)
{
	size_t k;
	for (k = 0; k < p->kernels.size(); k++)
		analyse_body(p->kernels[k]->body, out);
	for (k = 0; k < p->hosts.size(); k++)
		analyse_body(p->hosts[k]->body, out);
}

int opt_indvar(IRProgram *p, int verbose)
{
	std::vector<LoopIVs> all;
	size_t i, j;
	int found = 0;

	opt_indvar_analyse(p, &all);
	for (i = 0; i < all.size(); i++) {
		LoopIVs &iv = all[i];
		found += (int)iv.derived.size();
		if (!verbose)
			continue;
		printf("  loop %s: basic IV %s  (init %ld, step %ld)\n",
		       iv.basic.c_str(), iv.basic.c_str(), iv.init, iv.step);
		if (iv.derived.empty())
			printf("      no derived induction variables\n");
		for (j = 0; j < iv.derived.size(); j++)
			printf("      derived IV %-6s = %ld*%s %c %ld     (%s)\n",
			       iv.derived[j].name.c_str(), iv.derived[j].coeff, iv.basic.c_str(),
			       iv.derived[j].offset < 0 ? '-' : '+',
			       iv.derived[j].offset < 0 ? -iv.derived[j].offset : iv.derived[j].offset,
			       ir_stmt_text(iv.derived[j].stmt).c_str());
	}
	return found;
}
