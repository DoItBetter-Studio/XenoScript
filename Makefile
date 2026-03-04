# =============================================================================
# XenoScript Makefile
#
# Targets:
#   all           Build xenoc and xenovm (default)
#   xenoc         Build the standalone compiler
#   xenovm        Build the standalone VM
#   tests         Build and run all test binaries
#   clean         Remove all build artifacts
#
# Layout:
#   include/      Public headers (all source files use -I include)
#   source/core/     Shared: lexer, ast, parser, checker, bytecode
#   source/compiler/ Compiler, xbc serializer, xenoc entry point
#   source/vm/       VM, xenovm entry point
#   tests/        Test drivers
#   build/        Object files and static libraries (generated)
#   bin/          Output binaries (generated)
# =============================================================================

CC      := x86_64-w64-mingw32-gcc
CFLAGS  := -std=c17 -Wall -Wextra -Iincludes
LDFLAGS := -lm

# Debug vs Release
# Pass DEBUG=1 to enable: make DEBUG=1
ifdef DEBUG
    CFLAGS += -g -fsanitize=address -DDEBUG
    LDFLAGS += -fsanitize=address
else
    CFLAGS += -O2
endif

# Directories
BUILD   := build
BIN     := bin

# =============================================================================
# SOURCES
# =============================================================================

CORE_sourceS := \
    source/core/lexer.c    \
    source/core/ast.c      \
    source/core/parser.c   \
    source/core/checker.c  \
    source/core/bytecode.c

COMPILER_sourceS := \
    source/compiler/compiler.c \
    source/compiler/xbc.c

VM_sourceS := \
    source/vm/vm.c

# =============================================================================
# OBJECT FILES
# Object files mirror the source tree under build/
# =============================================================================

CORE_OBJS     := $(patsubst source/%.c, $(BUILD)/%.o, $(CORE_sourceS))
COMPILER_OBJS := $(patsubst source/%.c, $(BUILD)/%.o, $(COMPILER_sourceS))
VM_OBJS       := $(patsubst source/%.c, $(BUILD)/%.o, $(VM_sourceS))

# =============================================================================
# STATIC LIBRARIES
#
# libxeno_core.a     — lexer, ast, parser, checker, bytecode
# libxeno_compiler.a — compiler, xbc (links against core)
# libxeno_vm.a       — vm (links against core + compiler for source mode)
# =============================================================================

LIB_CORE     := $(BUILD)/libxeno_core.a
LIB_COMPILER := $(BUILD)/libxeno_compiler.a
LIB_VM       := $(BUILD)/libxeno_vm.a

# =============================================================================
# FINAL BINARIES
# =============================================================================

XENOC   := $(BIN)/xenoc.exe
XENOVM  := $(BIN)/xenovm.exe

# =============================================================================
# DEFAULT TARGET
# =============================================================================

.PHONY: all
all: $(XENOC) $(XENOVM)

# =============================================================================
# DIRECTORY CREATION
# =============================================================================

$(BUILD)/core $(BUILD)/compiler $(BUILD)/vm $(BIN):
	mkdir -p $@

# =============================================================================
# COMPILE RULES
# Each .c -> .o, mirroring the source/ tree under build/
# =============================================================================

$(BUILD)/core/%.o: source/core/%.c | $(BUILD)/core
	$(CC) $(CFLAGS) -c $< -o $@

$(BUILD)/compiler/%.o: source/compiler/%.c | $(BUILD)/compiler
	$(CC) $(CFLAGS) -c $< -o $@

$(BUILD)/vm/%.o: source/vm/%.c | $(BUILD)/vm
	$(CC) $(CFLAGS) -c $< -o $@

# =============================================================================
# STATIC LIBRARIES
# =============================================================================

$(LIB_CORE): $(CORE_OBJS)
	ar rcs $@ $^

$(LIB_COMPILER): $(COMPILER_OBJS)
	ar rcs $@ $^

$(LIB_VM): $(VM_OBJS)
	ar rcs $@ $^

# =============================================================================
# BINARIES
#
# xenoc:   compiler entry point + libxeno_compiler + libxeno_core
# xenovm:  vm entry point + libxeno_vm + libxeno_compiler + libxeno_core
#          (xenovm includes compiler because it supports .xeno source files)
# =============================================================================

XENOC_MAIN  := $(BUILD)/compiler/xenoc_main.o
XENOVM_MAIN := $(BUILD)/vm/xenovm_main.o

$(XENOC_MAIN): source/compiler/xenoc_main.c | $(BUILD)/compiler
	$(CC) $(CFLAGS) -c $< -o $@

$(XENOVM_MAIN): source/vm/xenovm_main.c | $(BUILD)/vm
	$(CC) $(CFLAGS) -c $< -o $@

.PHONY: xenoc
xenoc: $(XENOC)

$(XENOC): $(XENOC_MAIN) $(LIB_COMPILER) $(LIB_CORE) | $(BIN)
	$(CC) $(CFLAGS) $^ -o $@ $(LDFLAGS)
	@echo "Built: $@"

.PHONY: xenovm
xenovm: $(XENOVM)

$(XENOVM): $(XENOVM_MAIN) $(LIB_VM) $(LIB_COMPILER) $(LIB_CORE) | $(BIN)
	$(CC) $(CFLAGS) $^ -o $@ $(LDFLAGS)
	@echo "Built: $@"

# =============================================================================
# TESTS
#
# Each test binary links against whatever libraries it needs.
# Running 'make tests' builds and runs all tests.
# Running 'make test_vm' builds and runs just that test.
# =============================================================================

TEST_BINS := \
    $(BUILD)/test_lexer    \
    $(BUILD)/test_parser   \
    $(BUILD)/test_checker  \
    $(BUILD)/test_compiler \
    $(BUILD)/test_vm       \
    $(BUILD)/test_xbc

$(BUILD)/test_lexer: tests/test_lexer.c $(LIB_CORE) | $(BUILD)
	$(CC) $(CFLAGS) $^ -o $@ $(LDFLAGS)

$(BUILD)/test_parser: tests/test_parser.c $(LIB_CORE) | $(BUILD)
	$(CC) $(CFLAGS) $^ -o $@ $(LDFLAGS)

$(BUILD)/test_checker: tests/test_checker.c $(LIB_CORE) | $(BUILD)
	$(CC) $(CFLAGS) $^ -o $@ $(LDFLAGS)

$(BUILD)/test_compiler: tests/test_compiler.c $(LIB_COMPILER) $(LIB_CORE) | $(BUILD)
	$(CC) $(CFLAGS) $^ -o $@ $(LDFLAGS)

$(BUILD)/test_vm: tests/test_vm.c $(LIB_VM) $(LIB_COMPILER) $(LIB_CORE) | $(BUILD)
	$(CC) $(CFLAGS) $^ -o $@ $(LDFLAGS)

$(BUILD)/test_xbc: tests/test_xbc.c $(LIB_VM) $(LIB_COMPILER) $(LIB_CORE) | $(BUILD)
	$(CC) $(CFLAGS) $^ -o $@ $(LDFLAGS)

# Individual test targets — build and run
.PHONY: test_lexer test_parser test_checker test_compiler test_vm test_xbc

test_lexer:    $(BUILD)/test_lexer
	@echo "" | $<

test_parser:   $(BUILD)/test_parser
	@echo "" | $<

test_checker:  $(BUILD)/test_checker
	@echo "" | $<

test_compiler: $(BUILD)/test_compiler
	@echo "" | $<

test_vm:       $(BUILD)/test_vm
	@echo "" | $<

test_xbc:      $(BUILD)/test_xbc
	@echo "" | $<

# Run all tests in sequence, stopping on first failure
.PHONY: tests
tests: $(TEST_BINS)
	@echo ""
	@echo "=========================================="
	@echo "  Running all XenoScript tests"
	@echo "=========================================="
	@echo "" | $(BUILD)/test_lexer    && echo "  [PASS] test_lexer"    || (echo "  [FAIL] test_lexer";    exit 1)
	@echo "" | $(BUILD)/test_parser   && echo "  [PASS] test_parser"   || (echo "  [FAIL] test_parser";   exit 1)
	@echo "" | $(BUILD)/test_checker  && echo "  [PASS] test_checker"  || (echo "  [FAIL] test_checker";  exit 1)
	@echo "" | $(BUILD)/test_compiler && echo "  [PASS] test_compiler" || (echo "  [FAIL] test_compiler"; exit 1)
	@echo "" | $(BUILD)/test_vm       && echo "  [PASS] test_vm"       || (echo "  [FAIL] test_vm";       exit 1)
	@echo "" | $(BUILD)/test_xbc      && echo "  [PASS] test_xbc"      || (echo "  [FAIL] test_xbc";      exit 1)
	@echo ""
	@echo "  All tests passed."
	@echo "=========================================="

# =============================================================================
# CLEAN
# =============================================================================

.PHONY: clean
clean:
	rm -rf $(BUILD) $(BIN)
	@echo "Cleaned."

# =============================================================================
# CONVENIENCE — print the project structure
# =============================================================================

.PHONY: tree
tree:
	@find . -not -path './build/*' -not -path './bin/*' -not -name '*.o' \
	        -not -name '*.a' | sort | sed 's|[^/]*/|  |g'

# =============================================================================
# XenoScript language tests (auto-generating expected outputs)
# =============================================================================

XENO_TEST_DIR := test
XENO_TESTS := $(wildcard $(XENO_TEST_DIR)/*.xeno)

.PHONY: xeno_tests
xeno_tests: $(XENOC) $(XENOVM)
	@echo ""
	@echo "=========================================="
	@echo "  Running XenoScript language tests"
	@echo "=========================================="
	@for testfile in $(XENO_TESTS); do \
		base=$${testfile%.xeno}; \
		outfile="$${base}.out"; \
		errfile="$${base}.err"; \
		echo "Running $$testfile"; \
		$(XENOC) $$testfile > "$${base}.compile.out" 2> "$${base}.compile.err"; \
		if [ -f "$${base}.xbc" ]; then \
			$(XENOVM) "$${base}.xbc" > "$${base}.run.out" 2> "$${base}.run.err"; \
		else \
			: > "$${base}.run.out"; \
			: > "$${base}.run.err"; \
		fi; \
		if [ ! -f "$$outfile" ]; then \
			echo "  [GEN] $$outfile"; \
			cp "$${base}.run.out" "$$outfile"; \
		fi; \
		if [ ! -f "$$errfile" ]; then \
			echo "  [GEN] $$errfile"; \
			cat "$${base}.compile.err" "$${base}.compile.out" "$${base}.run.err" > "$$errfile"; \
		fi; \
		cat "$${base}.run.out" | diff -u "$$outfile" - || exit 1; \
		cat "$${base}.compile.err" "$${base}.compile.out" "$${base}.run.err" | diff -u "$$errfile" - || exit 1; \
		rm -f "$${base}.compile.out" "$${base}.compile.err" "$${base}.run.out" "$${base}.run.err" "$${base}.xbc" \
		echo "  [PASS] $$testfile"; \
		echo ""; \
	done
	@echo "All XenoScript tests passed."