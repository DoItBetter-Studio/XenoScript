# ==========================================================
# XenoScript Makefile
# Runs under WSL — targets Linux and Win64 via mingw
#
# Targets:
#   all         Build xenoc + xenovm for Linux and Win64
#   linux       Linux binaries only
#   win64       Win64 binaries only
#   stdlib      Rebuild stdlib .xar files and re-embed
#   xeno_tests  Run language tests
#   clean       Remove build/ and bin/
# ==========================================================

CC_LINUX  := /usr/bin/gcc
CC_WIN64  := /usr/bin/x86_64-w64-mingw32-gcc
LD_WIN64  := /usr/bin/x86_64-w64-mingw32-ld

CFLAGS    := -std=c17 -Wall -Wextra -Iincludes -O2 \
             -Wno-array-bounds \
             -Wno-maybe-uninitialized \
             -Wno-stringop-overflow
LDFLAGS   := -lm

BUILD     := build/xar
BIN       := bin

# ==========================================================
# Sources
# ==========================================================

CORE_SRCS := \
    source/core/lexer.c    \
    source/core/ast.c      \
    source/core/parser.c   \
    source/core/checker.c  \
    source/core/bytecode.c

COMPILER_SRCS := \
    source/compiler/compiler.c \
    source/compiler/xbc.c      \
    source/compiler/module_strip_stdlib.c

# Bootstrap strips module_strip_stdlib.c — it has no stdlib to reference
COMPILER_SRCS_BOOTSTRAP := \
    source/compiler/compiler.c \
    source/compiler/xbc.c

VM_SRCS  := source/vm/vm.c
XAR_SRCS := source/xar/xar.c source/xar/toml.c

STDLIB_DECLARE   := source/stdlib/stdlib_declare.c
STDLIB_REGISTER  := source/stdlib/stdlib_register.c
STDLIB_SOURCES_H := source/stdlib/stdlib_sources.h

XENOC_SRCS  := $(CORE_SRCS) $(COMPILER_SRCS) $(XAR_SRCS) $(STDLIB_DECLARE) source/compiler/xenoc_main.c
XENOVM_SRCS := $(CORE_SRCS) $(COMPILER_SRCS) $(VM_SRCS) $(XAR_SRCS) $(STDLIB_REGISTER) source/vm/xenovm_main.c
XAR_TOOL_SRCS := $(CORE_SRCS) $(COMPILER_SRCS) $(XAR_SRCS) $(STDLIB_DECLARE) source/xar/xar_main.c
XAR_BOOTSTRAP_SRCS := $(CORE_SRCS) $(COMPILER_SRCS_BOOTSTRAP) $(XAR_SRCS) $(STDLIB_DECLARE) source/xar/xar_main.c

LSP_SRCS := $(CORE_SRCS) $(COMPILER_SRCS) $(XAR_SRCS) $(STDLIB_DECLARE) \
    source/lsp/jsonrpc.c \
    source/lsp/json.c    \
    source/lsp/stub_gen.c \
    source/lsp/doc_store.c \
    source/lsp/lsp_main.c

# Embedded stdlib objects — one set per target platform
STDLIB_OBJS_LINUX := $(BUILD)/core.xar.linux.o $(BUILD)/math.xar.linux.o $(BUILD)/collections.xar.linux.o
STDLIB_OBJS_WIN64 := $(BUILD)/core.xar.win64.o $(BUILD)/math.xar.win64.o $(BUILD)/collections.xar.win64.o

# ==========================================================
# Default
# ==========================================================

.PHONY: all linux win64
all: linux win64

linux: $(BIN)/xenoc $(BIN)/xenovm $(BIN)/xar $(BIN)/xenolsp
win64: $(BIN)/xenoc.exe $(BIN)/xenovm.exe $(BIN)/xar.exe $(BIN)/xenolsp.exe

# ==========================================================
# Bootstrap: xar tool, stage 1 — no stdlib embedded.
# Used only to pack the stdlib XARs. Compiles with
# -DXAR_BOOTSTRAP so module_strip_stdlib is skipped.
# ==========================================================

$(BIN)/xar-bootstrap: $(XAR_BOOTSTRAP_SRCS) $(STDLIB_SOURCES_H) | $(BIN)
	@printf "🔧 Building xar bootstrap...\n"
	@$(CC_LINUX) $(CFLAGS) -DXAR_BOOTSTRAP $(XAR_BOOTSTRAP_SRCS) -o $@ $(LDFLAGS)

# ==========================================================
# Stdlib: pack → embed (uses bootstrap xar)
# ==========================================================

$(STDLIB_SOURCES_H): $(wildcard stdlib/collections/*.xeno) $(wildcard stdlib/core/*.xeno)
	@printf "🐍 Regenerating stdlib_sources.h...\n"
	@python3 source/stdlib/gen_stdlib.py

$(BUILD)/core.xar: stdlib/core/*.xeno | $(BIN)/xar-bootstrap $(BUILD)
	@$(BIN)/xar-bootstrap pack stdlib/core/ -o $@ -n core -v 1.0.0

$(BUILD)/math.xar: stdlib/math/*.xeno | $(BIN)/xar-bootstrap $(BUILD)
	@$(BIN)/xar-bootstrap pack stdlib/math/ -o $@ -n math -v 1.0.0

$(BUILD)/collections.xar: stdlib/collections/*.xeno | $(BIN)/xar-bootstrap $(BUILD)
	@$(BIN)/xar-bootstrap pack stdlib/collections/ -o $@ -n collections -v 1.0.0

# Linux embed (ELF) — add .note.GNU-stack to suppress executable-stack warning
$(BUILD)/%.xar.linux.o: $(BUILD)/%.xar
	@printf "📦 Embedding %s (linux)\n" $<
	@ld -r -b binary $< -o $@.tmp
	@objcopy --add-section .note.GNU-stack=/dev/null $@.tmp $@
	@rm $@.tmp

# Win64 embed (PE)
$(BUILD)/%.xar.win64.o: $(BUILD)/%.xar
	@printf "📦 Embedding %s (win64)\n" $<
	@$(LD_WIN64) -r -b binary $< -o $@

.PHONY: stdlib
stdlib: $(STDLIB_OBJS_LINUX) $(STDLIB_OBJS_WIN64)
	@printf "✅ Stdlib rebuilt.\n"

# ==========================================================
# Final xar tool — with stdlib embedded, stdlib strip enabled
# ==========================================================

$(BIN)/xar: $(XAR_TOOL_SRCS) $(STDLIB_OBJS_LINUX) $(STDLIB_SOURCES_H) | $(BIN)
	@printf "🔧 Building xar tool...\n"
	@$(CC_LINUX) $(CFLAGS) $(XAR_TOOL_SRCS) $(STDLIB_OBJS_LINUX) -o $@ $(LDFLAGS)

$(BIN)/xar.exe: $(XAR_TOOL_SRCS) $(STDLIB_OBJS_WIN64) $(STDLIB_SOURCES_H) | $(BIN)
	@printf "🪟 Building xar.exe (win64)...\n"
	@$(CC_WIN64) $(CFLAGS) $(XAR_TOOL_SRCS) $(STDLIB_OBJS_WIN64) -o $@ $(LDFLAGS)

# ==========================================================
# Linux Binaries
# ==========================================================

$(BIN)/xenoc: $(XENOC_SRCS) $(STDLIB_OBJS_LINUX) $(STDLIB_SOURCES_H) | $(BIN)
	@printf "🐧 Linking xenoc (linux)...\n"
	@$(CC_LINUX) $(CFLAGS) $(XENOC_SRCS) $(STDLIB_OBJS_LINUX) -o $@ $(LDFLAGS)

$(BIN)/xenolsp: $(LSP_SRCS) $(STDLIB_OBJS_LINUX) $(STDLIB_SOURCES_H) | $(BIN)
	@printf "🐧 Linking xenolsp (linux)...\n"
	@$(CC_LINUX) $(CFLAGS) $(LSP_SRCS) $(STDLIB_OBJS_LINUX) -o $@ $(LDFLAGS)

$(BIN)/xenovm: $(XENOVM_SRCS) $(STDLIB_OBJS_LINUX) | $(BIN)
	@printf "🐧 Linking xenovm (linux)...\n"
	@$(CC_LINUX) $(CFLAGS) $(XENOVM_SRCS) $(STDLIB_OBJS_LINUX) -o $@ $(LDFLAGS)

# ==========================================================
# Win64 Binaries
# ==========================================================

$(BIN)/xenoc.exe: $(XENOC_SRCS) $(STDLIB_OBJS_WIN64) $(STDLIB_SOURCES_H) | $(BIN)
	@printf "🪟 Linking xenoc.exe (win64)...\n"
	@$(CC_WIN64) $(CFLAGS) $(XENOC_SRCS) $(STDLIB_OBJS_WIN64) -o $@ $(LDFLAGS)

$(BIN)/xenolsp.exe: $(LSP_SRCS) $(STDLIB_OBJS_WIN64) $(STDLIB_SOURCES_H) | $(BIN)
	@printf "🪟 Linking xenolsp.exe (win64)...\n"
	@$(CC_WIN64) $(CFLAGS) $(LSP_SRCS) $(STDLIB_OBJS_WIN64) -o $@ $(LDFLAGS)

$(BIN)/xenovm.exe: $(XENOVM_SRCS) $(STDLIB_OBJS_WIN64) | $(BIN)
	@printf "🪟 Linking xenovm.exe (win64)...\n"
	@$(CC_WIN64) $(CFLAGS) $(XENOVM_SRCS) $(STDLIB_OBJS_WIN64) -o $@ $(LDFLAGS)

# ==========================================================
# Directories
# ==========================================================

$(BUILD) $(BIN):
	@mkdir -p $@

# ==========================================================
# Tests
# ==========================================================

.PHONY: xeno_tests
xeno_tests: $(BIN)/xenoc $(BIN)/xenovm
	@python3 source/tools/run_tests.py $(BIN)/xenoc $(BIN)/xenovm test/

# ==========================================================
# Clean
# ==========================================================

.PHONY: clean
clean:
	rm -rf build bin
	@printf "🧽 Cleaned.\n"
