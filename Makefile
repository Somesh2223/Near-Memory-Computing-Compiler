# ============================================================================
# NEMO-C  --  near-memory-aware compiler.  Plain GNU Makefile (no CMake).
#
# Phase A target: `nemoc`, a front end that scans + parses a .nmc file and
# prints its AST.  Generated scanner/parser go under build/; hand-written
# C++17 sources live in src/.
# ============================================================================

CXX      := g++
CXXFLAGS := -std=c++17 -g -O0 -Wall -Wextra
# flex/bison output does not survive -Wall cleanly; build it with a plain set
GENFLAGS := -std=c++17 -g -O0
FLEX     := flex
BISON    := bison

SRC   := src
BUILD := build
BIN   := nemoc

GEN_OBJ  := $(BUILD)/parser.tab.o $(BUILD)/lex.yy.o
HAND_OBJ := $(BUILD)/ast.o $(BUILD)/symtab.o $(BUILD)/semantic.o $(BUILD)/main.o

.PHONY: all clean demo
all: $(BIN)

$(BIN): $(GEN_OBJ) $(HAND_OBJ)
	$(CXX) $(CXXFLAGS) -o $@ $^

# ---- bison: LALR(1) tables, token header (-d), state report (-v),
#      and concrete counterexamples for any unexpected conflict --------------
$(BUILD)/parser.tab.c $(BUILD)/parser.tab.h: $(SRC)/parser.y | $(BUILD)
	$(BISON) -d -v -Wcounterexamples -o $(BUILD)/parser.tab.c $<

# ---- flex: the scanner (needs the token codes from bison) -----------------
$(BUILD)/lex.yy.c: $(SRC)/lexer.l $(BUILD)/parser.tab.h | $(BUILD)
	$(FLEX) -o $@ $<

# ---- compilation ---------------------------------------------------------
$(BUILD)/parser.tab.o: $(BUILD)/parser.tab.c $(SRC)/ast.h
	$(CXX) $(GENFLAGS) -I$(SRC) -I$(BUILD) -c -o $@ $<

$(BUILD)/lex.yy.o: $(BUILD)/lex.yy.c $(BUILD)/parser.tab.h $(SRC)/ast.h
	$(CXX) $(GENFLAGS) -I$(SRC) -I$(BUILD) -c -o $@ $<

$(BUILD)/ast.o: $(SRC)/ast.cpp $(SRC)/ast.h | $(BUILD)
	$(CXX) $(CXXFLAGS) -I$(SRC) -c -o $@ $<

$(BUILD)/symtab.o: $(SRC)/symtab.cpp $(SRC)/symtab.h $(SRC)/ast.h | $(BUILD)
	$(CXX) $(CXXFLAGS) -I$(SRC) -c -o $@ $<

$(BUILD)/semantic.o: $(SRC)/semantic.cpp $(SRC)/semantic.h $(SRC)/symtab.h $(SRC)/ast.h | $(BUILD)
	$(CXX) $(CXXFLAGS) -I$(SRC) -c -o $@ $<

$(BUILD)/main.o: $(SRC)/main.cpp $(SRC)/ast.h $(SRC)/semantic.h $(BUILD)/parser.tab.h | $(BUILD)
	$(CXX) $(CXXFLAGS) -I$(SRC) -I$(BUILD) -c -o $@ $<

$(BUILD):
	mkdir -p $(BUILD)

# ---- Phase A demo ------------------------------------------------------
demo: $(BIN)
	@echo "============ lexer: token stream of benchmarks/saxpy.nmc ============"
	./$(BIN) benchmarks/saxpy.nmc --dump tokens
	@echo
	@echo "============ lexer: invalid characters (tests/bad_token.nmc) ========"
	-./$(BIN) tests/bad_token.nmc --dump tokens
	@echo
	@echo "============ parser: AST of benchmarks/saxpy.nmc (valid) ============"
	./$(BIN) benchmarks/saxpy.nmc
	@echo
	@echo "============ parser: error recovery (tests/bad_syntax.nmc) =========="
	-./$(BIN) tests/bad_syntax.nmc --dump ast
	@echo
	@echo "============ semantic analysis: valid program ======================="
	./$(BIN) benchmarks/saxpy.nmc --check
	@echo
	@echo "============ semantic analysis: broken program ======================"
	-./$(BIN) tests/bad_semantics.nmc --check
	@echo
	@echo "============ bison: grammar is conflict-free ========================"
	@grep -qiE 'conflict' $(BUILD)/parser.output && grep -iE 'conflict' $(BUILD)/parser.output || echo "0 shift/reduce and 0 reduce/reduce conflicts (see build/parser.output)"

clean:
	rm -rf $(BUILD) $(BIN)
