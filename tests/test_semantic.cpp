/* Semantic analysis: one program per rule, counting the diagnostics. */
#include "testutil.h"
#include "frontend.h"
#include "semantic.h"

static int errors_of(const char *text)
{
	int ne = 0;
	Program *p = frontend_parse_text(text, &ne);
	if (!p)
		return -1;
	return semantic_check(p);
}

int main(void)
{
	/* a clean program */
	CHECK_INT(errors_of("array float A[4]; scalar float a;\n"
	                    "kernel k() { for (i = 0; i < 4; i += 1) { A[i] = a * i; } }\n"
	                    "host h() { call k(); print A[0]; }\n"), 0);

	/* undeclared identifier (read and written) */
	CHECK_INT(errors_of("array float A[4];\n"
	                    "kernel k() { A[0] = z; }\n"), 1);
	CHECK_INT(errors_of("host h() { q = 1; }\n"), 1);

	/* wrong number of subscripts, both ways */
	CHECK_INT(errors_of("array float A[4, 4];\n"
	                    "kernel k() { A[1] = 1.0; }\n"), 1);
	CHECK_INT(errors_of("array float A[4]; scalar float x;\n"
	                    "kernel k() { x = A[1, 2]; }\n"), 1);

	/* array used without a subscript */
	CHECK_INT(errors_of("array float A[4]; scalar float x;\n"
	                    "kernel k() { x = A; }\n"), 1);

	/* float cannot be assigned to int; int promotes to float */
	CHECK_INT(errors_of("scalar int n; scalar float f;\n"
	                    "host h() { n = f; }\n"), 1);
	CHECK_INT(errors_of("scalar int n; scalar float f;\n"
	                    "host h() { f = n; }\n"), 0);

	/* modulo needs integers; a float subscript is rejected */
	CHECK_INT(errors_of("scalar float f; scalar float g;\n"
	                    "host h() { g = f % 2; }\n"), 1);
	CHECK_INT(errors_of("array float A[4]; scalar float f;\n"
	                    "kernel k() { A[f] = 1.0; }\n"), 1);

	/* loop bounds must be compile-time integer constants */
	CHECK_INT(errors_of("scalar int n; array float A[4];\n"
	                    "kernel k() { for (i = 0; i < n; i += 1) { A[i] = 1.0; } }\n"), 1);
	CHECK_INT(errors_of("array float A[8];\n"
	                    "kernel k() { for (i = 0; i < 2 * 4; i += 1) { A[i] = 1.0; } }\n"), 0);

	/* the loop index may not be reassigned, nor shadowed by a nested loop */
	CHECK_INT(errors_of("array float A[4];\n"
	                    "kernel k() { for (i = 0; i < 4; i += 1) { i = 2; } }\n"), 1);
	CHECK_INT(errors_of("array float A[4];\n"
	                    "kernel k() { for (i = 0; i < 4; i += 1) { for (i = 0; i < 2; i += 1) { A[i] = 1.0; } } }\n"), 1);

	/* the three header slots must name one variable; step positive; test less-than */
	CHECK_INT(errors_of("array float A[4];\n"
	                    "kernel k() { for (i = 0; j < 4; i += 1) { A[i] = 1.0; } }\n"), 1);
	CHECK_INT(errors_of("array float A[4];\n"
	                    "kernel k() { for (i = 0; i < 4; i += 0) { A[i] = 1.0; } }\n"), 1);
	CHECK_INT(errors_of("array float A[4];\n"
	                    "kernel k() { for (i = 0; i > 4; i += 1) { A[i] = 1.0; } }\n"), 1);

	/* the loop index is out of scope after its loop */
	CHECK_INT(errors_of("array float A[4]; scalar int s;\n"
	                    "kernel k() { for (i = 0; i < 4; i += 1) { A[i] = 1.0; } s = i; }\n"), 1);

	/* call and print are host-only; a call must name a kernel */
	CHECK_INT(errors_of("kernel a() { } kernel k() { call a(); }\n"), 1);
	CHECK_INT(errors_of("array float A[2];\n"
	                    "kernel k() { print A[0]; }\n"), 1);
	CHECK_INT(errors_of("host h() { call nothere(); }\n"), 1);
	CHECK_INT(errors_of("host h() { } host g() { call h(); }\n"), 1);

	/* redeclaration, non-positive extent */
	CHECK_INT(errors_of("scalar int a; scalar int a;\n"), 1);
	CHECK_INT(errors_of("array float A[0];\n"), 1);

	/* several errors in one run are all reported */
	CHECK_INT(errors_of("array float A[4];\n"
	                    "kernel k() { A[0] = u; A[1, 2] = 1.0; print A[0]; }\n"), 3);

	TEST_DONE();
}
