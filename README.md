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
| **B** | IR (loop-nest-preserving), CFG | **done** |
| C | const-fold/prop, CSE, DCE, strength reduction, induction vars | next |
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
./nemoc file.nmc --dump ir       # ...then lower to IR and print it (three-address code)
./nemoc file.nmc --emit-dot cfg  # also build a CFG per kernel/host -> build/dot/*.cfg.dot
```

Exit status is non-zero if any lexical, syntax, or semantic error is found.
`--emit-dot cfg` needs `build/dot/` to exist (`make` creates it; `mkdir -p build/dot`
otherwise). Render a graph with e.g. `dot -Tpng build/dot/saxpy.cfg.dot -o saxpy.png`
(Graphviz) or any `.dot` viewer/extension — nemoc itself only writes the text.

## Layout

```
src/    lexer.l parser.y ast.{h,cpp} symtab.{h,cpp} semantic.{h,cpp}
        ir.{h,cpp} lower.cpp cfg.{h,cpp} main.cpp + (later) the passes
build/  generated scanner/parser + object files + bison state report + dot/
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

## IR and CFG (Phase B — complete)

- **`src/ir.h`** — the IR. Deliberately two-level: statements are
  **three-address code** (one operation each, temps named `%t0`, `%t1`, …),
  but loops and if/else stay **structured** (`LoopNest` / `IfNode` hold nested
  bodies) rather than being dissolved into a flat graph — every later pass
  (dependence analysis, the cost model, fission/interchange/fusion,
  partitioning) needs a real loop nest to walk. `IndexExpr` (affine subscript
  classification) is declared here but populated later, by `pass_affine.cpp`
  in Phase D; every subscript starts out `IDX_NON_AFFINE`.
- **`src/lower.cpp`** — AST → IR. Flattens each expression to 3AC by
  introducing a fresh temp per subexpression bottom-up, e.g.
  `C[i] = alpha*A[i] + B[i]` becomes
  `%t0=A[i]; %t1=alpha*%t0; %t2=B[i]; %t3=%t1+%t2; C[i]=%t3`. Loop bounds are
  folded to plain integers here, since semantic analysis already proved they
  are compile-time constants. Assumes `semantic_check()` returned 0.
- **`src/cfg.{h,cpp}`** — flattens the same nested IR into a real
  control-flow graph of basic blocks, one CFG per kernel/host block, built
  fresh on demand (the nested IR itself is untouched). This is where if/else
  needs real work: both branches (or the false fall-through) reconverge at an
  explicit **merge block** — the join point classical dataflow analysis takes
  a meet over — and a loop's body closes with a **back edge** into its own
  header. `--emit-dot cfg` writes one Graphviz file per kernel/host to
  `build/dot/`.
