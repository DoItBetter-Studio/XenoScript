/*
 * ast.h — Abstract Syntax Tree node definitions
 *
 * This file defines the data structures that represent a parsed program.
 * Every node in the tree is one of two things:
 *
 *   Expr  — something that produces a value  (has a type)
 *   Stmt  — something that performs an action (no value)
 *
 * Both use the tagged-union pattern: an enum tag identifies the variant,
 * and a union holds the variant-specific data.
 *
 * TYPE INFORMATION
 * ----------------
 * Because this language is statically typed, EVERY expression node carries
 * a `resolved_type` field. This starts as TYPE_UNKNOWN and gets filled in
 * by the type checker (Phase 4). By the time the compiler (Phase 6) sees
 * the AST, every expression has a known, verified type. This is what lets
 * the compiler emit the RIGHT bytecode instruction — e.g. OP_ADD_INT vs
 * OP_ADD_FLOAT — without any runtime type checking in the VM.
 *
 * MEMORY
 * ------
 * All nodes are allocated from an Arena (see arena.h). The AST is entirely
 * temporary — it exists only during compilation and is freed en masse when
 * the compiler finishes. The VM never sees it.
 */

#ifndef AST_H
#define AST_H

#include "token.h"
#include "arena.h"
#include <stdint.h>
#include <stdbool.h>

/* Forward declaration — Expr and Stmt reference each other */
typedef struct Expr Expr;
typedef struct Stmt Stmt;


/* ═════════════════════════════════════════════════════════════════════════════
 * THE TYPE SYSTEM
 *
 * These are the types the language supports. The type checker resolves every
 * expression to one of these. TYPE_UNKNOWN is the initial state before
 * resolution — if any node still has TYPE_UNKNOWN after the type checker runs,
 * that's a bug in our compiler.
 * ═════════════════════════════════════════════════════════════════════════════*/
typedef enum {
    TYPE_UNKNOWN = 0,   /* Unresolved — should never survive the type checker */
    TYPE_VOID,          /* No value — only valid as a function return type     */
    TYPE_BOOL,          /* true / false                                        */
    TYPE_INT,           /* 64-bit signed integer                               */
    TYPE_FLOAT,         /* 64-bit double precision float                       */
    TYPE_STRING,        /* Immutable string — heap-allocated in the VM         */
    TYPE_OBJECT,        /* Instance of a user-defined class                    */
    TYPE_ENUM,          /* A named enum type — backed by int at runtime        */
    TYPE_CLASS_REF,     /* Compile-time only: a bare class name used as lvalue
                         * (e.g. "MyClass" in "MyClass.staticField").
                         * Never appears at runtime -- always rewritten away.  */
    /* Numeric types with distinct bit-widths */
    TYPE_SBYTE,         /* int8_t   -- signed 8-bit   */
    TYPE_BYTE,          /* uint8_t  -- unsigned 8-bit  */
    TYPE_SHORT,         /* int16_t  -- signed 16-bit   */
    TYPE_USHORT,        /* uint16_t -- unsigned 16-bit */
    TYPE_UINT,          /* uint32_t -- unsigned 32-bit */
    TYPE_LONG,          /* int64_t  -- signed 64-bit (alias for int) */
    TYPE_ULONG,         /* uint64_t -- unsigned 64-bit */
    TYPE_DOUBLE,        /* double   -- float alias     */
    TYPE_CHAR,          /* uint32_t -- Unicode codepoint */
    TYPE_ANY,           /* Wildcard -- used only for host fn param declarations */
    TYPE_ARRAY,         /* Homogeneous array — element_type holds the elem type */
} TypeKind;

/* Human-readable type names for error messages */
const char *type_kind_name(TypeKind kind);

/*
 * A full type descriptor.
 *
 * Right now this is just a wrapper around TypeKind, but having it as a
 * struct from the start means we can extend it later without changing
 * every function signature. Future additions:
 *   - Array types:    int[]
 *   - Function types: fn(int, float) -> bool
 *   - Struct types:   user-defined records
 *
 * We add those fields here as placeholders so you can see where it's going.
 */
typedef struct Type {
    TypeKind       kind;
    const char    *class_name;    /* Non-null only when kind == TYPE_OBJECT.  */
    const char    *enum_name;     /* Non-null only when kind == TYPE_ENUM.    */
    struct Type   *element_type;  /* Non-null only when kind == TYPE_ARRAY.   */
} Type;

/* Convenience constructors — these live in ast.c */
Type type_void(void);
Type type_bool(void);
Type type_int(void);
Type type_float(void);
Type type_string(void);
Type type_object(const char *class_name);     /* kind=TYPE_OBJECT,    name set */
Type type_enum(const char *enum_name);        /* kind=TYPE_ENUM,      name set */
Type type_sbyte(void);
Type type_byte(void);
Type type_short(void);
Type type_ushort(void);
Type type_uint(void);
Type type_long(void);
Type type_ulong(void);
Type type_double(void);
Type type_char(void);
Type type_any(void);
Type type_array(struct Type *element_type);
bool type_is_int_family(Type t);
bool type_is_unsigned(Type t);
struct ArgNode;  /* forward decl for expr_array_lit */
Expr *expr_array_lit    (Arena *a, struct ArgNode *elements, int count, int line);
Expr *expr_index        (Arena *a, Expr *array, Expr *index, int line);
Expr *expr_index_assign (Arena *a, Expr *array, Expr *index, Expr *value, int line);
Expr *expr_new_array    (Arena *a, Type element_type, Expr *length, int line);
Expr *expr_is         (Arena *a, Expr *operand, Type check_type, int line);
Expr *expr_as         (Arena *a, Expr *operand, Type check_type, int line);
Expr *expr_typeof     (Arena *a, Expr *operand, int line);
Type type_class_ref(const char *class_name);  /* kind=TYPE_CLASS_REF, name set */

/* Are two types the same? */
bool type_equals(Type a, Type b);

/* Is this type a numeric type (int or float)? Used by type checker. */
bool type_is_numeric(Type t);


/* ═════════════════════════════════════════════════════════════════════════════
 * EXPRESSIONS
 *
 * An expression is anything that produces a value.
 * Examples: 42, x, x + y, foo(1, 2), true, "hello"
 *
 * Every Expr carries a `resolved_type` that starts TYPE_UNKNOWN and is
 * filled in by the type checker. The compiler uses this to decide which
 * bytecode instructions to emit.
 * ═════════════════════════════════════════════════════════════════════════════*/
typedef enum {
    /* Literals — values baked directly into the source */
    EXPR_INT_LIT,
    EXPR_CHAR_LIT,       /* 42                    */
    EXPR_FLOAT_LIT,     /* 3.14                  */
    EXPR_BOOL_LIT,      /* true / false          */
    EXPR_STRING_LIT,    /* "hello"               */
    EXPR_INTERP_STRING, /* $"Hello, {name}!"     */

    /* Variables */
    EXPR_IDENT,         /* x  — a variable reference   */

    /* Operations */
    EXPR_UNARY,         /* -x  or  !flag         */
    EXPR_BINARY,        /* x + y, x == y, etc.   */
    EXPR_POSTFIX,       /* x++  or  x--          */

    /* Assignment is an expression — it produces the assigned value.
     * This lets you write:  int x = y = 5;
     * (though we may restrict this later for clarity) */
    EXPR_ASSIGN,        /* x = expr              */

    /* Function call */
    EXPR_CALL,          /* foo(a, b, c)          */

    /* Object expressions */
    EXPR_NEW,           /* new Foo(a, b)         */
    EXPR_FIELD_GET,     /* obj.field             */
    EXPR_SUPER_CALL,    /* super(args)           — call parent constructor */
    EXPR_FIELD_SET,     /* obj.field = value     */
    EXPR_METHOD_CALL,   /* obj.method(a, b)      */
    EXPR_ENUM_ACCESS,   /* Direction.North — resolved at compile time to int */
    EXPR_STATIC_GET,    /* ClassName.field       — resolved static field read  */
    EXPR_STATIC_SET,    /* ClassName.field = val — resolved static field write */
    EXPR_STATIC_CALL,   /* ClassName.method(...) — resolved static method call */
    EXPR_THIS,          /* this                  */
    EXPR_ARRAY_LIT,     /* {1, 2, 3}             — array literal          */
    EXPR_INDEX,         /* arr[i]                — element read            */
    EXPR_INDEX_ASSIGN,  /* arr[i] = v            — element write           */
    EXPR_NEW_ARRAY,     /* new int[n]            — fixed-size allocation   */
    EXPR_IS,            /* expr is TypeName      — runtime type check      */
    EXPR_AS,            /* expr as TypeName      — runtime cast            */
    EXPR_TYPEOF,        /* typeof(expr)          — type descriptor         */

} ExprKind;

/*
 * Argument list for function calls.
 * A linked list of expressions — simple and arena-friendly.
 */
typedef struct ArgNode {
    Expr           *expr;
    struct ArgNode *next;
} ArgNode;

/*
 * THE EXPRESSION NODE
 *
 * All expression variants share the same struct. The `kind` field tells
 * you which union member to access. Accessing the wrong union member is
 * undefined behavior, so always switch on `kind` first.
 *
 * `resolved_type` is filled by the type checker. Before that pass, it holds
 * TYPE_UNKNOWN. After that pass, it holds the verified type of this expression.
 *
 * `line` is the source line — carried through for error messages at every stage.
 */
struct Expr {
    ExprKind kind;
    Type     resolved_type;   /* Filled in by the type checker */
    int      line;            /* Source line for error messages */

    union {
        /* EXPR_INT_LIT */
        struct {
            int64_t value;
        } int_lit;

        /* EXPR_CHAR_LIT -- Unicode codepoint, stored as uint32_t */
        struct {
            uint32_t value;
        } char_lit;

        /* EXPR_FLOAT_LIT */
        struct {
            double value;
        } float_lit;

        /* EXPR_BOOL_LIT */
        struct {
            bool value;
        } bool_lit;

        /* EXPR_STRING_LIT
         * We store the raw source pointer and length (no null terminator).
         * The compiler will intern/copy the string when emitting bytecode. */
        struct {
            const char *chars;   /* Points into the source string */
            int         length;  /* Does NOT include surrounding quotes */
        } string_lit;

        /* EXPR_INTERP_STRING — $"Hello, {name}! You have {count} items."
         * Stores a linked list of segments: alternating text and expressions.
         * At runtime all segments are converted to strings and concatenated. */
        struct {
            /* Linked list of InterpSegment nodes, in source order */
            struct InterpSegment {
                bool  is_expr;          /* true  → expr node; false → text  */
                /* Text segment: */
                const char *text;       /* cooked text (escapes resolved)   */
                int         text_len;
                /* Expr segment: */
                struct Expr *expr;
                struct InterpSegment *next;
            } *segments;
            int segment_count;
        } interp_string;

        /* EXPR_IDENT
         * We store the name as (pointer, length) into the source — same
         * zero-copy approach as the lexer. The type checker will resolve
         * this name to a symbol table entry. */
        struct {
            const char *name;
            int         length;
        } ident;

        /* EXPR_UNARY
         * op is one of: TOK_MINUS (negation), TOK_BANG (logical not) */
        struct {
            TokenType  op;
            Expr      *operand;
        } unary;

        /* EXPR_BINARY
         * op is any arithmetic, comparison, or logical operator token */
        struct {
            TokenType  op;
            Expr      *left;
            Expr      *right;
        } binary;

        /* EXPR_POSTFIX — x++ or x-- (and prefix ++x / --x)
         * op is TOK_PLUS_PLUS or TOK_MINUS_MINUS.
         * is_prefix=false: postfix — evaluates to old value, then mutates.
         * is_prefix=true:  prefix  — mutates first, then evaluates to new value.
         * is_field=false: simple variable lvalue — name/length identify it.
         * is_field=true:  field lvalue — object.field_name identifies it. */
        struct {
            TokenType  op;
            bool       is_prefix;  /* true => ++x / --x  */
            bool       is_field;   /* true => obj.field lvalue */
            bool       is_static_field; /* true => ClassName.staticField lvalue */
            /* Simple variable lvalue (is_field == false) */
            const char *name;
            int         length;
            /* Field lvalue (is_field == true) */
            Expr       *object;
            const char *field_name;
            int         field_name_len;
        } postfix;

        /* EXPR_ASSIGN */
        struct {
            const char *name;     /* Target variable name  */
            int         length;   /* Name length           */
            Expr       *value;    /* Right-hand side       */
        } assign;

        /* EXPR_CALL */
        struct {
            const char *name;       /* Function name           */
            int         length;     /* Name length             */
            ArgNode    *args;       /* Linked list of arguments */
            int         arg_count;  /* How many arguments       */
        } call;

        /* EXPR_NEW — new ClassName(args) */
        struct {
            const char *class_name;
            int         class_name_len;
            ArgNode    *args;
            int         arg_count;
        } new_expr;

        /* EXPR_SUPER_CALL — super(args)
         * Calls the parent class constructor from within a child constructor.
         * The parent class name is resolved by the checker from current_class. */
        struct {
            ArgNode    *args;
            int         arg_count;
        } super_call;

        /* EXPR_FIELD_GET — obj.field
         * object is the expression on the left of the dot. */
        struct {
            Expr       *object;
            const char *field_name;
            int         field_name_len;
        } field_get;

        /* EXPR_FIELD_SET — obj.field = value */
        struct {
            Expr       *object;
            const char *field_name;
            int         field_name_len;
            Expr       *value;
        } field_set;

        /* EXPR_METHOD_CALL — obj.method(args) */
        struct {
            Expr       *object;
            const char *method_name;
            int         method_name_len;
            ArgNode    *args;
            int         arg_count;
        } method_call;

        /* EXPR_ENUM_ACCESS — Direction.North
         * Resolved by the checker to the member's integer value.
         * The compiler emits a LOAD_CONST_INT. */
        struct {
            const char *enum_name;
            int         enum_name_len;
            const char *member_name;
            int         member_name_len;
            int         value;   /* filled in by checker */
        } enum_access;

        /* EXPR_STATIC_GET — ClassName.field
         * Resolved by the checker. Compiler emits OP_LOAD_STATIC. */
        struct {
            const char *class_name;
            int         class_name_len;
            const char *field_name;
            int         field_name_len;
            int         class_idx;   /* index into Module->classes[] */
            int         field_idx;   /* index into ClassDef->fields[] */
        } static_get;

        /* EXPR_STATIC_SET — ClassName.field = value
         * Resolved by the checker. Compiler emits OP_STORE_STATIC. */
        struct {
            const char *class_name;
            int         class_name_len;
            const char *field_name;
            int         field_name_len;
            Expr       *value;
            int         class_idx;
            int         field_idx;
        } static_set;

        /* EXPR_STATIC_CALL — ClassName.method(args)
         * Resolved by the checker. Compiler emits OP_CALL with fn_index. */
        struct {
            const char *class_name;
            int         class_name_len;
            const char *method_name;
            int         method_name_len;
            ArgNode    *args;
            int         arg_count;
            int         fn_index;   /* index into Module->chunks */
        } static_call;

        /* EXPR_THIS — no extra data, type resolved to current class */

        /* EXPR_ARRAY_LIT — {e0, e1, e2, ...} */
        struct {
            struct ArgNode *elements;
            int             count;
        } array_lit;

        /* EXPR_INDEX — arr[index] */
        struct {
            Expr *array;
            Expr *index;
        } index_expr;

        /* EXPR_INDEX_ASSIGN — arr[index] = value */
        struct {
            Expr *array;
            Expr *index;
            Expr *value;
        } index_assign;

        /* EXPR_NEW_ARRAY — new ElementType[n] */
        struct {
            Type  element_type;
            Expr *length;
        } new_array;

        /* EXPR_IS / EXPR_AS
         * operand is the sub-expression; check_type is the target type.
         * is  → bool result  as  → cast or runtime error */
        struct {
            Expr *operand;
            Type  check_type;
        } type_op;

        /* EXPR_TYPEOF
         * operand is the sub-expression; result is a XenoType* */
        struct {
            Expr *operand;
        } type_of;

    };
};


/* ═════════════════════════════════════════════════════════════════════════════
 * STATEMENTS
 *
 * A statement performs an action. It doesn't produce a value.
 * Statements form the "spine" of the program — expressions hang off them.
 * ═════════════════════════════════════════════════════════════════════════════*/
/* ── Annotation key-value pair ──────────────────────────────────────────────
 * Represents one key="value" argument inside an annotation, e.g. name="My Mod" */
typedef struct AnnotationKVNode {
    const char             *key;        /* key name (not null-terminated)   */
    int                     key_len;
    const char             *value;      /* string value (not null-terminated) */
    int                     value_len;
    struct AnnotationKVNode *next;
} AnnotationKVNode;

/* ── Annotation ─────────────────────────────────────────────────────────────
 * Represents one @Name(key="value", ...) annotation on a declaration. */
typedef struct AnnotationNode {
    const char         *name;       /* annotation name, e.g. "Mod"          */
    int                 name_len;
    AnnotationKVNode   *args;       /* linked list of key=value pairs        */
    struct AnnotationNode *next;
} AnnotationNode;

/* ── Access level for class members ────────────────────────────────────────
 * Used by ClassFieldNode and ClassMethodNode to record which section
 * (public:, private:, protected:) the member was declared under. */
typedef enum {
    ACCESS_PUBLIC    = 0,  /* default — visible everywhere             */
    ACCESS_PRIVATE   = 1,  /* visible only within the declaring class  */
    ACCESS_PROTECTED = 2,  /* visible within the class and subclasses  */
} AccessLevel;

typedef enum {
    /* Variable declaration with mandatory type annotation.
     * Because we're statically typed, you can't write: x = 5;
     * You must write:                                   int x = 5;
     * The type is explicit. The type checker verifies the initializer matches. */
    STMT_VAR_DECL,      /* int x = expr;           */

    /* A bare expression used as a statement — typically a function call.
     * e.g.: print("hello");
     * The expression's value is computed and discarded. */
    STMT_EXPR,          /* expr;                   */

    /* Control flow */
    STMT_IF,            /* if (cond) { } else { }          */
    STMT_WHILE,         /* while (cond) { }                */
    STMT_FOR,           /* for (init; cond; step)          */
    STMT_FOREACH,       /* foreach (Type var in array)     */
    STMT_MATCH,         /* match (expr) { case X: ... }    */
    STMT_RETURN,        /* return expr;            */
    STMT_BREAK,         /* break;                  */
    STMT_CONTINUE,      /* continue;               */

    /* A block is a sequence of statements with its own scope.
     * Declared variables inside a block are not visible outside it. */
    STMT_BLOCK,         /* { stmt* }               */

    /* Function declaration.
     * Functions are statements at the top level — you declare them,
     * you don't evaluate them. The body is a block statement. */
    STMT_FN_DECL,       /* function foo(int x): int { }    */

    /* Class declaration — top-level only */
    STMT_CLASS_DECL,    /* class Foo { fields; methods; }  */

    STMT_ENUM_DECL,     /* enum Direction { North, South = 2, ... } */

} StmtKind;

/*
 * Parameter list for function declarations.
 * A linked list of (type, name) pairs.
 */
typedef struct ParamNode {
    Type             type;
    const char      *name;
    int              length;
    struct ParamNode *next;
} ParamNode;

/*
 * A linked list of statements — used for block bodies and program top-level.
 */
typedef struct StmtNode {
    Stmt            *stmt;
    struct StmtNode *next;
} StmtNode;

/*
 * THE STATEMENT NODE
 */
struct Stmt {
    StmtKind kind;
    int      line;

    union {
        /* STMT_VAR_DECL
         * The `type` is what the programmer wrote: int x = ...
         * The type checker will verify that `init->resolved_type` matches. */
        struct {
            Type        type;         /* Declared type (from source)     */
            const char *name;         /* Variable name                   */
            int         length;       /* Name length                     */
            Expr       *init;         /* Initializer expression (or NULL
                                         if declared without a value)    */
        } var_decl;

        /* STMT_EXPR */
        struct {
            Expr *expr;
        } expr;

        /* STMT_IF */
        struct {
            Expr *condition;
            Stmt *then_branch;    /* Always a STMT_BLOCK */
            Stmt *else_branch;    /* NULL if no else, otherwise STMT_BLOCK */
        } if_stmt;

        /* STMT_WHILE */
        struct {
            Expr *condition;
            Stmt *body;           /* Always a STMT_BLOCK */
        } while_stmt;

        /* STMT_FOR
         * We store for-loop parts directly rather than desugaring to while
         * here in the AST — desugaring will happen in the compiler.
         * Keeping the original structure makes error messages better. */
        struct {
            Stmt *init;       /* STMT_VAR_DECL or STMT_EXPR, or NULL */
            Expr *condition;  /* NULL means infinite loop             */
            Expr *step;       /* Post-iteration expression, or NULL   */
            Stmt *body;       /* Always a STMT_BLOCK                  */
        } for_stmt;

        struct {
            Type        elem_type;
            const char *var_name;
            int         var_len;
            Expr       *array;
            Stmt       *body;
        } foreach_stmt;

        /* STMT_MATCH
         * match (subject) { case Pattern: body  case ...: ... default: ... }
         *
         * Each arm is a MatchArmNode carrying:
         *   is_default   — true for the 'default:' arm
         *   pattern      — the expression to compare against (enum member or int
         *                  literal); NULL when is_default is true
         *   body         — a STMT_BLOCK (even single-statement arms are wrapped)
         *
         * Fall-through is C-style: execution continues into the next arm's body
         * unless the arm ends with 'break'. The compiler emits a jump-past-match
         * at each break. */
        struct {
            Expr       *subject;     /* The expression being matched    */
            struct MatchArmNode {
                bool        is_default;
                Expr       *pattern;  /* NULL if is_default              */
                Stmt       *body;     /* Always STMT_BLOCK               */
                struct MatchArmNode *next;
            } *arms;
            int         arm_count;
            bool        has_default;
        } match_stmt;

        /* STMT_RETURN */
        struct {
            Expr *value;    /* NULL for bare 'return;' in void functions */
        } return_stmt;

        /* STMT_BREAK / STMT_CONTINUE — no extra data needed */

        /* STMT_BLOCK */
        struct {
            StmtNode *stmts;  /* Linked list of statements in this block */
        } block;

        /* STMT_FN_DECL */
        struct {
            Type        return_type;   /* Declared return type          */
            const char *name;          /* Function name                 */
            int         length;        /* Name length                   */
            ParamNode  *params;        /* Linked list of parameters     */
            int         param_count;   /* How many parameters           */
            Stmt       *body;          /* Always a STMT_BLOCK           */
        } fn_decl;

        /* STMT_CLASS_DECL */
        struct {
            const char     *name;           /* Class name                        */
            int             length;

            /* Annotations: @Mod(...), @Event(...), etc. — linked list */
            AnnotationNode *annotations;

            /* Parent class — NULL if none */
            const char *parent_name;
            int         parent_length;

            /* Fields: stored as a simple array (arena-allocated) */
            struct ClassFieldNode {
                Type        type;
                const char *name;
                int         length;
                bool        is_static;
                AccessLevel access;   /* public / private / protected */
                Expr       *initializer;  /* NULL if no initializer, e.g. int x = 42; */
                struct ClassFieldNode *next;
            } *fields;
            int field_count;

            /* Methods and constructors stored as fn_decl statements */
            struct ClassMethodNode {
                Stmt       *fn;          /* STMT_FN_DECL */
                bool        is_static;
                AccessLevel access;      /* public / private / protected */
                bool        is_constructor;
                struct ClassMethodNode *next;
            } *methods;
            int method_count;
        } class_decl;

        /* STMT_ENUM_DECL */
        struct {
            const char *name;       /* Enum type name, e.g. "Direction"  */
            int         length;

            /* Members stored as a linked list */
            struct EnumMemberNode {
                const char *name;       /* Member name, e.g. "North"         */
                int         length;
                int         value;      /* Integer value (auto or explicit)  */
                bool        has_explicit_value;
                struct EnumMemberNode *next;
            } *members;
            int member_count;
        } enum_decl;
    };
};


/* ═════════════════════════════════════════════════════════════════════════════
 * THE PROGRAM
 *
 * The root of the AST. A program is just a list of top-level statements.
 * In our language, the only valid top-level statements are:
 *   - Function declarations (STMT_FN_DECL)
 *   - Global variable declarations (STMT_VAR_DECL)  [future]
 *
 * We store this as a linked list of StmtNodes rather than a dynamic array
 * to keep arena allocation simple — each StmtNode is just one arena_alloc().
 * ═════════════════════════════════════════════════════════════════════════════*/
typedef struct {
    StmtNode *stmts;   /* Linked list of top-level statements */
    int       count;   /* How many top-level statements       */
} Program;


/* ═════════════════════════════════════════════════════════════════════════════
 * CONSTRUCTOR HELPERS
 *
 * These functions allocate and initialize AST nodes from an arena.
 * The parser will call these — it never touches the union fields directly.
 * This centralizes all node initialization in one place.
 * ═════════════════════════════════════════════════════════════════════════════*/
#include "arena.h"

/* Expressions */
Expr *expr_int_lit   (Arena *a, int64_t value,           int line);
Expr *expr_char_lit  (Arena *a, uint32_t value,          int line);
Expr *expr_float_lit (Arena *a, double value,            int line);
Expr *expr_bool_lit  (Arena *a, bool value,              int line);
Expr *expr_string_lit(Arena *a, const char *chars, int len, int line);
Expr *expr_interp_string(Arena *a, int line);
Expr *expr_ident     (Arena *a, const char *name,  int len, int line);
Expr *expr_unary     (Arena *a, TokenType op, Expr *operand,        int line);
Expr *expr_postfix   (Arena *a, TokenType op, const char *name, int length, int line);
Expr *expr_postfix_field(Arena *a, TokenType op, bool is_prefix,
                         Expr *object, const char *field_name, int field_name_len, int line);
Expr *expr_prefix    (Arena *a, TokenType op, const char *name, int length, int line);
Expr *expr_binary    (Arena *a, TokenType op, Expr *left, Expr *right, int line);
Expr *expr_assign    (Arena *a, const char *name, int len, Expr *value, int line);
Expr *expr_call      (Arena *a, const char *name, int len, ArgNode *args, int count, int line);
Expr *expr_new       (Arena *a, const char *class_name, int class_len,
                      ArgNode *args, int count, int line);
Expr *expr_super_call(Arena *a, ArgNode *args, int count, int line);
Expr *expr_field_get (Arena *a, Expr *object, const char *field, int field_len, int line);
Expr *expr_field_set (Arena *a, Expr *object, const char *field, int field_len,
                      Expr *value, int line);
Expr *expr_method_call(Arena *a, Expr *object, const char *method, int method_len,
                       ArgNode *args, int count, int line);
Expr *expr_this      (Arena *a, int line);

/* Statements */
Stmt *stmt_var_decl  (Arena *a, Type type, const char *name, int len, Expr *init, int line);
Stmt *stmt_expr      (Arena *a, Expr *expr, int line);
Stmt *stmt_if        (Arena *a, Expr *cond, Stmt *then_b, Stmt *else_b, int line);
Stmt *stmt_while     (Arena *a, Expr *cond, Stmt *body,   int line);
Stmt *stmt_for       (Arena *a, Stmt *init, Expr *cond, Expr *step, Stmt *body, int line);
Stmt *stmt_foreach  (Arena *a, Type elem_type, const char *var_name, int var_len,
                     Expr *array, Stmt *body, int line);
Stmt *stmt_match     (Arena *a, Expr *subject, int line);
Stmt *stmt_return    (Arena *a, Expr *value, int line);
Stmt *stmt_break     (Arena *a, int line);
Stmt *stmt_continue  (Arena *a, int line);
Stmt *stmt_block     (Arena *a, StmtNode *stmts, int line);
Stmt *stmt_fn_decl   (Arena *a, Type ret, const char *name, int len,
                      ParamNode *params, int param_count, Stmt *body, int line);
Stmt *stmt_class_decl(Arena *a, const char *name, int len,
                      const char *parent_name, int parent_len,
                      int line);
Stmt *stmt_enum_decl (Arena *a, const char *name, int len, int line);

/* List node helpers */
StmtNode  *stmt_node  (Arena *a, Stmt *stmt,  StmtNode *next);
ArgNode   *arg_node   (Arena *a, Expr *expr,  ArgNode  *next);
ParamNode *param_node (Arena *a, Type type,   const char *name, int len, ParamNode *next);

/* Debug: print the entire AST as indented text — invaluable for debugging */
void ast_print_program(const Program *program);
void ast_print_stmt(const Stmt *stmt, int indent);
void ast_print_expr(const Expr *expr, int indent);

#endif /* AST_H */