/* ============================================================================
 * semantic.h  --  semantic analysis entry point (declaration only)
 * ==========================================================================*/
#ifndef NEMOC_SEMANTIC_H
#define NEMOC_SEMANTIC_H

#include "ast.h"

/* Run all static checks over the parsed program.  Prints one diagnostic per
 * error to stderr (file:line:col: error: ...) and returns the error count;
 * 0 means the program is well-formed. */
int semantic_check(Program *p);

#endif /* NEMOC_SEMANTIC_H */
