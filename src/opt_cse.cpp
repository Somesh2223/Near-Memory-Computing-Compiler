/* ============================================================================
 * opt_cse.cpp  --  common subexpression elimination, local to a basic block
 *
 * Compiler-theory concept: local common-subexpression elimination driven by
 * the set of AVAILABLE EXPRESSIONS at each point of a basic block (Aho/Lam/
 * Sethi/Ullman 2nd ed., section 8.5.4 "Finding Local Common Subexpressions" and
 * the available-expressions formulation of section 9.2.6).  An expression is
 * available at a point if it has been computed on every path to that point and
 * none of its operands has been redefined since.  Inside one basic block there
 * is only one path, so "available" is simply "computed earlier in this block
 * and not killed since".
 *
 *   gen   a statement  x = a op b  (or  x = A[idx])  makes the expression
 *         "a op b" available, held in x -- unless x is one of its own operands
 *   kill  redefining a name kills every expression that mentions that name and
 *         every expression whose value is held in that name;
 *         a store to array X kills every available load from X;
 *         a call kills everything (the kernel may write any array or scalar)
 *
 * A recomputation of an available expression becomes a copy from its holder.
 * Block-local copy propagation then rewrites later uses of the copy's target
 * to the original, so the copy itself dies in dead-code elimination.
 * Commutative operators (+ * == !=) are canonicalised so a+b and b+a match.
 * ==========================================================================*/
#include <cstdio>
#include <map>
#include <string>
#include <vector>
#include <algorithm>

#include "ir.h"
#include "cfg.h"
#include "opt.h"

struct AvailExpr {
	std::string holder;                 /* the name currently holding the value */
	std::vector<std::string> operands;  /* names the expression mentions        */
	bool        is_load;
	std::string array;                  /* for loads                            */
};

typedef std::map<std::string, AvailExpr> AvailSet;

static bool commutative(int op)
{
	return op == OP_ADD || op == OP_MUL || op == OP_EQ || op == OP_NE;
}

/* canonical text of the expression a statement computes ("" if it computes none) */
static std::string expr_key(IRStmt *s, std::vector<std::string> *operands)
{
	size_t i;
	std::string key;

	if (s->kind == IR_BINOP) {
		std::string a = opd_text(s->lhs);
		std::string b = opd_text(s->rhs);
		if (commutative(s->op) && b < a)
			std::swap(a, b);
		key = std::string("b") + op_str(s->op) + "|" + a + "|" + b;
		if (s->lhs.kind == OPD_NAME) operands->push_back(s->lhs.name);
		if (s->rhs.kind == OPD_NAME) operands->push_back(s->rhs.name);
		return key;
	}
	if (s->kind == IR_ARRAY_LOAD) {
		key = "l|" + s->array;
		for (i = 0; i < s->idx.size(); i++) {
			key += "|" + opd_text(s->idx[i].value);
			if (s->idx[i].value.kind == OPD_NAME)
				operands->push_back(s->idx[i].value.name);
		}
		return key;
	}
	return key;
}

static void kill_name(AvailSet &av, const std::string &n)
{
	AvailSet::iterator it = av.begin();
	while (it != av.end()) {
		bool dead = (it->second.holder == n);
		size_t i;
		for (i = 0; i < it->second.operands.size() && !dead; i++)
			if (it->second.operands[i] == n)
				dead = true;
		if (dead) {
			AvailSet::iterator del = it;
			++it;
			av.erase(del);
		} else {
			++it;
		}
	}
}

static void kill_loads(AvailSet &av, const std::string &array)
{
	AvailSet::iterator it = av.begin();
	while (it != av.end()) {
		if (it->second.is_load && (array.empty() || it->second.array == array)) {
			AvailSet::iterator del = it;
			++it;
			av.erase(del);
		} else {
			++it;
		}
	}
}

/* block-local copy propagation: name -> the name it is a copy of */
typedef std::map<std::string, Operand> CopyMap;

static int subst_operand(CopyMap &cm, Operand *o)
{
	if (o->kind != OPD_NAME)
		return 0;
	CopyMap::iterator it = cm.find(o->name);
	if (it == cm.end())
		return 0;
	Operand repl = it->second;
	repl.type = o->type;           /* the original use keeps its declared type */
	*o = repl;
	return 1;
}

static void cm_kill(CopyMap &cm, const std::string &n)
{
	CopyMap::iterator it = cm.begin();
	while (it != cm.end()) {
		bool dead = (it->first == n) ||
		            (it->second.kind == OPD_NAME && it->second.name == n);
		if (dead) {
			CopyMap::iterator del = it;
			++it;
			cm.erase(del);
		} else {
			++it;
		}
	}
}

static int cse_block(BasicBlock *bb, int verbose, const std::string &kname)
{
	AvailSet av;
	CopyMap  cm;
	int changes = 0;
	size_t i, k;

	for (i = 0; i < bb->stmts.size(); i++) {
		IRStmt *s = bb->stmts[i];

		/* 1. copy propagation on the uses */
		switch (s->kind) {
		case IR_ARRAY_LOAD:
		case IR_ARRAY_STORE:
			for (k = 0; k < s->idx.size(); k++)
				changes += subst_operand(cm, &s->idx[k].value);
			if (s->kind == IR_ARRAY_STORE)
				changes += subst_operand(cm, &s->src);
			break;
		case IR_BINOP:
			changes += subst_operand(cm, &s->lhs);
			changes += subst_operand(cm, &s->rhs);
			break;
		case IR_COPY:
		case IR_PRINT:
			changes += subst_operand(cm, &s->src);
			break;
		default:
			break;
		}

		/* 2. is the expression already available? */
		std::vector<std::string> operands;
		std::string key = expr_key(s, &operands);
		if (!key.empty()) {
			AvailSet::iterator hit = av.find(key);
			if (hit != av.end() && hit->second.holder != s->dst) {
				if (verbose)
					printf("    [%s] %s : replaces `%s` by a copy of %s\n",
					       kname.c_str(), bb->label.c_str(),
					       ir_stmt_text(s).c_str(), hit->second.holder.c_str());
				std::string holder = hit->second.holder;
				TypeTag t = s->dst_type;
				s->kind = IR_COPY;
				s->src  = mk_opd_name(holder, t);
				changes++;
				key.clear();
			}
		}

		/* 3. kill what this statement invalidates */
		switch (s->kind) {
		case IR_ARRAY_STORE:
			kill_loads(av, s->array);
			break;
		case IR_CALL:
			av.clear();
			cm.clear();
			break;
		default:
			break;
		}
		std::string def = ir_stmt_def(s);
		if (!def.empty()) {
			kill_name(av, def);
			cm_kill(cm, def);
		}

		/* 4. gen */
		if (!key.empty() && !def.empty()) {
			bool self = false;
			for (k = 0; k < operands.size(); k++)
				if (operands[k] == def)
					self = true;
			if (!self) {
				AvailExpr e;
				e.holder   = def;
				e.operands = operands;
				e.is_load  = (s->kind == IR_ARRAY_LOAD);
				e.array    = s->array;
				av[key] = e;
			}
		}
		if (s->kind == IR_COPY && s->src.kind == OPD_NAME && s->src.name != s->dst
		    && s->dst_type == s->src.type)
			cm[s->dst] = s->src;
	}

	if (bb->branch != 0)
		changes += subst_operand(cm, &bb->branch->cond);
	return changes;
}

static int cse_kernel(Kernel *k, int verbose)
{
	CFG *g = build_cfg(k);
	int changes = 0;
	size_t b;
	for (b = 0; b < g->blocks.size(); b++)
		changes += cse_block(g->blocks[b], verbose, k->name);
	return changes;
}

int opt_cse(IRProgram *p, int verbose)
{
	int total = 0;
	size_t k;
	for (k = 0; k < p->kernels.size(); k++)
		total += cse_kernel(p->kernels[k], verbose);
	for (k = 0; k < p->hosts.size(); k++)
		total += cse_kernel(p->hosts[k], verbose);
	return total;
}
