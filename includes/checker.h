/*
 * checker.h — Type checker and symbol table
 *
 * The type checker performs two jobs in one AST walk:
 *
 *   1. Symbol table management — tracks what variables and functions exist
 *      in each scope, and what their types are.
 *
 *   2. Type resolution and verification — fills in resolved_type on every
 *      Expr node, and verifies that types are compatible wherever they meet
 *      (assignments, binary ops, return statements, function calls, etc.)
 *
 * After this pass, every Expr node has a valid resolved_type (no TYPE_UNKNOWN
 * remaining), OR had_error is true and the program should not be compiled.
 *
 * DESIGN: No implicit type coercion. int + float is an error. This keeps
 * the type rules simple, the compiler straightforward, and modder code
 * explicit and readable.
 */

#ifndef CHECKER_H
#define CHECKER_H

#include "ast.h"
#include "arena.h"
#include <stdbool.h>

/* ─────────────────────────────────────────────────────────────────────────────
 * SYMBOL TABLE
 *
 * A symbol is one entry in the table — a name bound to a type.
 * We distinguish between variable symbols and function symbols because
 * functions carry extra information (parameter types, return type) that
 * the call-site type checker needs.
 * ───────────────────────────────────────────────────────────────────────────*/

#define MAX_PARAMS      16    /* Maximum parameters per function             */
#define SCOPE_MAX_SYMS  64   /* Maximum symbols per scope level             */
#define MAX_SCOPE_DEPTH 32    /* Maximum nesting depth (scopes on the stack) */
#define CHECKER_MAX_ERRORS 64

typedef enum {
    SYM_VAR,    /* A variable: has a single type                  */
    SYM_FN,     /* A function: has a return type and param types   */
    SYM_CLASS,  /* A class name: type is TYPE_OBJECT               */
    SYM_ENUM,   /* An enum name: type is TYPE_ENUM                 */
} SymbolKind;

typedef struct {
    SymbolKind  kind;

    /* The symbol's name — stored as (pointer, length) into the source.
     * We never copy identifier strings; we compare them with memcmp. */
    const char *name;
    int         length;

    /* For SYM_VAR: the variable's type.
     * For SYM_FN:  the function's return type. */
    Type        type;

    /* For SYM_FN only — parameter types in declaration order.
     * Variables leave these zeroed. */
    Type        param_types[MAX_PARAMS];
    int         param_count;

    /* For SYM_CLASS only — pointer back to the STMT_CLASS_DECL AST node.
     * Used by field/method lookup in the type checker.
     * NULL for non-class symbols. */
    struct Stmt *class_decl;

    /* For SYM_CLASS only — null-terminated copy of the class name, safe
     * to pass to strlen/strcmp. Source tokens are NOT null-terminated. */
    char class_name_buf[64];

    /* For SYM_ENUM only — pointer back to the STMT_ENUM_DECL AST node.
     * Used by member lookup in the type checker. */
    struct Stmt *enum_decl;

    /* For SYM_ENUM only — null-terminated copy of the enum name. */
    char enum_name_buf[64];
} Symbol;

/*
 * A scope is a flat array of symbols.
 * We use a fixed-size array per scope — dynamic arrays would require
 * allocation, and 256 symbols per scope is more than enough for any
 * realistic mod script function.
 */
typedef struct {
    Symbol symbols[SCOPE_MAX_SYMS];
    int    count;
} Scope;

/* ─────────────────────────────────────────────────────────────────────────────
 * CHECKER STATE
 * ───────────────────────────────────────────────────────────────────────────*/

typedef struct {
    char message[256];
    int  line;
    bool is_warning;   /* true → printed as warning, does not block compilation */
} CheckError;

typedef struct {
    /* Arena for allocating synthetic AST nodes (e.g. implicit 'this' nodes
     * when a bare field name is used inside a class method). Points to the
     * parser's arena — valid for the lifetime of the checker pass. */
    Arena *arena;

    /* Scope stack — index 0 is global, higher indices are more nested */
    Scope scopes[MAX_SCOPE_DEPTH];
    int   scope_depth;   /* Index of the CURRENT (innermost) scope */

    /* The return type of the function currently being type-checked. */
    Type  current_fn_return_type;
    bool  inside_function;

    /* When type-checking a class method or constructor, this points to
     * the class declaration AST node so we can resolve 'this', fields,
     * and methods. NULL when not inside a class. */
    Stmt *current_class;    /* STMT_CLASS_DECL or NULL */

    /* Loop depth — tracks whether break/continue are valid */
    int   loop_depth;

    /* Match depth — tracks whether break is valid inside a match arm */
    int   match_depth;

    /* Set to true while type-checking the body of a static method.
     * Prevents implicit 'this' rewrites and blocks 'this' expressions. */
    bool  in_static_method;

    /* Error accumulation */
    CheckError errors[CHECKER_MAX_ERRORS];
    int        error_count;
    bool       had_error;
} Checker;


/* ─────────────────────────────────────────────────────────────────────────────
 * PUBLIC INTERFACE
 * ───────────────────────────────────────────────────────────────────────────*/

/* Initialize a fresh checker */
void checker_init(Checker *checker, Arena *arena);

/*
 * Pre-declare a host function so the type checker knows it exists.
 * Call this for every host function BEFORE calling checker_check().
 * return_type: the function's return type
 * param_types: array of param types, param_count entries
 */
void checker_declare_host(Checker *checker,
                          const char *name,
                          Type return_type,
                          Type *param_types,
                          int param_count);

/*
 * Type-check an entire program.
 *
 * Modifies the AST in place — fills in resolved_type on every Expr node.
 * Returns true if the program is well-typed, false if any errors were found.
 *
 * If this returns false, the AST is in a partially-resolved state and must
 * NOT be passed to the compiler.
 */
bool checker_check(Checker *checker, Program *program);

/* Print all type errors to stderr */
void checker_print_errors(const Checker *checker);

#endif /* CHECKER_H */