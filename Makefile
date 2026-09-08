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
HAND_OBJ := $(BUILD)/ast.o $(BUILD)/main.o

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

$(BUILD)/main.o: $(SRC)/main.cpp $(SRC)/ast.h $(BUILD)/parser.tab.h | $(BUILD)
	$(CXX) $(CXXFLAGS) -I$(SRC) -I$(BUILD) -c -o $@ $<

$(BUILD):
	mkdir -p $(BUILD)

# ---- Phase A demo ------------------------------------------------------
demo: $(BIN)
	@echo "==================== benchmarks/saxpy.nmc  (valid) ===================="
	./$(BIN) benchmarks/saxpy.nmc
	@echo
	@echo "==================== tests/bad_syntax.nmc  (malformed) ================"
	-./$(BIN) tests/bad_syntax.nmc

clean:
	rm -rf $(BUILD) $(BIN)
