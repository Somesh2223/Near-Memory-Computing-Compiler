/* ============================================================================
 * frontend.h  --  one-call access to lexing, parsing, semantic analysis and
 * lowering, for the driver and for the unit tests.
 * ==========================================================================*/
#ifndef NEMOC_FRONTEND_H
#define NEMOC_FRONTEND_H

#include <string>
#include "ast.h"
#include "ir.h"

/* lex + parse a file.  Returns the AST, or 0 if the file cannot be opened or
 * has syntax errors; *nerrors receives the number of syntax errors (-1 if the
 * file could not be opened). Diagnostics go to stderr. */
Program *frontend_parse(const char *path, int *nerrors);

/* parse + semantic analysis + lowering to IR.  Returns 0 on any error. When
 * optimise is nonzero the classical passes of Phase C are run on the IR. */
IRProgram *frontend_compile(const char *path, int optimise);

/* same, but from program text held in memory (writes a temp file) */
Program   *frontend_parse_text(const std::string &text, int *nerrors);
IRProgram *frontend_compile_text(const std::string &text, int optimise);

#endif
