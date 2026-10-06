/* ============================================================================
 * opt_strength.cpp  --  strength reduction of induction-variable multiplies
 *
 * Compiler-theory concept: strength reduction (Aho/Lam/Sethi/Ullman 2nd ed.,
 * section 9.1.8 and 9.6.8, "Strength Reduction on Induction Variables").  A
 * derived induction variable  t = a*i + b  defined by a multiplication can be
 * maintained by an addition instead: keep an accumulator s that always equals
 * a*i + b at the top of an iteration,
 *
 *      before the loop :  s = a*lower + b
 *      in the body     :  t = s                (the multiply disappears)
 *      end of the body :  s = s + a*step       (one add per iteration)
 *
 * which is correct because i advances by exactly `step` each iteration.  On a
 * DPU, where a 32-bit multiply is a shift-add sequence (PIM_MUL_CYCLES) and an
 * add is one operation, this matters.
 *
 * opt_indvar.cpp supplies the (a, b) pair for each candidate.  Each accumulator
 * is recorded in IRProgram::ivs so the affine analysis of Phase D can still see
 * that the subscript is a*i + b even though it is now computed incrementally.
 * ==========================================================================*/
#include <cstdio>
#include <string>
#include <vector>

#include "ir.h"
#include "opt.h"

static int g_sr_counter = 0;

static int sr_body(IRProgram *p, std::vector<BodyItem> &body, int verbose);

static int sr_loop(IRProgram *p, LoopNest *l, std::vector<BodyItem> *inits, int verbose)
{
	int changes = sr_body(p, l->body, verbose);    /* inner loops first */

	LoopIVs iv;
	size_t i;
	indvar_analyse_loop(l, &iv);

	for (i = 0; i < iv.derived.size(); i++) {
		DerivedIV &d = iv.derived[i];
		if (!d.via_mul || d.coeff == 0 || d.coeff == 1)
			continue;

		char nm[32];
		snprintf(nm, sizeof nm, "%%sr%d", g_sr_counter++);
		std::string sr = nm;
		Loc loc = d.stmt->loc;

		/* before the loop: s = a*lower + b */
		inits->push_back(mk_bi_stmt(mk_copy(sr, mk_opd_int(d.coeff * l->lower + d.offset),
		                                     TY_INT, loc)));

		/* in place of the multiply: t = s */
		if (verbose)
			printf("    loop %s: `%s` becomes `%s = %s`, update `%s = %s + %ld`\n",
			       l->index_var.c_str(), ir_stmt_text(d.stmt).c_str(),
			       d.name.c_str(), sr.c_str(), sr.c_str(), sr.c_str(),
			       d.coeff * l->step);
		d.stmt->kind = IR_COPY;
		d.stmt->src  = mk_opd_name(sr, TY_INT);

		/* end of the body: s = s + a*step */
		l->body.push_back(mk_bi_stmt(mk_binop_stmt(sr, OP_ADD, mk_opd_name(sr, TY_INT),
		                                           mk_opd_int(d.coeff * l->step), loc)));

		IVInfo info;
		info.basic  = l->index_var;
		info.coeff  = d.coeff;
		info.offset = d.offset;
		p->ivs[sr]  = info;
		changes++;
	}
	return changes;
}

static int sr_body(IRProgram *p, std::vector<BodyItem> &body, int verbose)
{
	std::vector<BodyItem> out;
	int changes = 0;
	size_t i;

	for (i = 0; i < body.size(); i++) {
		BodyItem it = body[i];
		if (it.kind == BK_LOOP) {
			std::vector<BodyItem> inits;
			changes += sr_loop(p, it.loop, &inits, verbose);
			size_t j;
			for (j = 0; j < inits.size(); j++)
				out.push_back(inits[j]);
			out.push_back(it);
		} else if (it.kind == BK_IF) {
			changes += sr_body(p, it.ifn->then_body, verbose);
			changes += sr_body(p, it.ifn->else_body, verbose);
			out.push_back(it);
		} else {
			out.push_back(it);
		}
	}
	body = out;
	return changes;
}

int opt_strength(IRProgram *p, int verbose)
{
	int total = 0;
	size_t k;
	for (k = 0; k < p->kernels.size(); k++)
		total += sr_body(p, p->kernels[k]->body, verbose);
	for (k = 0; k < p->hosts.size(); k++)
		total += sr_body(p, p->hosts[k]->body, verbose);
	return total;
}
