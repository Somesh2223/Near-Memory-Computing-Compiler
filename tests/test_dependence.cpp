/* GCD and Banerjee tests on cases whose answers were worked out by hand, then
 * whole-nest dependence/parallelism results on small programs. */
#include "testutil.h"
#include "frontend.h"
#include "analysis.h"
#include "opt.h"

static NestAnalysis *first_nest(IRProgram *p, const char *kernel)
{
	Kernel *k = ir_find_kernel(p, kernel);
	NestAnalysis *na = new NestAnalysis();
	size_t i;
	for (i = 0; i < k->body.size(); i++)
		if (k->body[i].kind == BK_LOOP) {
			analyse_nest(p, k, k->body[i].loop, na);
			return na;
		}
	return na;
}

int main(void)
{
	/* ---- single-variable tests, hand computed ------------------------------
	 * write A[a*i+c1], read A[b*i+c2], i in [0, 64) */
	/* A[2i] vs A[2i+1]: gcd(2,2)=2 does not divide 1 -> GCD proves independence */
	CHECK(!gcd_test_single(2, 2, 0, 1));
	/* ... and Banerjee cannot: 2i - 2i' = 1 is within the real range */
	CHECK(banerjee_test_single(2, 2, 0, 1, 0, 1, 64));

	/* A[i] vs A[i+100], i in [0,64): gcd(1,1)=1 divides 100 -> GCD inconclusive */
	CHECK(gcd_test_single(1, 1, 0, 100));
	/* i - i' = 100 but |i - i'| <= 63 -> Banerjee proves independence */
	CHECK(!banerjee_test_single(1, 1, 0, 100, 0, 1, 64));

	/* A[i] vs A[i+10], i in [0,64): both inconclusive -> a real dependence */
	CHECK(gcd_test_single(1, 1, 0, 10));
	CHECK(banerjee_test_single(1, 1, 0, 10, 0, 1, 64));

	/* A[i] vs A[i+63] fits exactly (i'=63? i - i' = 63 with i=63,i'=0) */
	CHECK(banerjee_test_single(1, 1, 0, 63, 0, 1, 64));
	CHECK(!banerjee_test_single(1, 1, 0, 64, 0, 1, 64));

	/* A[4i] vs A[6i+3]: gcd(4,6)=2 does not divide 3 */
	CHECK(!gcd_test_single(4, 6, 0, 3));
	/* A[3i] vs A[3i+6]: gcd 3 divides 6 */
	CHECK(gcd_test_single(3, 3, 0, 6));

	/* a loop with step 2 over [10, 30): A[i] vs A[i+1] -- i and i+1 never both
	 * even, but the tests only see i - i' = 1 with i-i' a multiple of 2 is NOT
	 * modelled by GCD on i-space; Banerjee on counters: 2n - 2n' = 1 + (1-1)*10 */
	CHECK(banerjee_test_single(1, 1, 0, 1, 10, 2, 10));   /* range [-18,18] contains 1 */

	/* ---- whole-nest results ---------------------------------------------- */
	IRProgram *p;
	NestAnalysis *na;

	/* elementwise: no dependences, loop parallel */
	p = frontend_compile_text(
	    "array float A[64]; array float B[64]; array float C[64];\n"
	    "kernel k() { for (i = 0; i < 64; i += 1) { C[i] = A[i] + B[i]; } }\n"
	    "host h() { call k(); }\n", 0);
	CHECK(p != 0);
	na = first_nest(p, "k");
	CHECK(na->analysable);
	CHECK_INT(na->deps.size(), 0);
	CHECK_INT(na->par[na->root], PAR_YES);

	/* recurrence A[i] = A[i-1] + 1 : flow dependence carried by i, distance 1 */
	p = frontend_compile_text(
	    "array float A[64];\n"
	    "kernel k() { for (i = 1; i < 64; i += 1) { A[i] = A[i - 1] + 1.0; } }\n"
	    "host h() { call k(); }\n", 0);
	na = first_nest(p, "k");
	CHECK_INT(na->par[na->root], PAR_NO);
	bool saw_flow = false;
	for (size_t i = 0; i < na->deps.size(); i++)
		if (na->deps[i].kind == DEP_FLOW && na->deps[i].level == 1 &&
		    na->deps[i].dir.size() == 1 && na->deps[i].dir[0] == DIR_LT)
			saw_flow = true;
	CHECK(saw_flow);

	/* A[i] = A[i+100] + 1 over 64 iterations: Banerjee proves no dependence */
	p = frontend_compile_text(
	    "array float A[256];\n"
	    "kernel k() { for (i = 0; i < 64; i += 1) { A[i] = A[i + 100] + 1.0; } }\n"
	    "host h() { call k(); }\n", 0);
	na = first_nest(p, "k");
	CHECK_INT(na->deps.size(), 0);
	CHECK_INT(na->par[na->root], PAR_YES);
	CHECK(na->pairs.size() >= 1);
	CHECK(na->pairs[0].gcd_maybe && !na->pairs[0].banerjee_maybe);

	/* A[2i] = ... , reads A[2i+1] : GCD proves independence */
	p = frontend_compile_text(
	    "array float A[256];\n"
	    "kernel k() { for (i = 0; i < 64; i += 1) { A[i * 2] = A[i * 2 + 1] + 1.0; } }\n"
	    "host h() { call k(); }\n", 0);
	na = first_nest(p, "k");
	CHECK_INT(na->deps.size(), 0);
	CHECK(na->pairs.size() >= 1 && !na->pairs[0].gcd_maybe);

	/* matmul: i and j parallel, k a reduction */
	p = frontend_compile_text(
	    "array float A[8,8]; array float B[8,8]; array float C[8,8];\n"
	    "kernel mm() { for (i = 0; i < 8; i += 1) { for (j = 0; j < 8; j += 1) {\n"
	    "  for (k = 0; k < 8; k += 1) { C[i,j] = C[i,j] + A[i,k] * B[k,j]; } } } }\n"
	    "host h() { call mm(); }\n", 0);
	na = first_nest(p, "mm");
	CHECK_INT(na->loops.size(), 3);
	CHECK_INT(na->par[na->loops[0]], PAR_YES);
	CHECK_INT(na->par[na->loops[1]], PAR_YES);
	CHECK_INT(na->par[na->loops[2]], PAR_REDUCTION);
	CHECK_INT(na->reductions.size(), 1);

	/* indirect access is not analysable */
	p = frontend_compile_text(
	    "array float A[64]; array int B[64]; array float C[64];\n"
	    "kernel k() { for (i = 0; i < 64; i += 1) { C[i] = A[B[i]]; } }\n"
	    "host h() { call k(); }\n", 0);
	na = first_nest(p, "k");
	CHECK(!na->analysable);

	TEST_DONE();
}
