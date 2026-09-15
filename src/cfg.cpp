/* ============================================================================
 * cfg.cpp  --  control-flow graph construction
 *
 * Compiler-theory concept: building a control-flow graph (CFG) of basic
 * blocks out of structured control constructs (Aho/Lam/Sethi/Ullman 2nd ed.,
 * section 8.4, "Basic Blocks" and section 8.5, "Flow Graphs").  ir.h keeps
 * loops and if/else as a NESTED tree (LoopNest / IfNode) because Phases D-G
 * need a real loop nest to analyse.  The dataflow passes (constant
 * propagation and liveness in Phase C, the coherence analysis in Phase G)
 * are classically defined over a flat graph of basic blocks with explicit
 * join points instead -- so this file walks the same nested IR once more and
 * threads it into that flat graph on demand.  Nothing is destroyed; the
 * LoopNest/IfNode tree that produced this CFG still exists unchanged.
 *
 * A basic block is a maximal run of statements with a single entry (its first
 * statement) and a single exit (its last) -- control never jumps into the
 * middle of one or out of it early.
 *
 * The project brief singles out if/else as "what makes this non-trivial",
 * and the reason is the MERGE block: after the condition, control forks into
 * a then-block and an else-block (or falls straight through when there is no
 * else), and both paths reconverge at one block before anything after the
 * if/else runs. That merge block is exactly the confluence point classical
 * dataflow analysis takes a meet (union / intersection) over -- see Phase C's
 * constant-propagation dump and Phase G's coherence analysis.
 *
 * A `for` loop is the mirror image: a header block reachable both from above
 * and from a back edge out of the loop body, plus a normal edge out to
 * whatever follows the loop.  That back edge -- an edge from a block to one
 * of its own ancestors in program order -- is the textbook signature of a
 * loop in a flow graph.
 * ==========================================================================*/
#include "cfg.h"

struct CfgBuilder {
	CFG *g;
	int  cur;   /* id of the block currently being appended to */
};

static int new_block(CfgBuilder *b, const std::string &label)
{
	BasicBlock *bb = new BasicBlock();
	bb->id    = (int)b->g->blocks.size();
	bb->label = label;
	b->g->blocks.push_back(bb);
	return bb->id;
}

static void connect(CfgBuilder *b, int from, int to)
{
	b->g->blocks[from]->succ.push_back(to);
}

static void build_seq(CfgBuilder *b, std::vector<BodyItem> &items);

static void build_if(CfgBuilder *b, IfNode *n)
{
	int cond_blk = b->cur;   /* the condition test lives at the end of this block */

	int then_entry = new_block(b, "if.then");
	connect(b, cond_blk, then_entry);
	b->cur = then_entry;
	build_seq(b, n->then_body);
	int then_exit = b->cur;

	int else_exit = -1;
	if (n->has_else) {
		int else_entry = new_block(b, "if.else");
		connect(b, cond_blk, else_entry);
		b->cur = else_entry;
		build_seq(b, n->else_body);
		else_exit = b->cur;
	}

	int merge = new_block(b, "if.merge");
	connect(b, then_exit, merge);
	if (n->has_else)
		connect(b, else_exit, merge);
	else
		connect(b, cond_blk, merge);   /* condition false: skip straight to merge */
	b->cur = merge;
}

static void build_loop(CfgBuilder *b, LoopNest *ln)
{
	int pred = b->cur;

	int header = new_block(b, "for." + ln->index_var + ".header");
	connect(b, pred, header);

	int body_entry = new_block(b, "for." + ln->index_var + ".body");
	connect(b, header, body_entry);        /* condition true: enter the body   */
	b->cur = body_entry;
	build_seq(b, ln->body);
	int body_exit = b->cur;
	connect(b, body_exit, header);         /* back edge: this makes it a loop  */

	int after = new_block(b, "for." + ln->index_var + ".exit");
	connect(b, header, after);             /* condition false: leave the loop  */
	b->cur = after;
}

static void build_seq(CfgBuilder *b, std::vector<BodyItem> &items)
{
	size_t i;
	for (i = 0; i < items.size(); i++) {
		BodyItem &it = items[i];
		if (it.kind == BK_STMT)
			b->g->blocks[b->cur]->stmts.push_back(it.stmt);
		else if (it.kind == BK_IF)
			build_if(b, it.ifn);
		else
			build_loop(b, it.loop);
	}
}

CFG *build_cfg(Kernel *k)
{
	CFG *g = new CFG();
	g->name = k->name;

	CfgBuilder b;
	b.g = g;
	b.cur = new_block(&b, "entry");
	g->entry = b.cur;

	build_seq(&b, k->body);

	/* whatever block is open when the body runs out is the exit -- either the
	 * last merge/loop-exit block, or "entry" itself if there was no control
	 * flow at all */
	g->exit = b.cur;
	return g;
}

void cfg_emit_dot(CFG *g, FILE *out)
{
	size_t i, j;

	fprintf(out, "digraph \"%s\" {\n", g->name.c_str());
	fprintf(out, "  rankdir=TB;\n  node [shape=box, fontname=\"monospace\", fontsize=10];\n");

	for (i = 0; i < g->blocks.size(); i++) {
		BasicBlock *bb = g->blocks[i];
		fprintf(out, "  n%d [label=\"%s (B%d)", bb->id, bb->label.c_str(), bb->id);
		for (j = 0; j < bb->stmts.size(); j++)
			fprintf(out, "\\l%s", ir_stmt_text(bb->stmts[j]).c_str());
		fprintf(out, "\\l\"];\n");
	}
	for (i = 0; i < g->blocks.size(); i++) {
		BasicBlock *bb = g->blocks[i];
		for (j = 0; j < bb->succ.size(); j++)
			fprintf(out, "  n%d -> n%d;\n", bb->id, bb->succ[j]);
	}
	fprintf(out, "  n%d [peripheries=2];\n", g->entry);   /* mark the entry block */
	fprintf(out, "}\n");
}
