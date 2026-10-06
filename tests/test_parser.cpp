/* Parser: ASTs on valid input (precedence, structure) and recovery on invalid. */
#include "testutil.h"
#include "frontend.h"

static Item *kernel_of(Program *p)
{
	for (size_t i = 0; i < p->items.size(); i++)
		if (p->items[i]->kind == IK_KERNEL)
			return p->items[i];
	return 0;
}

int main(void)
{
	int ne = 0;
	Program *p;

	/* structure of a small program */
	p = frontend_parse_text(
	    "array float A[8, 4]; scalar int n;\n"
	    "kernel k() { A[1, 2] = 3.0; }\n"
	    "host h() { call k(); print A[0, 0]; }\n", &ne);
	CHECK(p != 0);
	CHECK_INT(ne, 0);
	CHECK_INT(p->items.size(), 4);
	CHECK_INT(p->items[0]->decl->dims.size(), 2);
	CHECK_INT(p->items[0]->decl->dims[0], 8);
	CHECK_INT(p->items[0]->decl->dims[1], 4);
	CHECK_INT(p->items[1]->decl->kind, DK_SCALAR);

	/* precedence: a + b * c  parses as  a + (b * c) */
	p = frontend_parse_text("scalar int a; scalar int b; scalar int c; scalar int x;\n"
	                        "host h() { x = a + b * c; }\n", &ne);
	CHECK(p != 0);
	Stmt *s = p->items[4]->body[0];
	CHECK_INT(s->kind, SK_ASSIGN);
	CHECK_INT(s->value->kind, EK_BINOP);
	CHECK_INT(s->value->op, OP_ADD);
	CHECK_INT(s->value->rhs->kind, EK_BINOP);
	CHECK_INT(s->value->rhs->op, OP_MUL);

	/* left associativity: a - b - c  is  (a - b) - c */
	p = frontend_parse_text("scalar int a; scalar int b; scalar int c; scalar int x;\n"
	                        "host h() { x = a - b - c; }\n", &ne);
	s = p->items[4]->body[0];
	CHECK_INT(s->value->op, OP_SUB);
	CHECK_INT(s->value->lhs->kind, EK_BINOP);
	CHECK_INT(s->value->rhs->kind, EK_IDENT);

	/* unary minus binds tighter than multiplication */
	p = frontend_parse_text("scalar int a; scalar int b; scalar int x;\n"
	                        "host h() { x = -a * b; }\n", &ne);
	s = p->items[3]->body[0];
	CHECK_INT(s->value->op, OP_MUL);
	CHECK_INT(s->value->lhs->kind, EK_UNOP);

	/* if / else nesting: the else belongs to the inner if */
	p = frontend_parse_text("scalar int a; scalar int x;\n"
	                        "host h() { if (a > 0) { if (a > 1) { x = 1; } else { x = 2; } } }\n", &ne);
	CHECK(p != 0);
	Stmt *outer = p->items[2]->body[0];
	CHECK(!outer->has_else);
	CHECK(outer->then_body[0]->has_else);

	/* a for loop keeps its three header names and bounds */
	p = frontend_parse_text("array float A[10];\n"
	                        "kernel k() { for (i = 2; i <= 8; i += 3) { A[i] = 1.0; } }\n", &ne);
	Item *k = kernel_of(p);
	CHECK_INT(k->body[0]->kind, SK_FOR);
	CHECK_INT(k->body[0]->step, 3);
	CHECK_INT(k->body[0]->rel, OP_LE);
	CHECK_STR(k->body[0]->ivar_test, "i");

	/* syntax errors: both reported, parser recovers and keeps going */
	p = frontend_parse_text("array float A[4];\n"
	                        "kernel k() {\n"
	                        "  A[0] = 1.0 * * 2.0;\n"
	                        "  A[1] = ;\n"
	                        "  A[2] = 3.0;\n"
	                        "}\n", &ne);
	CHECK(p == 0);
	CHECK_INT(ne, 2);

	/* a missing closing brace is an error */
	p = frontend_parse_text("kernel k() { \n", &ne);
	CHECK(p == 0);
	CHECK(ne >= 1);

	/* an else without an if is an error */
	p = frontend_parse_text("host h() { else { } }\n", &ne);
	CHECK(p == 0);

	TEST_DONE();
}
