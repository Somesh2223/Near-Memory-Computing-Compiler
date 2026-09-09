/* ============================================================================
 * symtab.h  --  scoped symbol table (declaration only)
 *
 * Compiler-theory concept: a block-structured symbol table with a stack of
 * scopes.  Name resolution follows the "most closely nested scope" rule --
 * lookup walks from the innermost open scope outward and returns the first
 * binding found (Aho/Lam/Sethi/Ullman 2nd ed., section 2.7 "Symbol Tables" and
 * section 5.5).  The same table is reused by every later phase that needs to
 * know what a name refers to.
 * ==========================================================================*/
#ifndef NEMOC_SYMTAB_H
#define NEMOC_SYMTAB_H

#include <string>
#include <vector>
#include <map>
#include "ast.h"

enum SymKind {
	SYM_SCALAR,    /* scalar <type> name;                                   */
	SYM_ARRAY,     /* array  <type> name[dims];                             */
	SYM_LOOPVAR,   /* a for-loop index, lives only in that loop's scope     */
	SYM_KERNEL,    /* kernel name()                                         */
	SYM_HOST       /* host   name()                                         */
};

struct Sym {
	SymKind          kind;
	std::string      name;
	TypeTag          type;   /* element/scalar type; unused for KERNEL/HOST */
	std::vector<int> dims;   /* SYM_ARRAY only                              */
	Loc              loc;    /* where it was declared                      */
};

struct Scope {
	std::map<std::string, Sym> names;
};

struct SymTab {
	std::vector<Scope> stack;   /* stack[0] = global, stack.back() = innermost */
};

void  st_init(SymTab *st);        /* start with a single (global) scope        */
void  st_push(SymTab *st);        /* enter a nested scope                      */
void  st_pop(SymTab *st);         /* leave it (global scope is never popped)   */

/* Declare in the innermost scope.  Returns 0 on success, or a pointer to the
 * existing binding if the name is already declared in that same scope (the
 * caller turns that into a diagnostic). */
Sym  *st_declare(SymTab *st, SymKind kind, const std::string &name,
                 TypeTag type, const std::vector<int> &dims, Loc loc);

Sym  *st_lookup(SymTab *st, const std::string &name);        /* innermost->global */
Sym  *st_lookup_local(SymTab *st, const std::string &name);  /* innermost only    */

#endif /* NEMOC_SYMTAB_H */
