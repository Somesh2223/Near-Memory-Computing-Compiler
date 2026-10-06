/* Classical optimisations: before / after of each pass on small programs. */
#include "testutil.h"
#include <vector>
#include "frontend.h"
#include "opt.h"

static std::vector<std::string> texts(IRProgram *p, const char *kernel)
{
	std::vector<IRStmt*> all;
	std::vector<std::string> out;
	Kernel *k = ir_find_kernel(p, kernel);
	ir_collect_stmts(k->body, &all);
	for (size_t i = 0; i < all.size(); i++)
		out.push_back(ir_stmt_text(all[i]));
	return out;
}

static bool has(const std::vector<std::string> &v, const std::string &s)
{
	for (size_t i = 0; i < v.size(); i++)
		if (v[i] == s)
			return true;
	return false;
}

static int count_kind(IRProgram *p, const char *kernel, int kind)
{
	std::vector<IRStmt*> all;
	int n = 0;
	ir_collect_stmts(ir_find_kernel(p, kernel)->body, &all);
	for (size_t i = 0; i < all.size(); i++)
		if (all[i]->kind == kind)
			n++;
	return n;
}

static int count_loops_and_ifs(std::vector<BodyItem> &b, int kind)
{
	int n = 0;
	for (size_t i = 0; i < b.size(); i++) {
		if (b[i].kind == kind)
			n++;
		if (b[i].kind == BK_IF) {
			n += count_loops_and_ifs(b[i].ifn->then_body, kind);
			n += count_loops_and_ifs(b[i].ifn->else_body, kind);
		} else if (b[i].kind == BK_LOOP) {
			n += count_loops_and_ifs(b[i].loop->body, kind);
		}
	}
	return n;
}

int main(void)
{
	IRProgram *p;

	/* ---- constant folding and propagation -------------------------------- */
	p = frontend_compile_text("scalar int n;\n"
	                          "kernel k() { n = 3 * 4; n = n + 1; }\n"
	                          "host h() { call k(); }\n", 0);
	CHECK_INT(count_kind(p, "k", IR_BINOP), 2);
	opt_constfold(p, 0);
	CHECK_INT(count_kind(p, "k", IR_BINOP), 0);        /* every operation folded */
	CHECK(has(texts(p, "k"), "n = 12"));
	CHECK(has(texts(p, "k"), "n = 13"));

	/* a value changed in a loop is not a constant */
	p = frontend_compile_text("scalar int s;\n"
	                          "kernel k() { s = 0; for (i = 0; i < 4; i += 1) { s = s + 1; } }\n"
	                          "host h() { call k(); }\n", 0);
	opt_constfold(p, 0);
	CHECK_INT(count_kind(p, "k", IR_BINOP), 1);        /* s + 1 survives */

	/* a branch with a constant condition disappears */
	p = frontend_compile_text("scalar int x;\n"
	                          "host h() { if (2 > 1) { x = 1; } else { x = 2; } }\n", 0);
	CHECK_INT(count_loops_and_ifs(ir_find_kernel(p, "h")->body, BK_IF), 1);
	opt_constfold(p, 0);
	CHECK_INT(count_loops_and_ifs(ir_find_kernel(p, "h")->body, BK_IF), 0);
	CHECK(has(texts(p, "h"), "x = 1"));
	CHECK(!has(texts(p, "h"), "x = 2"));

	/* the folded float result matches the interpreter's single-precision arithmetic */
	p = frontend_compile_text("scalar float f;\n"
	                          "host h() { f = 0.1 + 0.2; }\n", 0);
	opt_constfold(p, 0);
	Value want = eval_binop(OP_ADD, mk_val_float(0.1f), mk_val_float(0.2f));
	CHECK(has(texts(p, "h"), "f = " + value_text(want)));

	/* ---- common subexpression elimination --------------------------------- */
	p = frontend_compile_text("array float A[8]; array float D[8];\n"
	                          "kernel k() { for (i = 0; i < 8; i += 1) { D[i] = A[i] * A[i] + A[i]; } }\n"
	                          "host h() { call k(); }\n", 0);
	CHECK_INT(count_kind(p, "k", IR_ARRAY_LOAD), 3);
	opt_cse(p, 0);
	opt_dce(p, 0);
	CHECK_INT(count_kind(p, "k", IR_ARRAY_LOAD), 1);

	/* a store between two loads of the same element blocks the elimination */
	p = frontend_compile_text("array float A[8]; scalar float x; scalar float y;\n"
	                          "kernel k() { x = A[0]; A[0] = 5.0; y = A[0]; }\n"
	                          "host h() { call k(); }\n", 0);
	opt_cse(p, 0);
	CHECK_INT(count_kind(p, "k", IR_ARRAY_LOAD), 2);

	/* commutativity: a+b and b+a are the same expression */
	p = frontend_compile_text("scalar int a; scalar int b; scalar int x; scalar int y;\n"
	                          "kernel k() { x = a + b; y = b + a; }\n"
	                          "host h() { call k(); }\n", 0);
	opt_cse(p, 0);
	opt_dce(p, 0);
	CHECK_INT(count_kind(p, "k", IR_BINOP), 1);

	/* ---- dead-code elimination --------------------------------------------- */
	p = frontend_compile_text("scalar float s;\n"
	                          "kernel k() { s = 1.0; s = 2.5; }\n"
	                          "host h() { call k(); }\n", 0);
	CHECK_INT(count_kind(p, "k", IR_COPY), 2);
	opt_dce(p, 0);
	CHECK_INT(count_kind(p, "k", IR_COPY), 1);
	CHECK(has(texts(p, "k"), "s = 2.5"));

	/* a dead load disappears, its dependent dead store to a scalar too */
	p = frontend_compile_text("array float A[4]; scalar float x;\n"
	                          "kernel k() { x = A[0]; x = 2.0; }\n"
	                          "host h() { call k(); }\n", 0);
	opt_dce(p, 0);
	CHECK_INT(count_kind(p, "k", IR_ARRAY_LOAD), 0);

	/* stores and calls are never removed */
	p = frontend_compile_text("array float A[4];\n"
	                          "kernel k() { A[0] = 1.0; A[0] = 2.0; }\n"
	                          "host h() { call k(); }\n", 0);
	opt_dce(p, 0);
	CHECK_INT(count_kind(p, "k", IR_ARRAY_STORE), 2);

	/* ---- induction variables and strength reduction ---------------------------- */
	p = frontend_compile_text("array int X[64];\n"
	                          "kernel k() { for (i = 0; i < 8; i += 1) { X[i * 4 + 1] = i; } }\n"
	                          "host h() { call k(); }\n", 0);
	std::vector<LoopIVs> ivs;
	opt_indvar_analyse(p, &ivs);
	bool found = false;
	for (size_t i = 0; i < ivs.size(); i++)
		for (size_t j = 0; j < ivs[i].derived.size(); j++)
			if (ivs[i].derived[j].coeff == 4 && ivs[i].derived[j].offset == 1)
				found = true;
	CHECK(found);

	int muls_before = 0, muls_after = 0;
	{
		std::vector<IRStmt*> all;
		ir_collect_stmts(ir_find_kernel(p, "k")->body, &all);
		for (size_t i = 0; i < all.size(); i++)
			if (all[i]->kind == IR_BINOP && all[i]->op == OP_MUL)
				muls_before++;
	}
	CHECK_INT(opt_strength(p, 0), 1);
	{
		std::vector<IRStmt*> all;
		ir_collect_stmts(ir_find_kernel(p, "k")->body, &all);
		for (size_t i = 0; i < all.size(); i++)
			if (all[i]->kind == IR_BINOP && all[i]->op == OP_MUL)
				muls_after++;
	}
	CHECK_INT(muls_before, 1);
	CHECK_INT(muls_after, 0);
	CHECK_INT(p->ivs.size(), 1);

	TEST_DONE();
}
