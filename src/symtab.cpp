/* ============================================================================
 * symtab.cpp  --  scoped symbol table
 *
 * Compiler-theory concept: block-structured (lexically scoped) name resolution.
 * Scopes form a stack; a declaration binds a name in the top scope; a use is
 * resolved by scanning the stack from the top down and taking the first match
 * (the "most closely nested scope" rule -- Aho/Lam/Sethi/Ullman 2nd ed., 2.7).
 *
 * In NEMO-C the global scope holds arrays, scalars, kernels and host blocks;
 * each for-loop opens a nested scope that holds exactly one name -- its index
 * variable -- so the index is undefined outside the loop and a nested loop
 * that reuses the name shadows rather than clobbers.
 * ==========================================================================*/
#include "symtab.h"

void st_init(SymTab *st)
{
	st->stack.clear();
	st->stack.push_back(Scope());          /* the global scope */
}

void st_push(SymTab *st)
{
	st->stack.push_back(Scope());
}

void st_pop(SymTab *st)
{
	if (st->stack.size() > 1)
		st->stack.pop_back();
}

Sym *st_declare(SymTab *st, SymKind kind, const std::string &name,
                TypeTag type, const std::vector<int> &dims, Loc loc)
{
	Scope &top = st->stack.back();

	std::map<std::string, Sym>::iterator it = top.names.find(name);
	if (it != top.names.end())
		return &it->second;            /* already bound here: caller reports it */

	Sym s;
	s.kind = kind;
	s.name = name;
	s.type = type;
	s.dims = dims;
	s.loc  = loc;
	top.names[name] = s;
	return 0;
}

Sym *st_lookup(SymTab *st, const std::string &name)
{
	int i;
	for (i = (int)st->stack.size() - 1; i >= 0; i--) {
		std::map<std::string, Sym>::iterator it = st->stack[i].names.find(name);
		if (it != st->stack[i].names.end())
			return &it->second;
	}
	return 0;
}

Sym *st_lookup_local(SymTab *st, const std::string &name)
{
	Scope &top = st->stack.back();
	std::map<std::string, Sym>::iterator it = top.names.find(name);
	if (it == top.names.end())
		return 0;
	return &it->second;
}
