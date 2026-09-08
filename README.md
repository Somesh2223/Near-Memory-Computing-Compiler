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
| **A** | lexer, LALR parser, AST, symbol table, semantic analysis | parser + AST done; symtab/semantic next |
| B | IR (loop-nest-preserving), CFG | — |
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
./nemoc benchmarks/saxpy.nmc        # prints the AST
./nemoc path/to/file.nmc --dump ast
```

## Layout

```
src/    lexer.l parser.y ast.{h,cpp} + (later) the passes
build/  generated scanner/parser + object files + bison state report
benchmarks/  the .nmc benchmark programs
tests/       malformed inputs and (later) per-pass unit tests
docs/        analysis write-ups and Graphviz output
```

## Front end (Phase A so far)

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
  fields), no class hierarchy. `ast.cpp` also has the recursive pretty-printer
  used by `--dump ast`.
