# NEMO-C

A near-memory-aware compiler for the NEMO-C source language. It analyses loop
nests in a `.nmc` program, decides which ones are worth offloading to
processing-in-memory (PIM) hardware using a cost model, and generates host
orchestration code plus DPU kernel code for the ones it selects.

University Compiler Design course project. The compiler is built to demonstrate
classic compiler construction — lexing, LALR parsing, semantic analysis, IR,
dependence analysis, dataflow analysis, loop transformations, classical
optimisations, register allocation, code generation — applied to a near-memory
target.

## Status

| Phase | Contents | State |
|-------|----------|-------|
| **A** | lexer, LALR parser, AST, symbol table, semantic analysis | **done** |
| B | IR (loop-nest-preserving), CFG | next |
| C | const-fold/prop, CSE, DCE, strength reduction, induction vars | — |
| D | affine access matrices, GCD + Banerjee dependence tests, direction vectors | — |
| E | offload profitability cost model (the headline pass) | — |
| F | loop distribution (Allen–Kennedy), interchange, fusion | — |
| G | partitioning (owner-computes), coherence dataflow | — |
| H | host + DPU codegen, scratchpad tiling, Chaitin–Briggs register allocation | — |
| I | cycle/byte/energy simulator, benchmark suite | — |
| J | per-pass unit tests, result tables, charts | — |

## Build

Requires a C++17 compiler, GNU `make`, `flex`, and `bison`. Standard library
only — no LLVM, no Boost.

```bash
make          # builds ./nemoc
make demo     # parses benchmarks/saxpy.nmc and tests/bad_syntax.nmc
make clean
```

## Run

```bash
./nemoc file.nmc                 # lex + parse + semantic checks, then print the AST
./nemoc file.nmc --dump tokens   # stop after the lexer, print the token stream
./nemoc file.nmc --dump ast      # lex + parse only, print the AST (no semantic pass)
./nemoc file.nmc --check         # lex + parse + semantic checks, report pass/fail only
```

Exit status is non-zero if any lexical, syntax, or semantic error is found.

## Layout

```
src/    lexer.l parser.y ast.{h,cpp} + (later) the passes
build/  generated scanner/parser + object files + bison state report
benchmarks/  the .nmc benchmark programs
tests/       malformed inputs and (later) per-pass unit tests
docs/        analysis write-ups and Graphviz output
```

## Front end (Phase A — complete)

- **`src/lexer.l`** — flex scanner. Maximal-munch tokenisation with keyword-
  before-identifier rule ordering; exact line/column tracking via
  `YY_USER_ACTION` for diagnostics. `//` and `/* */` comments are consumed as
  whitespace.
- **`src/parser.y`** — bison LALR(1) grammar building the AST through
  syntax-directed actions. **Zero shift/reduce and reduce/reduce conflicts**
  (`%expect 0`); the header comment explains why the mandatory-brace `if`/`else`
  avoids the dangling-else conflict entirely and how operator precedence is
  resolved with `%left`/`%nonassoc` declarations. `stmt: error ';'` gives
  panic-mode recovery so multiple syntax errors are reported in one run.
- **`src/ast.{h,cpp}`** — the AST as tagged structs (`Kind` enum + superset of
  fields), no class hierarchy. `ast.cpp` also has the recursive pretty-printer.
- **`src/symtab.{h,cpp}`** — block-structured symbol table: a stack of scopes,
  most-closely-nested-scope lookup. Global scope holds arrays / scalars /
  kernels / host blocks; each `for` opens a nested scope holding just its index
  variable, so the index is undefined outside the loop.
- **`src/semantic.cpp`** — syntax-directed AST walk doing name resolution and
  bottom-up type synthesis. Two passes (collect top-level names, then check
  bodies). Checks: undeclared identifiers; int/float type rules with int→float
  promotion and no implicit narrowing; array subscript count vs declaration;
  integer-only subscripts; constant loop bounds; consistent `for`-header index
  variable, positive step, `<`/`<=` test; loop index not reassigned or shadowed;
  `call`/`print` only in host code; redeclarations. All errors are collected in
  one run with `file:line:col` positions.
