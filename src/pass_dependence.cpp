/* ============================================================================
 * pass_dependence.cpp  --  data-dependence analysis for loop nests
 *
 * Compiler-theory concept: array data-dependence testing with the GCD test and
 * the Banerjee bounds test, summarised by direction vectors (Aho/Lam/Sethi/
 * Ullman 2nd ed., sections 11.6-11.7; Banerjee 1988; Allen & Kennedy 1987).
 *
 * Two references R1 = F1*i + c1 and R2 = F2*i' + c2 to the same array touch the
 * same element when  F1*i + c1 = F2*i' + c2  for some iterations i, i' inside
 * the loop bounds.  One linear equation per array dimension:
 *
 *     sum_j a_j*i_j  -  sum_j b_j*i'_j  =  c2 - c1
 *
 *   GCD test       an integer solution exists only if gcd(all a_j, b_j) divides
 *                  c2 - c1.  Cheap, ignores the loop bounds entirely.
 *   Banerjee test  ignores integrality but USES the bounds: the left-hand side
 *                  is a linear function over a box (or, with a direction
 *                  constraint, a convex polygon), so its minimum and maximum
 *                  are attained at vertices; the equation is solvable only if
 *                  c2 - c1 lies between them.  Each test can prove independence
 *                  the other cannot -- both verdicts are reported separately.
 *
 * DIRECTION VECTORS.  For each loop level shared by both references the tested
 * relation between the source iteration i_k and the sink iteration i'_k is one
 * of  <  (source earlier),  =  (same iteration),  >  (source later).  All 3^m
 * vectors are enumerated and each is tested; a vector survives only if both
 * tests allow it.  A dependence is oriented so its leading non-'=' entry is '<'
 * (source executes first); a vector of all '=' is a loop-independent
 * dependence ordered by position within the iteration.  A dependence whose
 * first '<' is at common loop k is CARRIED by that loop, and a loop is
 * PARALLEL exactly when it carries no dependence.
 *
 * Each dependence is classified FLOW (write then read), ANTI (read then write)
 * or OUTPUT (write then write).  References to non-affine subscripts cannot be
 * tested: they are assumed to depend on everything, and make the nest "not
 * analysable" unless they form a recognised histogram update.
 *
 * REDUCTIONS.  s = s + e, s = s * e, the conditional max/min idiom, array
 * accumulations X[f(i)] = X[f(i)] + e and histogram updates H[A[i]] += e are
 * recognised syntactically.  Dependences between the two references of one
 * reduction are flagged `reduction`: the loops they are carried by can still
 * run in parallel with per-unit private accumulators merged afterwards.
 * ==========================================================================*/
#include <cstdio>
#include <cstdlib>
#include <map>
#include <set>
#include <string>
#include <vector>

#include "ir.h"
#include "analysis.h"

const char *dir_char(int d)
{
	return d == DIR_LT ? "<" : (d == DIR_EQ ? "=" : ">");
}

const char *par_text(int p)
{
	return p == PAR_YES ? "parallel" : (p == PAR_REDUCTION ? "parallel (reduction)" : "serial");
}

const char *red_text(int k)
{
	switch (k) {
	case RED_SUM:  return "sum";
	case RED_PROD: return "product";
	case RED_MAX:  return "max";
	case RED_MIN:  return "min";
	default:       return "histogram";
	}
}

int gcd_long(long a, long b)
{
	if (a < 0) a = -a;
	if (b < 0) b = -b;
	while (b != 0) {
		long t = a % b;
		a = b;
		b = t;
	}
	return (int)a;
}

/* --------------------------------------------------------------------------
 * the Banerjee bounds machinery
 * ------------------------------------------------------------------------*/
enum TermKind { T_COMMON, T_ONLY1, T_ONLY2 };

struct BTerm {
	int  kind;
	long A;       /* coefficient of the source iteration counter n  (already *step) */
	long B;       /* coefficient of the sink iteration counter  n'  (already *step) */
	long N;       /* trip count: counters range over 0 .. N-1                       */
	int  dir;     /* T_COMMON only: DIR_LT / DIR_EQ / DIR_GT, or -1 for "any"       */
};

static void upd(long v, long *mn, long *mx, bool *any)
{
	if (!*any || v < *mn) *mn = v;
	if (!*any || v > *mx) *mx = v;
	*any = true;
}

/* range of  A*n - B*n'  over the region the direction allows; false if empty */
static bool term_range(const BTerm &t, long *mn, long *mx)
{
	long N = t.N;
	bool any = false;
	*mn = 0;
	*mx = 0;

	if (N <= 0)
		return false;

	if (t.kind == T_ONLY1) {
		upd(0, mn, mx, &any);
		upd(t.A * (N - 1), mn, mx, &any);
		return true;
	}
	if (t.kind == T_ONLY2) {
		upd(0, mn, mx, &any);
		upd(-t.B * (N - 1), mn, mx, &any);
		return true;
	}

	/* T_COMMON: vertices of the feasible region in the (n, n') plane */
	long vn[4], vm[4];
	int  nv = 0;
	if (t.dir == DIR_EQ) {
		vn[0] = 0;     vm[0] = 0;
		vn[1] = N - 1; vm[1] = N - 1;
		nv = 2;
	} else if (t.dir == DIR_LT) {                 /* n < n' */
		if (N < 2) return false;
		vn[0] = 0;     vm[0] = 1;
		vn[1] = 0;     vm[1] = N - 1;
		vn[2] = N - 2; vm[2] = N - 1;
		nv = 3;
	} else if (t.dir == DIR_GT) {                 /* n > n' */
		if (N < 2) return false;
		vn[0] = 1;     vm[0] = 0;
		vn[1] = N - 1; vm[1] = 0;
		vn[2] = N - 1; vm[2] = N - 2;
		nv = 3;
	} else {                                       /* any */
		vn[0] = 0;     vm[0] = 0;
		vn[1] = 0;     vm[1] = N - 1;
		vn[2] = N - 1; vm[2] = 0;
		vn[3] = N - 1; vm[3] = N - 1;
		nv = 4;
	}
	for (int i = 0; i < nv; i++)
		upd(t.A * vn[i] - t.B * vm[i], mn, mx, &any);
	return true;
}

/* can  sum(terms) == rhs  hold?  (true = "possibly dependent") */
static bool banerjee_core(const std::vector<BTerm> &terms, long rhs)
{
	long lo = 0, hi = 0;
	size_t i;
	for (i = 0; i < terms.size(); i++) {
		long a, b;
		if (!term_range(terms[i], &a, &b))
			return false;          /* an empty region: no such pair of iterations */
		lo += a;
		hi += b;
	}
	return rhs >= lo && rhs <= hi;
}

bool gcd_test_single(long a, long b, long c1, long c2)
{
	long g = gcd_long(a, b);
	long d = c2 - c1;
	if (g == 0)
		return d == 0;
	return d % g == 0;
}

bool banerjee_test_single(long a, long b, long c1, long c2, long lo, long step, long trips)
{
	std::vector<BTerm> terms;
	BTerm t;
	t.kind = T_COMMON;
	t.A = a * step;
	t.B = b * step;
	t.N = trips;
	t.dir = -1;
	terms.push_back(t);
	return banerjee_core(terms, (c2 - c1) + (b - a) * lo);
}

/* --------------------------------------------------------------------------
 * testing one reference pair
 * ------------------------------------------------------------------------*/
static size_t common_prefix(RefInfo &r1, RefInfo &r2)
{
	size_t m = 0;
	while (m < r1.loops.size() && m < r2.loops.size() && r1.loops[m] == r2.loops[m])
		m++;
	return m;
}

/* GCD test on every dimension: false means independent */
static bool pair_gcd_maybe(RefInfo &r1, RefInfo &r2)
{
	size_t k, j;
	if (r1.is_scalar || r2.is_scalar || !r1.affine || !r2.affine)
		return true;
	for (k = 0; k < r1.F.size() && k < r2.F.size(); k++) {
		long g = 0;
		for (j = 0; j < r1.F[k].size(); j++) g = gcd_long(g, r1.F[k][j]);
		for (j = 0; j < r2.F[k].size(); j++) g = gcd_long(g, r2.F[k][j]);
		long d = r2.c[k] - r1.c[k];
		if (g == 0 ? d != 0 : d % g != 0)
			return false;
	}
	return true;
}

/* Banerjee test on every dimension under the direction vector `dirs` (length m);
 * dirs == 0 means "any direction at every common level" */
static bool pair_banerjee_maybe(RefInfo &r1, RefInfo &r2, size_t m, const int *dirs)
{
	size_t k, j;
	if (r1.is_scalar || r2.is_scalar || !r1.affine || !r2.affine)
		return true;

	for (k = 0; k < r1.F.size() && k < r2.F.size(); k++) {
		std::vector<BTerm> terms;
		long rhs = r2.c[k] - r1.c[k];

		for (j = 0; j < m; j++) {
			LoopNest *L = r1.loops[j];
			BTerm t;
			t.kind = T_COMMON;
			t.A = r1.F[k][j] * L->step;
			t.B = r2.F[k][j] * L->step;
			t.N = loop_trips(L);
			t.dir = dirs ? dirs[j] : -1;
			terms.push_back(t);
			rhs += (r2.F[k][j] - r1.F[k][j]) * L->lower;
		}
		for (j = m; j < r1.loops.size(); j++) {
			LoopNest *L = r1.loops[j];
			BTerm t;
			t.kind = T_ONLY1;
			t.A = r1.F[k][j] * L->step;
			t.B = 0;
			t.N = loop_trips(L);
			t.dir = -1;
			terms.push_back(t);
			rhs -= r1.F[k][j] * L->lower;
		}
		for (j = m; j < r2.loops.size(); j++) {
			LoopNest *L = r2.loops[j];
			BTerm t;
			t.kind = T_ONLY2;
			t.A = 0;
			t.B = r2.F[k][j] * L->step;
			t.N = loop_trips(L);
			t.dir = -1;
			terms.push_back(t);
			rhs += r2.F[k][j] * L->lower;
		}
		if (!banerjee_core(terms, rhs))
			return false;
	}
	return true;
}

/* --------------------------------------------------------------------------
 * reduction recognition
 * ------------------------------------------------------------------------*/
struct RedCtx {
	IRProgram    *p;
	NestAnalysis *na;
};

static IRStmt *def_of(NestAnalysis *na, const Operand &o)
{
	if (o.kind != OPD_NAME)
		return 0;
	std::map<std::string, IRStmt*>::iterator it = na->defs.find(o.name);
	if (it == na->defs.end())
		return 0;
	return it->second;
}

/* does the computation of `o` (following temp definitions) read the scalar `var`
 * or load from array `var`? */
static bool chain_mentions(NestAnalysis *na, const Operand &o, const std::string &var,
                           int depth)
{
	if (o.kind != OPD_NAME)
		return false;
	if (o.name == var)
		return true;
	if (depth > 20)
		return true;
	IRStmt *d = def_of(na, o);
	if (!d)
		return false;
	if (d->kind == IR_ARRAY_LOAD && d->array == var)
		return true;
	std::vector<std::string> uses;
	ir_stmt_uses(d, &uses);
	size_t i;
	for (i = 0; i < uses.size(); i++)
		if (chain_mentions(na, mk_opd_name(uses[i], TY_INT), var, depth + 1))
			return true;
	return false;
}

static bool same_subscripts(IRStmt *a, IRStmt *b)
{
	size_t i;
	if (a->idx.size() != b->idx.size())
		return false;
	for (i = 0; i < a->idx.size(); i++) {
		if (opd_equal(a->idx[i].value, b->idx[i].value))
			continue;
		if (a->idx[i].affine.kind == IDX_AFFINE && b->idx[i].affine.kind == IDX_AFFINE &&
		    a->idx[i].affine.konst == b->idx[i].affine.konst &&
		    a->idx[i].affine.coeffs == b->idx[i].affine.coeffs)
			continue;
		return false;
	}
	return true;
}

static int count_refs(NestAnalysis *na, const std::string &name)
{
	int n = 0;
	size_t i;
	for (i = 0; i < na->refs.size(); i++)
		if (na->refs[i].name == name)
			n++;
	return n;
}

static void mark_group(NestAnalysis *na, const std::string &name, int gid)
{
	size_t i;
	for (i = 0; i < na->refs.size(); i++)
		if (na->refs[i].name == name)
			na->refs[i].red_group = gid;
}

static int add_reduction(NestAnalysis *na, RedKind k, const std::string &var, bool is_array,
                         bool is_float, IRStmt *anchor)
{
	Reduction r;
	r.kind = k;
	r.var = var;
	r.is_array = is_array;
	r.is_float = is_float;
	r.anchor = anchor;
	na->reductions.push_back(r);
	return (int)na->reductions.size() - 1;
}

static void scan_for_reductions(RedCtx *rc, std::vector<BodyItem> &body)
{
	NestAnalysis *na = rc->na;
	size_t i;

	for (i = 0; i < body.size(); i++) {
		BodyItem &it = body[i];

		if (it.kind == BK_LOOP) {
			scan_for_reductions(rc, it.loop->body);
			continue;
		}

		if (it.kind == BK_IF) {
			IfNode *n = it.ifn;
			scan_for_reductions(rc, n->then_body);
			scan_for_reductions(rc, n->else_body);

			/* conditional max / min:  if (x > m) { m = x; } */
			if (n->has_else || n->cond.kind != OPD_NAME)
				continue;
			IRStmt *cmp = def_of(na, n->cond);
			if (!cmp || cmp->kind != IR_BINOP)
				continue;
			if (cmp->op != OP_LT && cmp->op != OP_LE && cmp->op != OP_GT && cmp->op != OP_GE)
				continue;

			std::string m;
			Operand other;
			bool m_on_right = false;
			if (cmp->lhs.kind == OPD_NAME && rc->p->scalars.count(cmp->lhs.name)) {
				m = cmp->lhs.name;
				other = cmp->rhs;
			} else if (cmp->rhs.kind == OPD_NAME && rc->p->scalars.count(cmp->rhs.name)) {
				m = cmp->rhs.name;
				other = cmp->lhs;
				m_on_right = true;
			} else {
				continue;
			}

			/* the then-branch must end in  m = <value loaded like `other`> */
			IRStmt *assign = 0;
			size_t j;
			for (j = 0; j < n->then_body.size(); j++)
				if (n->then_body[j].kind == BK_STMT && n->then_body[j].stmt->kind == IR_COPY &&
				    n->then_body[j].stmt->dst == m)
					assign = n->then_body[j].stmt;
			if (!assign)
				continue;
			bool value_ok = opd_equal(assign->src, other);
			if (!value_ok) {
				IRStmt *q = def_of(na, assign->src);
				IRStmt *pd = def_of(na, other);
				if (q && pd && q->kind == IR_ARRAY_LOAD && pd->kind == IR_ARRAY_LOAD &&
				    q->array == pd->array && same_subscripts(q, pd))
					value_ok = true;
			}
			if (!value_ok || count_refs(na, m) != 2)
				continue;

			/* `other > m` (m on the right) or `m < other` (m on the left) means m
			 * is replaced by a larger value: max.  The mirror images are min. */
			bool is_max = (m_on_right  && (cmp->op == OP_GT || cmp->op == OP_GE)) ||
			              (!m_on_right && (cmp->op == OP_LT || cmp->op == OP_LE));
			int g = add_reduction(na, is_max ? RED_MAX : RED_MIN, m, false, false, assign);
			mark_group(na, m, g);
			continue;
		}

		IRStmt *s = it.stmt;

		/* scalar:  s = s (+|*|-) e */
		if (s->kind == IR_COPY && rc->p->scalars.count(s->dst) && s->src.kind == OPD_NAME) {
			IRStmt *d = def_of(na, s->src);
			if (!d || d->kind != IR_BINOP)
				continue;
			if (d->op != OP_ADD && d->op != OP_MUL && d->op != OP_SUB)
				continue;
			Operand e;
			bool found = false;
			if (d->lhs.kind == OPD_NAME && d->lhs.name == s->dst) {
				e = d->rhs;
				found = true;
			} else if (d->op != OP_SUB && d->rhs.kind == OPD_NAME && d->rhs.name == s->dst) {
				e = d->lhs;
				found = true;
			}
			if (!found || chain_mentions(na, e, s->dst, 0) || count_refs(na, s->dst) != 2)
				continue;
			int g = add_reduction(na, d->op == OP_MUL ? RED_PROD : RED_SUM, s->dst, false,
			                      s->dst_type == TY_FLOAT, s);
			mark_group(na, s->dst, g);
			continue;
		}

		/* array:  X[f] = X[f] (+|*|-) e   (histogram when f is not affine) */
		if (s->kind == IR_ARRAY_STORE && s->src.kind == OPD_NAME) {
			IRStmt *d = def_of(na, s->src);
			if (!d || d->kind != IR_BINOP)
				continue;
			if (d->op != OP_ADD && d->op != OP_MUL && d->op != OP_SUB)
				continue;
			Operand e;
			IRStmt *ld = 0;
			IRStmt *l1 = def_of(na, d->lhs);
			IRStmt *l2 = def_of(na, d->rhs);
			if (l1 && l1->kind == IR_ARRAY_LOAD && l1->array == s->array &&
			    same_subscripts(l1, s)) {
				ld = l1;
				e = d->rhs;
			} else if (d->op != OP_SUB && l2 && l2->kind == IR_ARRAY_LOAD &&
			           l2->array == s->array && same_subscripts(l2, s)) {
				ld = l2;
				e = d->lhs;
			}
			if (!ld || chain_mentions(na, e, s->array, 0) || count_refs(na, s->array) != 2)
				continue;

			bool affine = true;
			size_t k;
			for (k = 0; k < s->idx.size(); k++)
				if (s->idx[k].affine.kind != IDX_AFFINE)
					affine = false;
			bool is_float = false;
			std::map<std::string, ArrayInfo>::iterator ai = rc->p->arrays.find(s->array);
			if (ai != rc->p->arrays.end())
				is_float = (ai->second.elem_type == TY_FLOAT);
			RedKind kind = affine ? (d->op == OP_MUL ? RED_PROD : RED_SUM) : RED_HIST;
			int g = add_reduction(na, kind, s->array, true, is_float, s);
			mark_group(na, s->array, g);
		}
	}
}

/* --------------------------------------------------------------------------
 * the driver
 * ------------------------------------------------------------------------*/
void dependence_analyse(IRProgram *p, NestAnalysis *na)
{
	size_t a, b, j;

	na->reductions.clear();
	na->deps.clear();
	na->pairs.clear();
	na->par.clear();
	na->analysable = true;
	na->why_not.clear();

	RedCtx rc;
	rc.p = p;
	rc.na = na;
	scan_for_reductions(&rc, na->root->body);

	/* non-affine references (other than histogram updates) defeat the analysis */
	for (a = 0; a < na->refs.size(); a++) {
		RefInfo &r = na->refs[a];
		bool hist = (r.red_group >= 0 && na->reductions[r.red_group].kind == RED_HIST);
		if (!r.affine && !hist && na->analysable) {
			na->analysable = false;
			na->why_not = "reference to " + r.name + " has a non-affine (data-dependent) subscript";
		}
	}

	for (a = 0; a < na->refs.size(); a++) {
		for (b = 0; b < na->refs.size(); b++) {
			RefInfo &r1 = na->refs[a];
			RefInfo &r2 = na->refs[b];

			if (r1.name != r2.name || r1.is_scalar != r2.is_scalar)
				continue;
			if (!r1.is_write && !r2.is_write)
				continue;

			size_t m = common_prefix(r1, r2);

			/* report both tests once per unordered pair */
			if (a <= b && !(a == b && !r1.is_write)) {
				PairVerdict pv;
				pv.a = (int)a;
				pv.b = (int)b;
				pv.nonaffine = (!r1.affine || !r2.affine);
				pv.gcd_maybe = pair_gcd_maybe(r1, r2);
				pv.banerjee_maybe = pair_banerjee_maybe(r1, r2, m, 0);
				na->pairs.push_back(pv);
			}

			if (!pair_gcd_maybe(r1, r2))
				continue;

			/* enumerate all 3^m direction vectors */
			long total = 1;
			for (j = 0; j < m; j++)
				total *= 3;
			for (long code = 0; code < total; code++) {
				std::vector<int> dirs(m, DIR_EQ);
				long c = code;
				for (j = 0; j < m; j++) {
					dirs[j] = (int)(c % 3);
					c /= 3;
				}

				/* orient: the leading non-'=' entry must be '<' */
				int lead = -1;
				for (j = 0; j < m; j++)
					if (dirs[j] != DIR_EQ) {
						lead = (int)j;
						break;
					}
				if (lead >= 0 && dirs[lead] == DIR_GT)
					continue;
				if (lead < 0) {
					if (a == b)
						continue;
					if (r1.order >= r2.order)
						continue;
				}

				if (!pair_banerjee_maybe(r1, r2, m, m ? &dirs[0] : 0))
					continue;

				Dep d;
				d.src = (int)a;
				d.dst = (int)b;
				d.src_stmt = r1.stmt;
				d.dst_stmt = r2.stmt;
				d.var = r1.name;
				d.kind = r1.is_write ? (r2.is_write ? DEP_OUTPUT : DEP_FLOW) : DEP_ANTI;
				for (j = 0; j < m; j++)
					d.common.push_back(r1.loops[j]);
				d.dir = dirs;
				d.level = lead < 0 ? 0 : lead + 1;
				d.reduction = (r1.red_group >= 0 && r1.red_group == r2.red_group);
				na->deps.push_back(d);
			}
		}
	}

	/* a loop is parallel iff it carries no dependence */
	std::map<LoopNest*, int> carried, red_carried;
	for (a = 0; a < na->deps.size(); a++) {
		Dep &d = na->deps[a];
		if (d.level == 0)
			continue;
		LoopNest *L = d.common[d.level - 1];
		if (d.reduction)
			red_carried[L] = 1;
		else
			carried[L] = 1;
	}
	for (j = 0; j < na->loops.size(); j++) {
		LoopNest *L = na->loops[j];
		na->par[L] = carried.count(L) ? PAR_NO : (red_carried.count(L) ? PAR_REDUCTION : PAR_YES);
	}
}

void analyse_nest(IRProgram *p, Kernel *k, LoopNest *root, NestAnalysis *out)
{
	affine_analyse(p, k, root, out);
	dependence_analyse(p, out);
}

/* --------------------------------------------------------------------------
 * reporting
 * ------------------------------------------------------------------------*/
static const char *kind_text(int k)
{
	return k == DEP_FLOW ? "flow" : (k == DEP_ANTI ? "anti" : "output");
}

static std::string ref_text(NestAnalysis *na, int idx)
{
	RefInfo &r = na->refs[idx];
	std::string t = r.name;
	size_t k, j;
	if (r.is_scalar)
		return t;
	t += "[";
	for (k = 0; k < r.F.size(); k++) {
		if (k) t += ", ";
		if (!r.affine) { t += "?"; continue; }
		std::string e;
		for (j = 0; j < r.loops.size(); j++) {
			long v = r.F[k][j];
			if (v == 0) continue;
			char buf[48];
			snprintf(buf, sizeof buf, "%s%s%s", e.empty() ? (v < 0 ? "-" : "") : (v < 0 ? " - " : " + "),
			         (v == 1 || v == -1) ? "" : std::to_string(v < 0 ? -v : v).c_str(),
			         r.loops[j]->index_var.c_str());
			e += buf;
		}
		if (r.c[k] != 0 || e.empty()) {
			char buf[32];
			snprintf(buf, sizeof buf, "%s%ld", e.empty() ? "" : (r.c[k] < 0 ? " - " : " + "),
			         e.empty() ? r.c[k] : (r.c[k] < 0 ? -r.c[k] : r.c[k]));
			e += buf;
		}
		t += e;
	}
	return t + "]";
}

void dependence_print(NestAnalysis *na, int verbose_pairs)
{
	size_t i, j;

	printf("  loop nest rooted at `for %s` of %s %s\n", na->root->index_var.c_str(),
	       na->kernel->is_host ? "host" : "kernel", na->kernel->name.c_str());

	printf("  references (access matrix F, offset c):\n");
	for (i = 0; i < na->refs.size(); i++) {
		RefInfo &r = na->refs[i];
		printf("    R%-2d %-5s %-18s ", (int)i, r.is_write ? "write" : "read", ref_text(na, (int)i).c_str());
		if (r.is_scalar) {
			printf("scalar\n");
			continue;
		}
		if (!r.affine) {
			printf("NON-AFFINE\n");
			continue;
		}
		printf("F=[");
		for (size_t k = 0; k < r.F.size(); k++) {
			printf("%s[", k ? " " : "");
			for (j = 0; j < r.F[k].size(); j++)
				printf("%s%ld", j ? " " : "", r.F[k][j]);
			printf("]");
		}
		printf("]  c=[");
		for (size_t k = 0; k < r.c.size(); k++)
			printf("%s%ld", k ? " " : "", r.c[k]);
		printf("]  loops=(");
		for (j = 0; j < r.loops.size(); j++)
			printf("%s%s", j ? "," : "", r.loops[j]->index_var.c_str());
		printf(")\n");
	}

	if (!na->analysable)
		printf("  NOT ANALYSABLE: %s\n", na->why_not.c_str());

	if (verbose_pairs) {
		printf("  pair tests (each test's verdict on its own):\n");
		for (i = 0; i < na->pairs.size(); i++) {
			PairVerdict &pv = na->pairs[i];
			printf("    R%d %s  vs  R%d %s : ", pv.a, ref_text(na, pv.a).c_str(), pv.b,
			       ref_text(na, pv.b).c_str());
			if (pv.nonaffine) {
				printf("not applicable (non-affine) -> assumed dependent\n");
				continue;
			}
			printf("GCD %-12s Banerjee %-12s", pv.gcd_maybe ? "inconclusive" : "INDEPENDENT",
			       pv.banerjee_maybe ? "inconclusive" : "INDEPENDENT");
			if (pv.gcd_maybe && !pv.banerjee_maybe)
				printf("  <- GCD inconclusive, Banerjee proves independence");
			if (!pv.gcd_maybe && pv.banerjee_maybe)
				printf("  <- GCD proves independence, Banerjee could not");
			printf("\n");
		}
	}

	printf("  dependences (%d):\n", (int)na->deps.size());
	for (i = 0; i < na->deps.size(); i++) {
		Dep &d = na->deps[i];
		printf("    %-6s %-6s R%d -> R%d  dir=(", kind_text(d.kind), d.var.c_str(), d.src, d.dst);
		for (j = 0; j < d.dir.size(); j++)
			printf("%s%s", j ? "," : "", dir_char(d.dir[j]));
		printf(")  ");
		if (d.level == 0)
			printf("loop-independent");
		else
			printf("carried by loop %s", d.common[d.level - 1]->index_var.c_str());
		if (d.reduction)
			printf("  [reduction]");
		printf("\n");
	}

	for (i = 0; i < na->reductions.size(); i++)
		printf("  reduction: %s %s%s\n", red_text(na->reductions[i].kind), na->reductions[i].var.c_str(),
		       na->reductions[i].is_float && (na->reductions[i].kind == RED_SUM ||
		                                       na->reductions[i].kind == RED_PROD)
		           ? "  (float: reassociation changes rounding)" : "");

	for (i = 0; i < na->loops.size(); i++)
		printf("  loop %-3s : %s\n", na->loops[i]->index_var.c_str(),
		       par_text(na->par[na->loops[i]]));
}

void dependence_emit_dot(NestAnalysis *na, FILE *out)
{
	std::map<IRStmt*, int> id;
	size_t i;

	fprintf(out, "digraph \"deps_%s\" {\n  rankdir=TB;\n", na->kernel->name.c_str());
	fprintf(out, "  node [shape=box, fontname=\"monospace\", fontsize=10];\n");
	for (i = 0; i < na->refs.size(); i++) {
		IRStmt *s = na->refs[i].stmt;
		if (!s || id.count(s))
			continue;
		int n = (int)id.size();
		id[s] = n;
		fprintf(out, "  s%d [label=\"%s\"];\n", n, ir_stmt_text(s).c_str());
	}
	for (i = 0; i < na->deps.size(); i++) {
		Dep &d = na->deps[i];
		size_t j;
		std::string dir;
		for (j = 0; j < d.dir.size(); j++)
			dir += std::string(j ? "," : "") + dir_char(d.dir[j]);
		fprintf(out, "  s%d -> s%d [label=\"%s %s (%s)\", style=%s%s];\n", id[d.src_stmt],
		        id[d.dst_stmt], kind_text(d.kind), d.var.c_str(), dir.c_str(),
		        d.kind == DEP_FLOW ? "solid" : (d.kind == DEP_ANTI ? "dashed" : "dotted"),
		        d.level > 0 ? ", color=red" : "");
	}
	fprintf(out, "}\n");
}
