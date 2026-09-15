/* ============================================================================
 * ir.cpp  --  IR node constructors and the IR pretty-printer
 *
 * Mirrors ast.cpp: plain constructor functions (no methods, no `new` hidden
 * behind a factory class) and a recursive pre-order walk for printing.
 * `ir_stmt_text` renders a single three-address statement as one line of
 * text; both the plain `--dump ir` printer and cfg.cpp's Graphviz export call
 * it, so a statement is formatted in exactly one place.
 * ==========================================================================*/
#include <cstdio>
#include "ir.h"

/* --------------------------------------------------------------------------
 * constructors
 * ------------------------------------------------------------------------*/
Operand mk_opd_int(long v)
{
	Operand o;
	o.kind = OPD_INT;
	o.type = TY_INT;
	o.ival = v;
	o.dval = 0.0;
	return o;
}

Operand mk_opd_float(double v)
{
	Operand o;
	o.kind = OPD_FLOAT;
	o.type = TY_FLOAT;
	o.dval = v;
	o.ival = 0;
	return o;
}

Operand mk_opd_name(const std::string &n, TypeTag t)
{
	Operand o;
	o.kind = OPD_NAME;
	o.type = t;
	o.name = n;
	o.ival = 0;
	o.dval = 0.0;
	return o;
}

IndexExpr mk_index_non_affine(void)
{
	IndexExpr ix;
	ix.kind  = IDX_NON_AFFINE;
	ix.konst = 0;
	return ix;
}

Subscript mk_subscript(Operand v)
{
	Subscript s;
	s.value  = v;
	s.affine = mk_index_non_affine();
	return s;
}

IRStmt *mk_load(const std::string &dst, const std::string &array,
                const std::vector<Subscript> &idx, Loc l)
{
	IRStmt *s = new IRStmt();
	s->kind  = IR_ARRAY_LOAD;
	s->loc   = l;
	s->dst   = dst;
	s->array = array;
	s->idx   = idx;
	return s;
}

IRStmt *mk_store(const std::string &array, const std::vector<Subscript> &idx,
                 Operand src, Loc l)
{
	IRStmt *s = new IRStmt();
	s->kind  = IR_ARRAY_STORE;
	s->loc   = l;
	s->array = array;
	s->idx   = idx;
	s->src   = src;
	return s;
}

IRStmt *mk_binop_stmt(const std::string &dst, int op, Operand lhs, Operand rhs, Loc l)
{
	IRStmt *s = new IRStmt();
	s->kind = IR_BINOP;
	s->loc  = l;
	s->dst  = dst;
	s->op   = (OpKind)op;
	s->lhs  = lhs;
	s->rhs  = rhs;
	return s;
}

IRStmt *mk_copy(const std::string &dst, Operand src, Loc l)
{
	IRStmt *s = new IRStmt();
	s->kind = IR_COPY;
	s->loc  = l;
	s->dst  = dst;
	s->src  = src;
	return s;
}

IRStmt *mk_call_stmt(const std::string &callee, Loc l)
{
	IRStmt *s = new IRStmt();
	s->kind   = IR_CALL;
	s->loc    = l;
	s->callee = callee;
	return s;
}

IRStmt *mk_print_stmt(Operand src, Loc l)
{
	IRStmt *s = new IRStmt();
	s->kind = IR_PRINT;
	s->loc  = l;
	s->src  = src;
	return s;
}

BodyItem mk_bi_stmt(IRStmt *s)
{
	BodyItem it;
	it.kind = BK_STMT;
	it.stmt = s;
	it.loop = 0;
	it.ifn  = 0;
	return it;
}

BodyItem mk_bi_loop(LoopNest *l)
{
	BodyItem it;
	it.kind = BK_LOOP;
	it.stmt = 0;
	it.loop = l;
	it.ifn  = 0;
	return it;
}

BodyItem mk_bi_if(IfNode *n)
{
	BodyItem it;
	it.kind = BK_IF;
	it.stmt = 0;
	it.loop = 0;
	it.ifn  = n;
	return it;
}

/* --------------------------------------------------------------------------
 * text rendering
 * ------------------------------------------------------------------------*/
static std::string opd_text(const Operand &o)
{
	char buf[64];
	if (o.kind == OPD_INT) {
		snprintf(buf, sizeof buf, "%ld", o.ival);
		return std::string(buf);
	}
	if (o.kind == OPD_FLOAT) {
		snprintf(buf, sizeof buf, "%g", o.dval);
		return std::string(buf);
	}
	return o.name;
}

std::string ir_stmt_text(IRStmt *s)
{
	std::string r;
	size_t i;

	switch (s->kind) {
	case IR_ARRAY_LOAD:
		r = s->dst + " = " + s->array + "[";
		for (i = 0; i < s->idx.size(); i++) {
			if (i) r += ", ";
			r += opd_text(s->idx[i].value);
		}
		r += "]";
		break;

	case IR_ARRAY_STORE:
		r = s->array + "[";
		for (i = 0; i < s->idx.size(); i++) {
			if (i) r += ", ";
			r += opd_text(s->idx[i].value);
		}
		r += "] = " + opd_text(s->src);
		break;

	case IR_BINOP:
		r = s->dst + " = " + opd_text(s->lhs) + " " + op_str(s->op) + " " +
		    opd_text(s->rhs);
		break;

	case IR_COPY:
		r = s->dst + " = " + opd_text(s->src);
		break;

	case IR_CALL:
		r = "call " + s->callee + "()";
		break;

	case IR_PRINT:
		r = "print " + opd_text(s->src);
		break;
	}
	return r;
}

/* --------------------------------------------------------------------------
 * pretty printer
 * ------------------------------------------------------------------------*/
static void indent(int depth)
{
	int i;
	for (i = 0; i < depth; i++)
		printf("  ");
}

static void print_body(std::vector<BodyItem> &body, int depth);

static void print_bodyitem(BodyItem &it, int depth)
{
	if (it.kind == BK_STMT) {
		indent(depth);
		printf("%-40s  @%d:%d\n", ir_stmt_text(it.stmt).c_str(),
		       it.stmt->loc.line, it.stmt->loc.col);
		return;
	}

	if (it.kind == BK_IF) {
		IfNode *n = it.ifn;
		indent(depth);
		printf("if (%s)   @%d:%d\n", opd_text(n->cond).c_str(),
		       n->loc.line, n->loc.col);
		indent(depth); printf("then:\n");
		print_body(n->then_body, depth + 1);
		if (n->has_else) {
			indent(depth); printf("else:\n");
			print_body(n->else_body, depth + 1);
		}
		return;
	}

	/* BK_LOOP */
	LoopNest *ln = it.loop;
	indent(depth);
	printf("for (%s = %ld; %s %s %ld; %s += %ld)   @%d:%d\n",
	       ln->index_var.c_str(), ln->lower,
	       ln->index_var.c_str(), op_str(ln->rel), ln->upper,
	       ln->index_var.c_str(), ln->step,
	       ln->loc.line, ln->loc.col);
	print_body(ln->body, depth + 1);
}

static void print_body(std::vector<BodyItem> &body, int depth)
{
	size_t i;
	for (i = 0; i < body.size(); i++)
		print_bodyitem(body[i], depth);
}

void ir_print_program(IRProgram *p)
{
	std::map<std::string, ArrayInfo>::iterator ai;
	std::map<std::string, TypeTag>::iterator   si;
	size_t k;

	printf("IR\n");
	printf("arrays:\n");
	for (ai = p->arrays.begin(); ai != p->arrays.end(); ai++) {
		printf("  %-10s %s[", ai->first.c_str(), type_str(ai->second.elem_type));
		for (size_t d = 0; d < ai->second.dims.size(); d++)
			printf("%s%d", d ? "," : "", ai->second.dims[d]);
		printf("]  (%d bytes/element)\n", ai->second.elem_bytes);
	}
	printf("scalars:\n");
	for (si = p->scalars.begin(); si != p->scalars.end(); si++)
		printf("  %-10s %s\n", si->first.c_str(), type_str(si->second));

	for (k = 0; k < p->kernels.size(); k++) {
		printf("kernel %s()\n", p->kernels[k]->name.c_str());
		print_body(p->kernels[k]->body, 1);
	}
	for (k = 0; k < p->hosts.size(); k++) {
		printf("host %s()\n", p->hosts[k]->name.c_str());
		print_body(p->hosts[k]->body, 1);
	}
}
