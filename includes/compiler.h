/*
 * compiler.h — Compiler interface
 *
 * The compiler takes a type-checked Program (AST) and produces a Module —
 * a collection of compiled Chunks, one per function.
 *
 * Prerequisites:
 *   - The AST must have passed the type checker (all resolved_types filled in)
 *   - If the type checker reported any errors, do NOT call the compiler
 *
 * Output:
 *   A Module containing one Chunk per function plus a function name table
 *   so the VM can resolve calls by name (for the host API) or index.
 */

#ifndef COMPILER_H
#define COMPILER_H

#include "ast.h"
#include "bytecode.h"
#include <stdbool.h>

/* ─────────────────────────────────────────────────────────────────────────────
 * MODULE — the compiler's output
 *
 * A module is the complete compiled form of a script file.
 * It contains one Chunk per function, in declaration order.
 * The VM loads a Module and executes it by calling the entry point
 * (conventionally the function named "main", index looked up at load time).
 * ───────────────────────────────────────────────────────────────────────────*/
#define MODULE_MAX_FUNCTIONS 256
#define MODULE_MAX_CLASSES   64

/* ── Mod metadata — populated from @Mod annotation, written to .xbc ────── */
#define MOD_STRING_MAX 128

typedef struct {
    bool has_mod;                       /* true if @Mod was found            */
    char name[MOD_STRING_MAX];          /* required                          */
    char version[MOD_STRING_MAX];       /* optional, empty string if absent  */
    char author[MOD_STRING_MAX];        /* optional, empty string if absent  */
    char description[MOD_STRING_MAX];   /* optional, empty string if absent  */
    char entry_class[MOD_STRING_MAX];   /* class name that bears @Mod        */
} ModMetadata;

typedef struct {
    Chunk      *chunks;                          /* heap-allocated array     */
    char        names[MODULE_MAX_FUNCTIONS][64]; /* Function names           */
    int         count;                           /* Number of functions      */
    int         capacity;                        /* Allocated chunk slots    */

    /* Class definitions — built by compiler, used by VM at runtime */
    ClassDef    classes[MODULE_MAX_CLASSES];
    int         class_count;

    /* Static initializer chunk index — runs before entry point.
     * -1 means no static initializers. Set by compiler_compile(). */
    int         sinit_index;

    /* Mod metadata — only populated if @Mod annotation was present */
    ModMetadata metadata;
} Module;

void   module_init(Module *m);
void   module_free(Module *m);
int    module_find(const Module *m, const char *name, int len); /* returns index or -1 */
int    module_find_class(const Module *m, const char *name);    /* returns index or -1 */
void   module_disassemble(const Module *m);   /* disassemble all chunks */
int    module_add_chunk(Module *m);           /* add and init a new chunk slot */

/*
 * Merge all chunks and class definitions from `src` into `dst`.
 * Used to graft stdlib .xar content into a user module before execution.
 * Functions already present in `dst` (by name) are skipped (no duplicates).
 * Returns true on success, false on allocation failure.
 */
bool   module_merge(Module *dst, const Module *src);


/* ─────────────────────────────────────────────────────────────────────────────
 * COMPILER ERRORS
 * ───────────────────────────────────────────────────────────────────────────*/
#define COMPILER_MAX_ERRORS 32

typedef struct {
    char message[256];
    int  line;
} CompileError;


/* Host function declaration for the compiler.
 * Must be called for every host function before compiler_compile(). */
typedef struct {
    const char *name;
    int         index;        /* Index in the VM's host_fns table */
    int         param_count;
    bool        has_any_param; /* If true, all args are auto-converted to string */
} CompilerHostDecl;

#define COMPILER_MAX_HOST_DECLS 256

typedef struct {
    CompilerHostDecl decls[COMPILER_MAX_HOST_DECLS];
    int count;
} CompilerHostTable;

void compiler_host_table_init(CompilerHostTable *t);
void compiler_host_table_add(CompilerHostTable *t,
                             const char *name, int index, int param_count);
void compiler_host_table_add_any(CompilerHostTable *t,
                                 const char *name, int index, int param_count);

/* ─────────────────────────────────────────────────────────────────────────────
 * LOCAL VARIABLE SLOT TRACKING
 *
 * The compiler maintains a flat list of locals for the current function.
 * Each entry maps a variable name to a slot index in the call frame.
 *
 * Scope is tracked by recording a "scope depth" — when a block opens we
 * push a marker, when it closes we discard all locals added since the marker.
 * The slot indices themselves don't change (the VM frame stays the same size);
 * we just stop tracking the names so future lookups don't find them.
 * ───────────────────────────────────────────────────────────────────────────*/
#define MAX_LOCALS 64

typedef struct {
    const char *name;    /* Points into the source string (no copy needed) */
    int         length;
    int         slot;    /* Index in the call frame                        */
    int         depth;   /* Scope depth when this local was declared       */
} LocalVar;


/* ─────────────────────────────────────────────────────────────────────────────
 * COMPILER STATE
 * ───────────────────────────────────────────────────────────────────────────*/
typedef struct {
    /* Output */
    Module *module;

    /* Current function being compiled */
    Chunk  *current_chunk;     /* Points into module->chunks[current_fn]  */
    int     current_fn;        /* Index of the current function           */

    /* Local variable table for current function */
    LocalVar locals[MAX_LOCALS];
    int      local_count;
    int      scope_depth;      /* Current nesting depth (0 = function top) */
    int      next_slot;        /* Next available slot index                */

    /* Loop context for break/continue */
    int      loop_start;       /* Bytecode offset of current loop condition */
    int      loop_depth;       /* How many loops we're nested inside        */

    /* Break/continue patch lists — one stack frame per nesting level.
     * When 'break' is compiled, we emit a forward JUMP and record its
     * offset here. When the loop ends we patch them all to jump past it.
     * Same for 'continue' but patched to jump to loop_start. */
#define MAX_LOOP_DEPTH   16
#define MAX_LOOP_PATCHES 64
    int  break_patches   [MAX_LOOP_DEPTH][MAX_LOOP_PATCHES];
    int  break_count     [MAX_LOOP_DEPTH];
    int  continue_patches[MAX_LOOP_DEPTH][MAX_LOOP_PATCHES];
    int  continue_count  [MAX_LOOP_DEPTH];

    /* Host function table — set by compiler_compile(), used in EXPR_CALL */
    const CompilerHostTable *host_table;

    /* Class compilation context.
     * current_class_idx is the index into module->classes[] of the class
     * currently being compiled. -1 when not inside a class.
     * current_class_ast points to the STMT_CLASS_DECL so we can resolve
     * field and method names to their indices. */
    int    current_class_idx;
    Stmt  *current_class_ast;

    /* Errors */
    CompileError errors[COMPILER_MAX_ERRORS];
    int          error_count;
    bool         had_error;
} Compiler;


/* ─────────────────────────────────────────────────────────────────────────────
 * PUBLIC INTERFACE
 * ───────────────────────────────────────────────────────────────────────────*/

/*
 * Compile a type-checked program into a Module.
 * Returns true on success, false if any compile errors occurred.
 * The module must be initialized before calling this.
 * host_table may be NULL if there are no host functions.
 */
bool compiler_compile(Compiler *c, const Program *program, Module *module,
                      const CompilerHostTable *host_table);

void compiler_print_errors(const Compiler *c);

#endif /* COMPILER_H */