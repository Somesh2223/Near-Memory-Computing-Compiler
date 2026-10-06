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
#include <cstring>
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
                const std::vector<Subscript> &idx, TypeTag elem_type, Loc l)
{
	IRStmt *s = new IRStmt();
	s->kind     = IR_ARRAY_LOAD;
	s->loc      = l;
	s->dst      = dst;
	s->dst_type = elem_type;
	s->array    = array;
	s->idx      = idx;
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
	/* result type: comparisons yield int; arithmetic promotes to float */
	if (op >= OP_LT && op <= OP_NE)
		s->dst_type = TY_INT;
	else if (lhs.type == TY_FLOAT || rhs.type == TY_FLOAT)
		s->dst_type = TY_FLOAT;
	else
		s->dst_type = TY_INT;
	return s;
}

IRStmt *mk_copy(const std::string &dst, Operand src, TypeTag dst_type, Loc l)
{
	IRStmt *s = new IRStmt();
	s->kind     = IR_COPY;
	s->loc      = l;
	s->dst      = dst;
	s->dst_type = dst_type;
	s->src      = src;
	return s;
}

IRStmt *mk_flush_stmt(const std::string &array, Loc l)
{
	IRStmt *s = new IRStmt();
	s->kind  = IR_FLUSH;
	s->loc   = l;
	s->array = array;
	return s;
}

IRStmt *mk_invalidate_stmt(const std::string &array, Loc l)
{
	IRStmt *s = new IRStmt();
	s->kind  = IR_INVALIDATE;
	s->loc   = l;
	s->array = array;
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
std::string opd_text(const Operand &o)
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

	case IR_FLUSH:
		r = "flush(" + s->array + ")";
		break;

	case IR_INVALIDATE:
		r = "invalidate(" + s->array + ")";
		break;
	}
	return r;
}

bool opd_is_name(const Operand &o)
{
	return o.kind == OPD_NAME;
}

bool opd_equal(const Operand &a, const Operand &b)
{
	if (a.kind != b.kind)
		return false;
	if (a.kind == OPD_INT)
		return a.ival == b.ival;
	if (a.kind == OPD_FLOAT)
		return (float)a.dval == (float)b.dval;
	return a.name == b.name;
}

bool is_temp_name(const std::string &n)
{
	return n.size() > 0 && n[0] == '%';
}

/* --------------------------------------------------------------------------
 * def / use sets of a statement
 * ------------------------------------------------------------------------*/
std::string ir_stmt_def(IRStmt *s)
{
	if (s->kind == IR_ARRAY_LOAD || s->kind == IR_BINOP || s->kind == IR_COPY)
		return s->dst;
	return std::string();
}

static void add_use(const Operand &o, std::vector<std::string> *out)
{
	if (o.kind == OPD_NAME)
		out->push_back(o.name);
}

void ir_stmt_uses(IRStmt *s, std::vector<std::string> *out)
{
	size_t i;
	switch (s->kind) {
	case IR_ARRAY_LOAD:
		for (i = 0; i < s->idx.size(); i++)
			add_use(s->idx[i].value, out);
		break;
	case IR_ARRAY_STORE:
		for (i = 0; i < s->idx.size(); i++)
			add_use(s->idx[i].value, out);
		add_use(s->src, out);
		break;
	case IR_BINOP:
		add_use(s->lhs, out);
		add_use(s->rhs, out);
		break;
	case IR_COPY:
	case IR_PRINT:
		add_use(s->src, out);
		break;
	default:
		break;
	}
}

/* --------------------------------------------------------------------------
 * run-time values.  The single source of truth for operator semantics.
 * ------------------------------------------------------------------------*/
Value mk_val_int(long v)
{
	Value r;
	r.type = TY_INT;
	r.i = v;
	r.f = 0.0f;
	return r;
}

Value mk_val_float(float v)
{
	Value r;
	r.type = TY_FLOAT;
	r.i = 0;
	r.f = v;
	return r;
}

Value value_convert(Value v, TypeTag t)
{
	if (v.type == t)
		return v;
	if (t == TY_FLOAT)
		return mk_val_float((float)v.i);
	return mk_val_int((long)v.f);
}

Value value_of_operand(const Operand &o)
{
	if (o.kind == OPD_FLOAT)
		return mk_val_float((float)o.dval);
	return mk_val_int(o.ival);
}

Value eval_binop(int op, Value a, Value b)
{
	if (op >= OP_LT && op <= OP_NE) {
		bool r = false;
		if (a.type == TY_FLOAT || b.type == TY_FLOAT) {
			float x = (a.type == TY_FLOAT) ? a.f : (float)a.i;
			float y = (b.type == TY_FLOAT) ? b.f : (float)b.i;
			switch (op) {
			case OP_LT: r = x <  y; break;
			case OP_LE: r = x <= y; break;
			case OP_GT: r = x >  y; break;
			case OP_GE: r = x >= y; break;
			case OP_EQ: r = x == y; break;
			default:    r = x != y; break;
			}
		} else {
			switch (op) {
			case OP_LT: r = a.i <  b.i; break;
			case OP_LE: r = a.i <= b.i; break;
			case OP_GT: r = a.i >  b.i; break;
			case OP_GE: r = a.i >= b.i; break;
			case OP_EQ: r = a.i == b.i; break;
			default:    r = a.i != b.i; break;
			}
		}
		return mk_val_int(r ? 1 : 0);
	}

	if (a.type == TY_FLOAT || b.type == TY_FLOAT) {
		float x = (a.type == TY_FLOAT) ? a.f : (float)a.i;
		float y = (b.type == TY_FLOAT) ? b.f : (float)b.i;
		float r = 0.0f;
		switch (op) {
		case OP_ADD: r = x + y; break;
		case OP_SUB: r = x - y; break;
		case OP_MUL: r = x * y; break;
		case OP_DIV: r = x / y; break;
		default:     r = 0.0f;  break;   /* '%' on floats is rejected by semantic.cpp */
		}
		return mk_val_float(r);
	}

	long r = 0;
	switch (op) {
	case OP_ADD: r = a.i + b.i; break;
	case OP_SUB: r = a.i - b.i; break;
	case OP_MUL: r = a.i * b.i; break;
	case OP_DIV: r = (b.i != 0) ? a.i / b.i : 0; break;
	case OP_MOD: r = (b.i != 0) ? a.i % b.i : 0; break;
	default:     r = 0;                          break;
	}
	return mk_val_int(r);
}

bool value_equal_bits(Value a, Value b)
{
	if (a.type != b.type)
		return false;
	if (a.type == TY_INT)
		return a.i == b.i;
	return memcmp(&a.f, &b.f, sizeof(float)) == 0;
}

std::string value_text(Value v)
{
	char buf[64];
	if (v.type == TY_INT)
		snprintf(buf, sizeof buf, "%ld", v.i);
	else
		snprintf(buf, sizeof buf, "%g", (double)v.f);
	return std::string(buf);
}

/* --------------------------------------------------------------------------
 * body walking
 * ------------------------------------------------------------------------*/
void ir_collect_stmts(std::vector<BodyItem> &body, std::vector<IRStmt*> *out)
{
	size_t i;
	for (i = 0; i < body.size(); i++) {
		BodyItem &it = body[i];
		if (it.kind == BK_STMT) {
			out->push_back(it.stmt);
		} else if (it.kind == BK_IF) {
			ir_collect_stmts(it.ifn->then_body, out);
			ir_collect_stmts(it.ifn->else_body, out);
		} else {
			ir_collect_stmts(it.loop->body, out);
		}
	}
}

void ir_collect_loops(std::vector<BodyItem> &body, std::vector<LoopNest*> *out)
{
	size_t i;
	for (i = 0; i < body.size(); i++) {
		BodyItem &it = body[i];
		if (it.kind == BK_IF) {
			ir_collect_loops(it.ifn->then_body, out);
			ir_collect_loops(it.ifn->else_body, out);
		} else if (it.kind == BK_LOOP) {
			out->push_back(it.loop);
			ir_collect_loops(it.loop->body, out);
		}
	}
}

Kernel *ir_find_kernel(IRProgram *p, const std::string &name)
{
	size_t i;
	for (i = 0; i < p->kernels.size(); i++)
		if (p->kernels[i]->name == name)
			return p->kernels[i];
	for (i = 0; i < p->hosts.size(); i++)
		if (p->hosts[i]->name == name)
			return p->hosts[i];
	return 0;
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
