/* ============================================================================
 * cfg.h  --  control-flow graph over basic blocks (declaration only)
 * ==========================================================================*/
#ifndef NEMOC_CFG_H
#define NEMOC_CFG_H

#include <cstdio>
#include <string>
#include <vector>

#include "ir.h"

struct BasicBlock {
	int                  id;
	std::string          label;   /* e.g. "for.i.header", "if.merge"          */
	std::vector<IRStmt*> stmts;   /* straight-line code, in order              */
	std::vector<int>     succ;    /* successor block ids                       */
};

struct CFG {
	std::string              name;    /* the kernel/host this graph belongs to */
	std::vector<BasicBlock*> blocks;
	int                      entry;
	int                      exit;
};

CFG *build_cfg(Kernel *k);
void cfg_emit_dot(CFG *g, FILE *out);

#endif /* NEMOC_CFG_H */
