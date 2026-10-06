# ============================================================================
# NEMO-C  --  near-memory-aware compiler.  Plain GNU Makefile (no CMake).
#
#   make            build ./nemoc
#   make demo       front-end + IR + CFG demo on the saxpy example
#   make test       build and run the unit-test programs in tests/
#   make run-all    compile + simulate every benchmark, write results/
#   make clean
#
# Generated scanner/parser and all object files live in build/; hand-written
# C++17 sources are in src/.  Header dependencies are tracked with -MMD.
# ============================================================================

CXX      := g++
CXXFLAGS := -std=c++17 -g -O1 -Wall -Wextra -ffp-contract=off
# flex/bison output does not survive -Wall cleanly; build it with a plain set
GENFLAGS := -std=c++17 -g -O1 -ffp-contract=off
FLEX     := flex
BISON    := bison

SRC   := src
BUILD := build
BIN   := nemoc

GEN_OBJ  := $(BUILD)/parser.tab.o $(BUILD)/lex.yy.o
HAND_SRC := $(filter-out $(SRC)/main.cpp,$(wildcard $(SRC)/*.cpp))
LIB_OBJ  := $(GEN_OBJ) $(patsubst $(SRC)/%.cpp,$(BUILD)/%.o,$(HAND_SRC))
MAIN_OBJ := $(BUILD)/main.o

TEST_SRC := $(wildcard tests/test_*.cpp)
TEST_BIN := $(patsubst tests/%.cpp,$(BUILD)/%,$(TEST_SRC))

.PHONY: all clean demo test run-all
all: $(BIN)

$(BIN): $(LIB_OBJ) $(MAIN_OBJ)
	$(CXX) $(CXXFLAGS) -o $@ $^

# ---- bison: LALR(1) tables, token header (-d), state report (-v),
#      and concrete counterexamples for any unexpected conflict --------------
$(BUILD)/parser.tab.c $(BUILD)/parser.tab.h: $(SRC)/parser.y | $(BUILD)
	$(BISON) -d -v -Wcounterexamples -o $(BUILD)/parser.tab.c $<

# ---- flex: the scanner (needs the token codes from bison) -----------------
$(BUILD)/lex.yy.c: $(SRC)/lexer.l $(BUILD)/parser.tab.h | $(BUILD)
	$(FLEX) -o $@ $<

$(BUILD)/parser.tab.o: $(BUILD)/parser.tab.c $(SRC)/ast.h
	$(CXX) $(GENFLAGS) -I$(SRC) -I$(BUILD) -c -o $@ $<

$(BUILD)/lex.yy.o: $(BUILD)/lex.yy.c $(BUILD)/parser.tab.h $(SRC)/ast.h
	$(CXX) $(GENFLAGS) -I$(SRC) -I$(BUILD) -c -o $@ $<

# ---- hand-written sources (header dependencies via -MMD) -----------------
$(BUILD)/%.o: $(SRC)/%.cpp $(BUILD)/parser.tab.h | $(BUILD)
	$(CXX) $(CXXFLAGS) -MMD -MP -I$(SRC) -I$(BUILD) -c -o $@ $<

-include $(wildcard $(BUILD)/*.d)

$(BUILD):
	mkdir -p $(BUILD)

$(BUILD)/dot:
	mkdir -p $(BUILD)/dot

# ---- unit tests: every tests/test_*.cpp is its own program ---------------
$(BUILD)/test_%: tests/test_%.cpp tests/testutil.h $(LIB_OBJ)
	$(CXX) $(CXXFLAGS) -I$(SRC) -I$(BUILD) -Itests -o $@ $< $(LIB_OBJ)

test: $(TEST_BIN)
	@fail=0; for t in $(TEST_BIN); do \
	    echo "=== $$t"; ./$$t || fail=1; \
	done; \
	if [ $$fail -ne 0 ]; then echo "SOME TESTS FAILED"; exit 1; fi; \
	echo "ALL TEST PROGRAMS PASSED"

# ---- demo ----------------------------------------------------------------
demo: $(BIN) $(BUILD)/dot
	@echo "============ lexer: token stream of benchmarks/saxpy.nmc ============"
	./$(BIN) benchmarks/saxpy.nmc --dump tokens
	@echo
	@echo "============ parser: error recovery (tests/bad_syntax.nmc) =========="
	-./$(BIN) tests/bad_syntax.nmc --dump ast
	@echo
	@echo "============ semantic analysis: broken program ======================"
	-./$(BIN) tests/bad_semantics.nmc --check
	@echo
	@echo "============ IR + CFG for benchmarks/saxpy.nmc ======================"
	./$(BIN) benchmarks/saxpy.nmc --dump ir
	./$(BIN) benchmarks/saxpy.nmc --emit-dot cfg --check

# ---- full evaluation (defined once the simulator exists) -------------------
run-all: $(BIN) $(BUILD)/dot
	./scripts/run_all.sh

clean:
	rm -rf $(BUILD) $(BIN)
