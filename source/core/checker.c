/*
 * checker.c — Type checker implementation
 *
 * Walk order:
 *   1. checker_check() does a first pass over top-level statements to
 *      register all function signatures in global scope (so forward calls work).
 *   2. checker_check() does a second pass to type-check all function bodies.
 *
 * Within a function body, statements are checked top-to-bottom and
 * expressions are checked bottom-up (children before parents), which is
 * the natural order for type inference: you need to know the types of
 * sub-expressions before you can determine the type of the parent.
 */

#include "checker.h"
#include "bytecode.h"
#include <stdio.h>
#include <string.h>
#include <stdarg.h>
#include <limits.h>

#define MAX_TYPE_ARGS 8   /* Maximum generic type arguments per instantiation */

/* ─────────────────────────────────────────────────────────────────────────────
 * INTERNAL: ERROR REPORTING
 * ───────────────────────────────────────────────────────────────────────────*/

static void type_error(Checker *c, int line, const char *fmt, ...) {
    if (c->error_count >= CHECKER_MAX_ERRORS) return;

    CheckError *e = &c->errors[c->error_count++];
    e->line       = line;
    e->is_warning = false;
    c->had_error  = true;

    va_list args;
    va_start(args, fmt);
    vsnprintf(e->message, sizeof(e->message), fmt, args);
    va_end(args);
}

static void type_warning(Checker *c, int line, const char *fmt, ...) {
    if (c->error_count >= CHECKER_MAX_ERRORS) return;

    CheckError *e = &c->errors[c->error_count++];
    e->line       = line;
    e->is_warning = true;
    /* Do NOT set c->had_error — warnings don't block compilation */

    va_list args;
    va_start(args, fmt);
    vsnprintf(e->message, sizeof(e->message), fmt, args);
    va_end(args);
}

/* ─────────────────────────────────────────────────────────────────────────────
 * INTERNAL: SCOPE MANAGEMENT
 *
 * The scope stack works like this:
 *
 *   scope_depth = 0  →  we are at global scope (scopes[0])
 *   scope_depth = 1  →  we are inside one function (scopes[1])
 *   scope_depth = 2  →  we are inside a nested block (scopes[2])
 *   ...etc
 *
 * push_scope() increments depth and clears the new scope's symbol count.
 * pop_scope()  decrements depth, discarding all symbols declared in it.
 * ───────────────────────────────────────────────────────────────────────────*/

static void push_scope(Checker *c) {
    c->scope_depth++;
    if (c->scope_depth >= MAX_SCOPE_DEPTH) {
        /* This would be an absurdly deeply nested script.
         * In practice mod scripts will never hit this. */
        c->scope_depth = MAX_SCOPE_DEPTH - 1;
        return;
    }
    c->scopes[c->scope_depth].count = 0;
}

static void pop_scope(Checker *c) {
    if (c->scope_depth > 0) c->scope_depth--;
}

/*
 * define_symbol — add a symbol to the CURRENT (innermost) scope.
 * Returns false if the symbol name is already defined in this exact scope
 * (shadowing a symbol from an outer scope is allowed; redefining in the
 * same scope is not).
 */
static bool define_symbol(Checker *c, Symbol sym) {
    Scope *scope = &c->scopes[c->scope_depth];

    /* Check for duplicate in current scope only */
    for (int i = 0; i < scope->count; i++) {
        Symbol *s = &scope->symbols[i];
        if (s->length == sym.length &&
            memcmp(s->name, sym.name, sym.length) == 0)
        {
            return false;  /* Already defined in this scope */
        }
    }

    if (scope->count >= SCOPE_MAX_SYMS) return false;  /* Scope full */

    scope->symbols[scope->count++] = sym;
    return true;
}

/* Set definition location on a symbol — call before define_symbol(). */
static inline void sym_set_loc(Symbol *sym, const char *file, int line, int col) {
    sym->def_file = (char *)file;   /* borrowed — do not free */
    sym->def_line = line;
    sym->def_col  = col;
}

/* Record a symbol usage at the given source location.
 * Called whenever an identifier is resolved to a symbol during checking.
 * Silently does nothing if the usage table is full or sym is NULL. */
static void record_usage(Checker *c, Symbol *sym, int line, int col, int length) {
    if (!sym || !c || c->usage_count >= CHECKER_MAX_USAGES) return;
    UsageRecord *r = &c->usages[c->usage_count++];
    r->file   = c->source_file;
    r->line   = line;
    r->col    = col;
    r->length = length;
    r->sym    = sym;
}

/*
 * lookup_symbol — search for a name from the innermost scope outward.
 * Returns a pointer to the Symbol if found, NULL if not declared.
 *
 * This implements lexical scoping: inner declarations shadow outer ones
 * because we search from the top of the stack downward.
 */
static Symbol *lookup_symbol(Checker *c, const char *name, int length) {
    /* Search from innermost scope outward */
    for (int d = c->scope_depth; d >= 0; d--) {
        Scope *scope = &c->scopes[d];
        for (int i = 0; i < scope->count; i++) {
            Symbol *s = &scope->symbols[i];
            if (s->length == length &&
                memcmp(s->name, name, length) == 0)
            {
                return s;
            }
        }
    }
    return NULL;  /* Not found in any scope */
}

/* ─────────────────────────────────────────────────────────────────────────────
 * INTERNAL: TYPE CHECKING — EXPRESSIONS
 *
 * check_expr walks an expression bottom-up, fills in resolved_type on
 * every node, and returns that type to the caller.
 *
 * Returning the type (rather than just setting it) lets the parent node
 * use it directly without having to re-read it from the child.
 * ───────────────────────────────────────────────────────────────────────────*/

static Type check_expr(Checker *c, Expr *expr);  /* forward decl */

/*
 * resolve — set the expression's type and return it.
 * All paths through check_expr end with this call.
 */
static Type resolve(Expr *expr, Type t) {
    expr->resolved_type = t;
    return t;
}

/*
 * error_type — report a type error and return TYPE_UNKNOWN so the parent
 * node can continue (and potentially find more errors) without crashing.
 * TYPE_UNKNOWN propagates upward without triggering additional errors —
 * the parent sees "unknown" and skips its own type check.
 */
static Type error_type(Checker *c, Expr *expr, int line,
                        const char *fmt, ...) {
    if (c->error_count < CHECKER_MAX_ERRORS) {
        CheckError *e = &c->errors[c->error_count++];
        e->line       = line;
        c->had_error  = true;
        va_list args;
        va_start(args, fmt);
        vsnprintf(e->message, sizeof(e->message), fmt, args);
        va_end(args);
    }
    expr->resolved_type.kind = TYPE_UNKNOWN;
    return expr->resolved_type;
}

/*
 * is_unknown — returns true if a type is TYPE_UNKNOWN, meaning an error
 * already occurred in a sub-expression. Used to suppress cascading errors:
 * if a child already errored, we don't need to report another error on
 * the parent — that would just confuse the modder with redundant messages.
 */
static bool is_unknown(Type t) { return t.kind == TYPE_UNKNOWN; }

/* Returns true if `t` can legally hold a null value.
 * A type is nullable if declared with ?, or if it is the null literal type. */
static bool type_is_nullable(Type t) __attribute__((unused));
static bool type_is_nullable(Type t) {
    return t.is_nullable || t.kind == TYPE_NULL;
}

/* Returns true if null (TYPE_NULL) is assignable to `target`.
 * Null is assignable to any nullable type (TYPE_NULL is excluded — you
 * can't declare a variable of type null). */
static bool null_assignable_to(Type target) __attribute__((unused));
static bool null_assignable_to(Type target) {
    return target.is_nullable;
}

/* Returns true if `from` is assignment-compatible with `to`, taking
 * nullability into account:
 *   - null  -> nullable type  : ok
 *   - T     -> T?             : ok (auto-wrap)
 *   - T     -> T              : ok
 *   - T?    -> T              : ERROR (must unwrap first with ! or ??)
 *   - null  -> T (non-null)   : ERROR */
static bool nullable_assignable(Type from, Type to) __attribute__((unused));
static bool nullable_assignable(Type from, Type to) {
    if (from.kind == TYPE_NULL)
        return to.is_nullable;
    /* Strip nullability for kind comparison */
    Type from_base = from; from_base.is_nullable = false;
    Type to_base   = to;   to_base.is_nullable   = false;
    if (!type_equals(from_base, to_base)) return false;
    /* Non-nullable assigned to nullable: ok */
    if (!from.is_nullable && to.is_nullable)  return true;
    /* Both same nullability: ok */
    if (from.is_nullable == to.is_nullable)   return true;
    /* Nullable assigned to non-nullable: NOT ok */
    return false;
}

/* Returns true if `expr` is a compile-time constant expression.
 * Valid: literals, unary negation of a literal, enum member access,
 *        static field references (MyClass.field), array literals of consts.
 * Invalid: function calls, new, binary ops, instance field access, this. */
static bool is_const_expr(const Expr *expr) {
    if (!expr) return false;
    switch (expr->kind) {
        case EXPR_INT_LIT:
        case EXPR_FLOAT_LIT:
        case EXPR_STRING_LIT:
        case EXPR_BOOL_LIT:
        case EXPR_ENUM_ACCESS:
        case EXPR_STATIC_GET:
            return true;
        case EXPR_UNARY:
            /* Allow negation of numeric literals: -1, -3.14 */
            if (expr->unary.op == TOK_MINUS || expr->unary.op == TOK_BANG)
                return is_const_expr(expr->unary.operand);
            return false;
        case EXPR_ARRAY_LIT: {
            /* Array literal is const if all elements are const */
            struct ArgNode *el = expr->array_lit.elements;
            for (; el; el = el->next)
                if (!is_const_expr(el->expr)) return false;
            return true;
        }
        default:
            return false;
    }
}

/* Count the minimum number of arguments required (params without defaults). */
static int count_required_params(ParamNode *params) {
    int n = 0;
    for (ParamNode *p = params; p; p = p->next)
        if (!p->default_value) n++;
    return n;
}

/* Count total params. */
static int count_params(ParamNode *params) {
    int n = 0;
    for (ParamNode *p = params; p; p = p->next) n++;
    return n;
}

/* Returns the bit-width rank of an integer type (for widening rules).
 * Higher rank = wider type. */
static int int_type_rank(TypeKind k) {
    switch (k) {
        case TYPE_SBYTE:  return 1;
        case TYPE_BYTE:   return 2;
        case TYPE_SHORT:  return 3;
        case TYPE_USHORT: return 4;
        case TYPE_UINT:   return 5;
        case TYPE_INT:    return 6;
        case TYPE_LONG:   return 7;
        case TYPE_ULONG:  return 8;
        case TYPE_CHAR:   return 2; /* uint32 codepoint — treated like ushort */
        default:          return 6; /* default to int */
    }
}

/* Returns the bit-width in bytes of an integer type (for overflow warnings). */
static int int_type_bytes(TypeKind k) {
    switch (k) {
        case TYPE_SBYTE:  return 1;
        case TYPE_BYTE:   return 1;
        case TYPE_SHORT:  return 2;
        case TYPE_USHORT: return 2;
        case TYPE_UINT:   return 4;
        case TYPE_CHAR:   return 4;
        case TYPE_INT:    return 8;
        case TYPE_LONG:   return 8;
        case TYPE_ULONG:  return 8;
        default:          return 8;
    }
}

/* Returns the wider of two integer types for arithmetic result typing. */
static Type type_wider_int(Type a, Type b) {
    return int_type_rank(a.kind) >= int_type_rank(b.kind) ? a : b;
}

/* Returns true if assigning from type `from` to type `to` could overflow.
 * Only relevant when both are int-family and `to` is narrower than `from`. */
static bool may_overflow(TypeKind to, TypeKind from) {
    Type t_to   = (Type){.kind=to,   .class_name=NULL, .enum_name=NULL};
    Type t_from = (Type){.kind=from, .class_name=NULL, .enum_name=NULL};
    if (!type_is_int_family(t_to) || !type_is_int_family(t_from)) return false;
    return int_type_bytes(to) < int_type_bytes(from);
}

/* Returns true if `rhs` is assignable to `lhs`.
 * Rules:
 *  - Exact type match always works.
 *  - Any int-family type is assignable to any other int-family type
 *    (wraps on overflow — caller should warn if narrowing).
 *  - double <-> float: compatible (same storage).
 *  - Object subtype: Dog assignable to Animal.
 * Returns false for everything else (int->float, char->string, etc.). */
/* ─────────────────────────────────────────────────────────────────────────────
 * GENERICS HELPERS
 *
 * Forward declarations needed by generic helpers:
 */
static bool is_subtype(Checker *c, const char *child_name, const char *parent_name);
static bool class_implements_interface(Checker *c, const char *class_name, const char *iface_name);

/*
 * substitute_type    - Replace TYPE_PARAM occurrences with concrete types.
 * generic_base_name  - Extract "Stack" from "Stack<int>".
 * parse_canonical_type_args - Parse type args out of "Stack<int,string>".
 * check_type_args_against_params - Validate arg count and constraints.
 */

/*
 * substitute_type — given a type (which may be TYPE_PARAM "T"), a list of
 * TypeParamNodes, and a parallel array of concrete types, return the
 * substituted type.  Non-param types are returned as-is.
 */
/*
 * build_type_params_from_def - given a ClassDef loaded from a .xar (no AST),
 * build a linked list of synthetic TypeParamNodes from type_param_names[].
 * Used so substitute_type works for xar-loaded generic classes (List, Dict, etc).
 */
static TypeParamNode *build_type_params_from_def(Checker *c, const ClassDef *def) {
    if (!def || def->type_param_count == 0) return NULL;
    TypeParamNode *head = NULL, *tail = NULL;
    for (int i = 0; i < def->type_param_count; i++) {
        TypeParamNode *tp = arena_alloc(c->arena, sizeof(TypeParamNode));
        if (!tp) break;
        memset(tp, 0, sizeof(TypeParamNode));
        tp->name   = def->type_param_names[i];
        tp->length = (int)strlen(def->type_param_names[i]);
        tp->next   = NULL;
        if (!head) { head = tail = tp; }
        else       { tail->next = tp; tail = tp; }
    }
    return head;
}

static Type substitute_type(Type t,
                             TypeParamNode *params, Type *args, int arg_count)
{
    if (t.kind == TYPE_PARAM && t.param_name) {
        TypeParamNode *p = params;
        int i = 0;
        while (p && i < arg_count) {
            if (strncmp(t.param_name, p->name, p->length) == 0 &&
                t.param_name[p->length] == '\0')
            {
                return args[i];
            }
            p = p->next; i++;
        }
    }
    /* Also handle TYPE_OBJECT whose class_name matches a type param name.
     * This happens when the parser sees "T" as an identifier and creates
     * type_object("T") — the checker hasn't resolved it to TYPE_PARAM yet. */
    if (t.kind == TYPE_OBJECT && t.class_name) {
        TypeParamNode *p = params;
        int i = 0;
        while (p && i < arg_count) {
            if (strncmp(t.class_name, p->name, p->length) == 0 &&
                t.class_name[p->length] == '\0')
            {
                return args[i];
            }
            p = p->next; i++;
        }
    }
    if (t.kind == TYPE_ARRAY && t.element_type) {
        Type subst = substitute_type(*t.element_type, params, args, arg_count);
        if (subst.kind != t.element_type->kind ||
            subst.class_name != t.element_type->class_name)
        {
            /* Need a heap-allocated element_type — use a static scratchpad.
             * Fine because this only happens at check time, not runtime. */
            static Type scratch;
            scratch = subst;
            Type arr = type_array(&scratch);
            return arr;
        }
    }
    return t;
}

/*
 * generic_base_name — Given a canonical generic type name like "Stack<int>"
 * or "Pair<int,string>", extract just "Stack" into buf (max buf_size bytes,
 * null-terminated).  Returns true on success, false if not a generic name.
 */
static bool generic_base_name(const char *full_name, char *buf, int buf_size) {
    if (!full_name) return false;
    const char *lt = strchr(full_name, '<');
    if (!lt) return false;
    int len = (int)(lt - full_name);
    if (len <= 0 || len >= buf_size) return false;
    memcpy(buf, full_name, len);
    buf[len] = '\0';
    return true;
}

/*
 * is_generic_type_name — returns true if the class_name looks like a
 * canonical generic instantiation (contains '<').
 */
static bool is_generic_type_name(const char *name) {
    return name && strchr(name, '<') != NULL;
}

/*
 * parse_canonical_type_args — Given "Stack<int>" or "Pair<int,string>",
 * fill out_args[] (max max_args entries) with the concrete Type values.
 * Returns the number of type args found.
 *
 * Type names recognised: int, float, bool, string, void, and bare identifiers
 * (treated as TYPE_OBJECT). This mirrors parse_base_type's capabilities.
 */
static int parse_canonical_type_args(const char *full_name,
                                     Type *out_args, int max_args)
{
    const char *lt = strchr(full_name, '<');
    if (!lt) return 0;
    const char *p = lt + 1;
    int count = 0;

    while (*p && *p != '>' && count < max_args) {
        /* Skip whitespace */
        while (*p == ' ') p++;
        if (*p == '>') break;

        /* Read a type name token (letters/digits/underscore) */
        const char *start = p;
        while (*p && *p != ',' && *p != '>' && *p != ' ') p++;
        int len = (int)(p - start);

        Type t;
        if      (len == 3 && strncmp(start, "int",    3) == 0) t = type_int();
        else if (len == 5 && strncmp(start, "float",  5) == 0) t = type_float();
        else if (len == 4 && strncmp(start, "bool",   4) == 0) t = type_bool();
        else if (len == 6 && strncmp(start, "string", 6) == 0) t = type_string();
        else if (len == 4 && strncmp(start, "void",   4) == 0) t = type_void();
        else if (len == 5 && strncmp(start, "sbyte",  5) == 0) t = type_sbyte();
        else if (len == 4 && strncmp(start, "byte",   4) == 0) t = type_byte();
        else if (len == 5 && strncmp(start, "short",  5) == 0) t = type_short();
        else if (len == 6 && strncmp(start, "ushort", 6) == 0) t = type_ushort();
        else if (len == 4 && strncmp(start, "uint",   4) == 0) t = type_uint();
        else if (len == 4 && strncmp(start, "long",   4) == 0) t = type_long();
        else if (len == 5 && strncmp(start, "ulong",  5) == 0) t = type_ulong();
        else if (len == 6 && strncmp(start, "double", 6) == 0) t = type_double();
        else if (len == 4 && strncmp(start, "char",   4) == 0) t = type_char();
        else {
            /* Identifier — treat as TYPE_OBJECT.  Build a stable string. */
            static char name_bufs[8][64]; /* small pool, enough for typical usage */
            static int  pool_idx = 0;
            char *nbuf = name_bufs[pool_idx++ % 8];
            int   nlen = len < 63 ? len : 63;
            memcpy(nbuf, start, nlen);
            nbuf[nlen] = '\0';
            t = type_object(nbuf);
        }
        out_args[count++] = t;

        while (*p == ' ') p++;
        if (*p == ',') p++;
    }
    return count;
}

/*
 * check_type_constraint — returns true if `concrete` satisfies the
 * constraint named `constraint` (either same class/interface or implements it).
 */
static bool check_type_constraint(Checker *c, Type concrete,
                                  const char *constraint, int constraint_len)
{
    if (!constraint || constraint_len == 0) return true;

    char cbuf[64];
    int clen = constraint_len < 63 ? constraint_len : 63;
    memcpy(cbuf, constraint, clen);
    cbuf[clen] = '\0';

    /* TYPE_OBJECT: check if it IS the constraint, is a subclass, or implements it */
    if (concrete.kind == TYPE_OBJECT && concrete.class_name) {
        if (strcmp(concrete.class_name, cbuf) == 0) return true;
        if (is_subtype(c, concrete.class_name, cbuf)) return true;
        if (class_implements_interface(c, concrete.class_name, cbuf)) return true;
    }
    return false;
}

static bool types_assignable(const Type lhs, const Type rhs) {
    /* null literal is assignable to any nullable type */
    if (rhs.kind == TYPE_NULL) return lhs.is_nullable;
    /* Non-nullable rhs assignable to nullable lhs (same base type) */
    if (lhs.is_nullable && !rhs.is_nullable) {
        Type lb = lhs; lb.is_nullable = false;
        Type rb = rhs; rb.is_nullable = false;
        if (type_equals(lb, rb)) return true;
    }
    if (type_equals(lhs, rhs)) return true;
    /* Array: element types must be assignable */
    if (lhs.kind == TYPE_ARRAY && rhs.kind == TYPE_ARRAY) {
        if (!lhs.element_type || !rhs.element_type) return false;
        return types_assignable(*lhs.element_type, *rhs.element_type);
    }
    /* Int family: any combination is allowed (wrap semantics) */
    if (type_is_int_family(lhs) && type_is_int_family(rhs)) return true;
    /* float <-> double: same runtime storage */
    if ((lhs.kind == TYPE_FLOAT || lhs.kind == TYPE_DOUBLE) &&
        (rhs.kind == TYPE_FLOAT || rhs.kind == TYPE_DOUBLE)) return true;
    /* Generic erasure: List<string> declared, List assigned (or vice versa).
     * Strip type args from both sides and compare base names. */
    if (lhs.kind == TYPE_OBJECT && rhs.kind == TYPE_OBJECT &&
        lhs.class_name && rhs.class_name) {
        char lbase[64], rbase[64];
        bool l_generic = generic_base_name(lhs.class_name, lbase, sizeof(lbase));
        bool r_generic = generic_base_name(rhs.class_name, rbase, sizeof(rbase));
        if (l_generic || r_generic) {
            const char *lname = l_generic ? lbase : lhs.class_name;
            const char *rname = r_generic ? rbase : rhs.class_name;
            if (strcmp(lname, rname) == 0) return true;
        }
    }
    return false;
}

/* Emit an overflow warning if assigning `rhs_kind` into a narrower `lhs_kind`. */
static void warn_if_narrowing(Checker *c, int line,
                               TypeKind lhs_kind, TypeKind rhs_kind) {
    if (may_overflow(lhs_kind, rhs_kind)) {
        type_warning(c, line,
            "Narrowing conversion from %s to %s — value will wrap on overflow",
            type_kind_name(rhs_kind), type_kind_name(lhs_kind));
    }
}

/* ─────────────────────────────────────────────────────────────────────────────
 * UNIFIED CLASS LOOKUP HELPERS
 *
 * These abstract over AST (class_decl) vs binary ClassDef (class_def) so that
 * XAR-loaded generic classes (Dictionary<K,V>, List<T>, etc.) work identically
 * to source-merged ones.  Every checker call site should use these instead of
 * reaching directly into sym->class_decl->class_decl.fields/methods.
 *
 * type_params / concrete_args / concrete_count describe the instantiation
 * context, e.g. for a List<Enemy> variable:
 *   type_params    = [T]          (from ClassDef.type_param_names or AST)
 *   concrete_args  = [TYPE_OBJECT "Enemy"]
 *   concrete_count = 1
 * Pass NULL / 0 for non-generic classes.
 * ───────────────────────────────────────────────────────────────────────────*/

/*
 * checker_class_type_context — given a resolved TYPE_OBJECT class_name like
 * "Dictionary<string,int>" and the base Symbol, fill out the type parameter
 * context (params list + concrete arg array) needed for substitution.
 * Returns the concrete_count written into out_args[].
 */
static int checker_class_type_context(Checker *c,
                                       const char *full_class_name,
                                       Symbol *cls_sym,
                                       TypeParamNode **out_params,
                                       Type out_args[MAX_TYPE_ARGS])
{
    *out_params = NULL;
    int concrete_count = 0;

    /* Build type param list from whichever source is available */
    if (cls_sym->class_decl) {
        *out_params = cls_sym->class_decl->class_decl.type_params;
    } else if (cls_sym->class_def) {
        *out_params = build_type_params_from_def(c, (ClassDef *)cls_sym->class_def);
    }
    if (!*out_params) return 0;

    /* Parse concrete args out of "Dictionary<string,int>" */
    concrete_count = parse_canonical_type_args(full_class_name, out_args, MAX_TYPE_ARGS);
    return concrete_count;
}

/*
 * checker_field_type — find a field by name on a class and return its type,
 * with generic type parameters substituted using the provided context.
 * Returns TYPE_UNKNOWN if not found.
 */
static Type checker_field_type(Symbol *cls_sym,
                                const char *field_name, int field_len,
                                TypeParamNode *type_params,
                                Type *concrete_args, int concrete_count)
{
    /* AST path */
    if (cls_sym->class_decl) {
        typedef struct ClassFieldNode CFN;
        for (CFN *f = cls_sym->class_decl->class_decl.fields; f; f = f->next) {
            if (f->length == field_len &&
                memcmp(f->name, field_name, field_len) == 0) {
                if (type_params && concrete_count > 0)
                    return substitute_type(f->type, type_params,
                                           concrete_args, concrete_count);
                return f->type;
            }
        }
    }
    /* ClassDef path (XAR-loaded) */
    if (cls_sym->class_def) {
        ClassDef *def = (ClassDef *)cls_sym->class_def;
        for (int i = 0; i < def->field_count; i++) {
            if ((int)strlen(def->fields[i].name) == field_len &&
                memcmp(def->fields[i].name, field_name, field_len) == 0) {
                /* Reconstruct a Type from the FieldDef */
                Type ft;
                memset(&ft, 0, sizeof(ft));
                ft.kind = (TypeKind)def->fields[i].type_kind;
                ft.is_nullable = def->fields[i].is_nullable;
                if (ft.kind == TYPE_OBJECT && def->fields[i].class_name[0])
                    ft.class_name = def->fields[i].class_name;
                if (type_params && concrete_count > 0)
                    return substitute_type(ft, type_params,
                                           concrete_args, concrete_count);
                return ft;
            }
        }
    }
    Type unk; memset(&unk, 0, sizeof(unk)); unk.kind = TYPE_UNKNOWN;
    return unk;
}

/*
 * checker_method_return_type — find a method by name on a class and return
 * its return type, with generic substitution applied.
 * Returns TYPE_UNKNOWN if not found.
 */
static Type checker_method_return_type(Symbol *cls_sym,
                                        const char *method_name, int method_len,
                                        TypeParamNode *type_params,
                                        Type *concrete_args, int concrete_count)
{
    /* AST path */
    if (cls_sym->class_decl) {
        typedef struct ClassMethodNode CMN;
        for (CMN *m = cls_sym->class_decl->class_decl.methods; m; m = m->next) {
            if (!m->fn) continue;
            const char *mn = m->fn->fn_decl.name;
            int         ml = m->fn->fn_decl.length;
            if (ml == method_len && memcmp(mn, method_name, method_len) == 0) {
                Type rt = m->fn->fn_decl.return_type;
                if (type_params && concrete_count > 0)
                    return substitute_type(rt, type_params,
                                           concrete_args, concrete_count);
                return rt;
            }
        }
    }
    /* ClassDef path (XAR-loaded) */
    if (cls_sym->class_def) {
        ClassDef *def = (ClassDef *)cls_sym->class_def;
        for (int i = 0; i < def->method_count; i++) {
            if ((int)strlen(def->methods[i].name) == method_len &&
                memcmp(def->methods[i].name, method_name, method_len) == 0) {
                Type rt;
                memset(&rt, 0, sizeof(rt));
                rt.kind = (TypeKind)def->methods[i].return_type_kind;
                rt.is_nullable = def->methods[i].return_is_nullable;
                if (rt.kind == TYPE_OBJECT && def->methods[i].return_class_name[0])
                    rt.class_name = def->methods[i].return_class_name;
                if (type_params && concrete_count > 0)
                    return substitute_type(rt, type_params,
                                           concrete_args, concrete_count);
                return rt;
            }
        }
    }
    Type unk; memset(&unk, 0, sizeof(unk)); unk.kind = TYPE_UNKNOWN;
    return unk;
}

/* ── LSP helper functions — used by completion/signature-help (future work).
 * Suppress unused-function warnings; these are intentionally kept for
 * the next development phase. */
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wunused-function"

/*
 * checker_method_param — find param type at index `param_idx` for a named
 * method.  Returns TYPE_UNKNOWN if not found or index out of range.
 */
static Type checker_method_param(Symbol *cls_sym,
                                  const char *method_name, int method_len,
                                  int param_idx,
                                  TypeParamNode *type_params,
                                  Type *concrete_args, int concrete_count)
{
    /* AST path */
    if (cls_sym->class_decl) {
        typedef struct ClassMethodNode CMN;
        for (CMN *m = cls_sym->class_decl->class_decl.methods; m; m = m->next) {
            if (!m->fn) continue;
            const char *mn = m->fn->fn_decl.name;
            int         ml = m->fn->fn_decl.length;
            if (ml != method_len || memcmp(mn, method_name, method_len) != 0)
                continue;
            ParamNode *p = m->fn->fn_decl.params;
            for (int i = 0; p && i < param_idx; i++, p = p->next) {}
            if (!p) break;
            Type pt = p->type;
            if (type_params && concrete_count > 0)
                return substitute_type(pt, type_params, concrete_args, concrete_count);
            return pt;
        }
    }
    /* ClassDef path — MethodDef doesn't store param types yet; return ANY */
    (void)method_name; (void)method_len; (void)param_idx;
    Type any; memset(&any, 0, sizeof(any)); any.kind = TYPE_ANY;
    return any;
}

/*
 * checker_method_param_count — return the parameter count for a named method.
 * Returns -1 if not found.
 */
static int checker_method_param_count(Symbol *cls_sym,
                                       const char *method_name, int method_len)
{
    /* AST path */
    if (cls_sym->class_decl) {
        typedef struct ClassMethodNode CMN;
        for (CMN *m = cls_sym->class_decl->class_decl.methods; m; m = m->next) {
            if (!m->fn) continue;
            const char *mn = m->fn->fn_decl.name;
            int         ml = m->fn->fn_decl.length;
            if (ml == method_len && memcmp(mn, method_name, method_len) == 0)
                return m->fn->fn_decl.param_count;
        }
    }
    /* ClassDef path — count from MethodDef; no param type info yet */
    if (cls_sym->class_def) {
        ClassDef *def = (ClassDef *)cls_sym->class_def;
        for (int i = 0; i < def->method_count; i++) {
            if ((int)strlen(def->methods[i].name) == method_len &&
                memcmp(def->methods[i].name, method_name, method_len) == 0)
                return -2; /* found but param count not stored in MethodDef yet */
        }
    }
    return -1;
}

#pragma GCC diagnostic pop   /* -Wunused-function */

/* ─────────────────────────────────────────────────────────────────────────── */

/*
 * is_subtype — returns true if class named `child_name` is the same as or
 * a direct/indirect subclass of `parent_name`, by walking the symbol table.
 * Used for type-compatibility checks on assignments and function arguments.
 */
static bool is_subtype(Checker *c, const char *child_name, const char *parent_name) {
    if (!child_name || !parent_name) return false;
    /* Walk the ancestor chain starting from child */
    const char *current = child_name;
    int safety = 64; /* guard against inheritance cycles */
    while (current && safety-- > 0) {
        if (strcmp(current, parent_name) == 0) return true;
        Symbol *sym = lookup_symbol(c, current, (int)strlen(current));
        if (!sym || sym->kind != SYM_CLASS || !sym->class_decl) return false;
        /* Move up: get the parent name from the AST node */
        Stmt *cls_ast = sym->class_decl;
        if (!cls_ast->class_decl.parent_name || cls_ast->class_decl.parent_length == 0)
            return false;
        /* Parent name is not null-terminated — need a temp buffer */
        static char parent_buf[64];
        int plen = cls_ast->class_decl.parent_length < 63
                 ? cls_ast->class_decl.parent_length : 63;
        memcpy(parent_buf, cls_ast->class_decl.parent_name, plen);
        parent_buf[plen] = '\0';
        current = parent_buf;
    }
    return false;
}

/*
 * class_implements_interface — returns true if class `class_name` directly
 * or through its ancestor chain implements interface `iface_name`.
 *
 * We walk:
 *   1. The interfaces list of the class itself.
 *   2. The parent class (recursively), so a child class that doesn't
 *      re-list an interface but inherits from one that does still satisfies it.
 *   3. Interface inheritance: if IChild : IBase, then implementing IChild
 *      also satisfies IBase.
 */
static bool iface_extends(Checker *c, const char *child_iface, const char *parent_iface);

static bool class_implements_interface(Checker *c,
                                       const char *class_name,
                                       const char *iface_name) {
    if (!class_name || !iface_name) return false;
    char buf[64];
    const char *cur = class_name;
    int safety = 64;
    while (cur && safety-- > 0) {
        Symbol *csym = lookup_symbol(c, cur, (int)strlen(cur));
        if (!csym || csym->kind != SYM_CLASS || !csym->class_decl) return false;
        Stmt *cls = csym->class_decl;
        /* Check every listed interface (and their parents) */
        typedef struct IfaceNameNode IFNode;
        for (IFNode *in = cls->class_decl.interfaces; in; in = in->next) {
            Symbol *isym = lookup_symbol(c, in->name, in->length);
            if (!isym || isym->kind != SYM_INTERFACE) continue;
            /* Direct match or the interface extends the target */
            if (strcmp(isym->iface_name_buf, iface_name) == 0) return true;
            if (iface_extends(c, isym->iface_name_buf, iface_name)) return true;
        }
        /* Walk up to parent class */
        if (!cls->class_decl.parent_name || cls->class_decl.parent_length == 0)
            return false;
        int plen = cls->class_decl.parent_length < 63
                 ? cls->class_decl.parent_length : 63;
        memcpy(buf, cls->class_decl.parent_name, plen);
        buf[plen] = '\0';
        cur = buf;
    }
    return false;
}

/*
 * iface_extends — returns true if interface `child_iface` inherits from
 * (or IS) `parent_iface`, walking the parent chain.
 */
static bool iface_extends(Checker *c, const char *child_iface, const char *parent_iface) {
    if (!child_iface || !parent_iface) return false;
    if (strcmp(child_iface, parent_iface) == 0) return true;
    char buf[64];
    const char *cur = child_iface;
    int safety = 32;
    while (cur && safety-- > 0) {
        Symbol *isym = lookup_symbol(c, cur, (int)strlen(cur));
        if (!isym || isym->kind != SYM_INTERFACE || !isym->interface_decl) return false;
        Stmt *iface = isym->interface_decl;
        if (!iface->interface_decl.parent_name || iface->interface_decl.parent_length == 0)
            return false;
        int plen = iface->interface_decl.parent_length < 63
                 ? iface->interface_decl.parent_length : 63;
        memcpy(buf, iface->interface_decl.parent_name, plen);
        buf[plen] = '\0';
        if (strcmp(buf, parent_iface) == 0) return true;
        cur = buf;
    }
    return false;
}

/*
 * type_is_assignable_to_interface — returns true when a TYPE_OBJECT value
 * (concrete class) can be stored in an interface-typed slot.
 */
static bool type_is_assignable_to_interface(Checker *c,
                                             const Type rhs,
                                             const char *iface_name) {
    if (rhs.kind != TYPE_OBJECT || !rhs.class_name) return false;
    return class_implements_interface(c, rhs.class_name, iface_name);
}

/*
 * iface_lookup_method — find a method signature in an interface (and its
 * parent chain). Returns a pointer to the IfaceMethodNode if found, NULL
 * if not. `iface_name` must be null-terminated.
 */
typedef struct IfaceMethodNode IMNodeT;
static IMNodeT *iface_lookup_method(Checker *c,
                                    const char *iface_name,
                                    const char *method_name,
                                    int         method_len) {
    char buf[64];
    const char *cur = iface_name;
    int safety = 32;
    while (cur && safety-- > 0) {
        Symbol *isym = lookup_symbol(c, cur, (int)strlen(cur));
        if (!isym || isym->kind != SYM_INTERFACE || !isym->interface_decl) return NULL;
        Stmt *iface = isym->interface_decl;
        for (IMNodeT *m = iface->interface_decl.methods; m; m = m->next) {
            if (m->length == method_len &&
                memcmp(m->name, method_name, method_len) == 0)
                return m;
        }
        if (!iface->interface_decl.parent_name || iface->interface_decl.parent_length == 0)
            return NULL;
        int plen = iface->interface_decl.parent_length < 63
                 ? iface->interface_decl.parent_length : 63;
        memcpy(buf, iface->interface_decl.parent_name, plen);
        buf[plen] = '\0';
        cur = buf;
    }
    return NULL;
}

/*
 * same_class_check — used by access checks.
 * Returns true if the checker is currently inside a class that is the same
 * as or a subclass of `target_class_name`.
 * For private: only same class. For protected: same class or subclass.
 */
static bool same_class_check(Checker *c, const char *target_class_name, bool allow_subclass) {
    if (!c->current_class) return false;
    const char *current_name = NULL;
    /* Build a null-terminated version of current class name */
    static char cur_buf[64];
    int clen = c->current_class->class_decl.length < 63
             ? c->current_class->class_decl.length : 63;
    memcpy(cur_buf, c->current_class->class_decl.name, clen);
    cur_buf[clen] = '\0';
    current_name  = cur_buf;

    if (strcmp(current_name, target_class_name) == 0) return true;
    if (allow_subclass && is_subtype(c, current_name, target_class_name)) return true;
    return false;
}

static Type check_expr(Checker *c, Expr *expr) {
    switch (expr->kind) {

        /* ── Literals — types are self-evident ────────────────────────── */
        case EXPR_INT_LIT:    return resolve(expr, type_int());
        case EXPR_CHAR_LIT:   return resolve(expr, type_char());
        case EXPR_FLOAT_LIT:  return resolve(expr, type_float());
        case EXPR_BOOL_LIT:   return resolve(expr, type_bool());
        case EXPR_STRING_LIT: return resolve(expr, type_string());

        /* ── Interpolated string — $"Hello, {name}!" ────────────────────
         * Each expression segment is type-checked independently.
         * Any type is allowed (int, float, bool, string, enum) — the
         * compiler will emit TO_STR for non-string segments.
         * The overall expression always produces TYPE_STRING. */
        case EXPR_INTERP_STRING: {
            typedef struct InterpSegment ISeg;
            for (ISeg *seg = expr->interp_string.segments; seg; seg = seg->next) {
                if (seg->is_expr) {
                    Type t = check_expr(c, seg->expr);
                    if (t.kind == TYPE_VOID) {
                        type_error(c, seg->expr->line,
                            "Cannot use void expression inside interpolated string");
                    }
                }
                /* text segments need no checking */
            }
            return resolve(expr, type_string());
        }

        /* ── Identifier — look up in symbol table ──────────────────────── */
        case EXPR_IDENT: {
            Symbol *sym = lookup_symbol(c, expr->ident.name, expr->ident.length);
            if (!sym) {
                /* Before giving up, check if we're inside a class and the name
                 * matches a field — if so, treat it as an implicit this.field
                 * (or implicit static field access if in a static method). */
                if (c->current_class) {
                    typedef struct ClassFieldNode CFNode;
                    const char *fname  = expr->ident.name;
                    int         flen   = expr->ident.length;

                    /* Walk the inheritance chain */
                    Stmt *search_cls = c->current_class;
                    char  search_name[CLASS_NAME_MAX];
                    int   clen = c->current_class->class_decl.length < CLASS_NAME_MAX - 1
                               ? c->current_class->class_decl.length : CLASS_NAME_MAX - 1;
                    memcpy(search_name, c->current_class->class_decl.name, clen);
                    search_name[clen] = '\0';

                    while (search_cls) {
                        for (CFNode *f = search_cls->class_decl.fields; f; f = f->next) {
                            if (f->length == flen && memcmp(f->name, fname, flen) == 0) {
                                if (c->in_static_method) {
                                    if (!f->is_static) {
                                        return error_type(c, expr, expr->line,
                                            "Cannot access instance field '%.*s' in a static method",
                                            flen, fname);
                                    }
                                    /* Rewrite to EXPR_STATIC_GET — indices resolved later
                                     * by the compiler (we don't have class_idx here). */
                                    expr->kind                     = EXPR_STATIC_GET;
                                    expr->static_get.class_name     = search_name;
                                    expr->static_get.class_name_len = (int)strlen(search_name);
                                    expr->static_get.field_name     = fname;
                                    expr->static_get.field_name_len = flen;
                                    expr->static_get.class_idx      = -1; /* filled by compiler */
                                    expr->static_get.field_idx      = -1;
                                    return resolve(expr, f->type);
                                }
                                /* Found a matching field — rewrite this EXPR_IDENT
                                 * in-place to EXPR_FIELD_GET with an implicit 'this'. */
                                Expr *implicit_this = expr_this(c->arena, expr->line, expr->col);
                                expr->kind                    = EXPR_FIELD_GET;
                                expr->field_get.object        = implicit_this;
                                expr->field_get.field_name     = fname;
                                expr->field_get.field_name_len = flen;
                                /* Now type-check the rewritten node */
                                return check_expr(c, expr);
                            }
                        }
                        /* Walk to parent class */
                        if (search_cls->class_decl.parent_name &&
                            search_cls->class_decl.parent_length > 0) {
                            int plen = search_cls->class_decl.parent_length < CLASS_NAME_MAX - 1
                                     ? search_cls->class_decl.parent_length : CLASS_NAME_MAX - 1;
                            memcpy(search_name, search_cls->class_decl.parent_name, plen);
                            search_name[plen] = '\0';
                            Symbol *psym = lookup_symbol(c, search_name, plen);
                            search_cls = (psym && psym->kind == SYM_CLASS) ? psym->class_decl : NULL;
                        } else {
                            search_cls = NULL;
                        }
                    }
                }
                return error_type(c, expr, expr->line,
                    "Undefined variable '%.*s'",
                    expr->ident.length, expr->ident.name);
            }
            if (sym->kind == SYM_FN) {
                return error_type(c, expr, expr->line,
                    "'%.*s' is a function, not a variable",
                    expr->ident.length, expr->ident.name);
            }
            /* A bare class name used as an expression (e.g. "MyClass" in
             * "MyClass.staticField") resolves to TYPE_CLASS_REF. */
            if (sym->kind == SYM_CLASS) {
                record_usage(c, sym, expr->line, expr->col, expr->ident.length);
                return resolve(expr, type_class_ref(sym->class_name_buf));
            }
            record_usage(c, sym, expr->line, expr->col, expr->ident.length);
            return resolve(expr, sym->type);
        }

        /* ── Unary operators ───────────────────────────────────────────── */
        case EXPR_UNARY: {
            Type operand = check_expr(c, expr->unary.operand);
            if (is_unknown(operand)) return resolve(expr, operand);

            if (expr->unary.op == TOK_MINUS) {
                /* Negation: operand must be numeric */
                if (!type_is_numeric(operand)) {
                    return error_type(c, expr, expr->line,
                        "Unary '-' requires int or float, got %s",
                        type_kind_name(operand.kind));
                }
                return resolve(expr, operand);  /* Same type as operand */

            } else if (expr->unary.op == TOK_BANG) {
                /* Logical not: operand must be bool */
                if (operand.kind != TYPE_BOOL) {
                    return error_type(c, expr, expr->line,
                        "Unary '!' requires bool, got %s",
                        type_kind_name(operand.kind));
                }
                return resolve(expr, type_bool());
            }

            return resolve(expr, operand);  /* Fallback */
        }

        /* ── Binary operators ──────────────────────────────────────────── */
        case EXPR_BINARY: {
            Type left  = check_expr(c, expr->binary.left);
            Type right = check_expr(c, expr->binary.right);

            /* Suppress cascading errors */
            if (is_unknown(left) || is_unknown(right))
                return resolve(expr, left);

            TokenType op = expr->binary.op;

            /* Arithmetic: + - * / % */
            if (op == TOK_PLUS || op == TOK_MINUS ||
                op == TOK_STAR || op == TOK_SLASH || op == TOK_PERCENT)
            {
                /* String concatenation: + with a string on either side.
                 * The non-string operand will be converted to string at runtime
                 * via OP_TO_STR. Any type is valid. */
                if (op == TOK_PLUS &&
                    (left.kind == TYPE_STRING || right.kind == TYPE_STRING))
                {
                    return resolve(expr, type_string());
                }

                /* Numeric arithmetic: both sides must be numeric.
                 * Integer-family types (int, byte, short, etc.) can mix freely —
                 * the result is widened to the larger of the two types.
                 * Float/double cannot mix with integer types (still explicit). */
                if (!type_is_numeric(left)) {
                    return error_type(c, expr, expr->line,
                        "Left side of '%s' must be a number, got %s",
                        token_type_name(op), type_kind_name(left.kind));
                }
                if (!type_is_numeric(right)) {
                    return error_type(c, expr, expr->line,
                        "Right side of '%s' must be a number, got %s",
                        token_type_name(op), type_kind_name(right.kind));
                }
                /* Float/double mixing check */
                bool left_float  = (left.kind  == TYPE_FLOAT || left.kind  == TYPE_DOUBLE);
                bool right_float = (right.kind == TYPE_FLOAT || right.kind == TYPE_DOUBLE);
                if (left_float != right_float) {
                    return error_type(c, expr, expr->line,
                        "Cannot mix integer and float types in '%s' (%s and %s) "
                        "— cast explicitly",
                        token_type_name(op),
                        type_kind_name(left.kind),
                        type_kind_name(right.kind));
                }
                /* Both float/double: result is double (widest) */
                if (left_float) {
                    return resolve(expr, type_float()); /* float and double both use float storage */
                }
                /* Both integer-family: result is the wider type.
                 * Width order: sbyte(1) < byte(1) < short(2) < ushort(2) <
                 *              int/uint(4) < long/ulong(8)
                 * We use int64_t for all, so no actual data loss — but the
                 * declared type tracks intent for overflow warnings. */
                return resolve(expr, type_wider_int(left, right));  /* Result is same type as operands */
            }

            /* Comparison: < <= > >= */
            if (op == TOK_LT || op == TOK_LTE ||
                op == TOK_GT || op == TOK_GTE)
            {
                if (!type_is_numeric(left) || !type_equals(left, right)) {
                    return error_type(c, expr, expr->line,
                        "Comparison '%s' requires matching numeric types, got %s and %s",
                        token_type_name(op),
                        type_kind_name(left.kind),
                        type_kind_name(right.kind));
                }
                return resolve(expr, type_bool());  /* Comparison always yields bool */
            }

            /* Equality: == != */
            if (op == TOK_EQ || op == TOK_NEQ) {
                /* Allow null comparisons: x == null, x != null (x must be nullable) */
                if (left.kind == TYPE_NULL || right.kind == TYPE_NULL) {
                    Type non_null = (left.kind == TYPE_NULL) ? right : left;
                    if (!non_null.is_nullable && non_null.kind != TYPE_ANY &&
                        non_null.kind != TYPE_NULL) {
                        type_error(c, expr->line,
                            "Cannot compare non-nullable type '%s' with null — "
                            "declare it as '%s?' to allow null",
                            type_kind_name(non_null.kind),
                            type_kind_name(non_null.kind));
                    }
                    return resolve(expr, type_bool());
                }
                /* Normal equality: types must match (ignoring nullability for comparison) */
                Type lb = left;  lb.is_nullable = false;
                Type rb = right; rb.is_nullable = false;
                if (!type_equals(lb, rb)) {
                    /* Give a more specific message for enum mismatches */
                    if (left.kind == TYPE_ENUM && right.kind == TYPE_ENUM) {
                        return error_type(c, expr, expr->line,
                            "Cannot compare enum '%s' with enum '%s' — different types",
                            left.enum_name  ? left.enum_name  : "?",
                            right.enum_name ? right.enum_name : "?");
                    }
                    if (left.kind == TYPE_ENUM || right.kind == TYPE_ENUM) {
                        return error_type(c, expr, expr->line,
                            "Cannot compare enum '%s' with %s — use enum values directly",
                            left.kind == TYPE_ENUM
                                ? (left.enum_name  ? left.enum_name  : "?")
                                : (right.enum_name ? right.enum_name : "?"),
                            left.kind == TYPE_ENUM
                                ? type_kind_name(right.kind)
                                : type_kind_name(left.kind));
                    }
                    return error_type(c, expr, expr->line,
                        "Equality '%s' requires matching types, got %s and %s",
                        token_type_name(op),
                        type_kind_name(left.kind),
                        type_kind_name(right.kind));
                }
                return resolve(expr, type_bool());
            }

            /* Logical: && || */
            if (op == TOK_AND || op == TOK_OR) {
                if (left.kind != TYPE_BOOL) {
                    return error_type(c, expr, expr->line,
                        "Left side of '%s' must be bool, got %s",
                        token_type_name(op), type_kind_name(left.kind));
                }
                if (right.kind != TYPE_BOOL) {
                    return error_type(c, expr, expr->line,
                        "Right side of '%s' must be bool, got %s",
                        token_type_name(op), type_kind_name(right.kind));
                }
                return resolve(expr, type_bool());
            }

            return resolve(expr, left);  /* Fallback */
        }

        /* ── Postfix increment/decrement ───────────────────────────────── */
        case EXPR_POSTFIX: {
            const char *op_str = expr->postfix.op == TOK_PLUS_PLUS ? "++" : "--";

            if (!expr->postfix.is_field) {
                /* Simple variable lvalue */
                Symbol *sym = lookup_symbol(c, expr->postfix.name, expr->postfix.length);
                if (!sym) {
                    /* Check if it's a bare field name inside a class */
                    if (c->current_class) {
                        typedef struct ClassFieldNode CFNode;
                        const char *fname = expr->postfix.name;
                        int         flen  = expr->postfix.length;

                        Stmt *search_cls = c->current_class;
                        char  search_name[CLASS_NAME_MAX];
                        int   clen = c->current_class->class_decl.length < CLASS_NAME_MAX - 1
                                   ? c->current_class->class_decl.length : CLASS_NAME_MAX - 1;
                        memcpy(search_name, c->current_class->class_decl.name, clen);
                        search_name[clen] = '\0';

                        while (search_cls) {
                            for (CFNode *f = search_cls->class_decl.fields; f; f = f->next) {
                                if (f->length == flen && memcmp(f->name, fname, flen) == 0) {
                                    /* Rewrite to field lvalue with implicit 'this' */
                                    Expr *implicit_this = expr_this(c->arena, expr->line, expr->col);
                                    expr->postfix.is_field        = true;
                                    expr->postfix.object          = implicit_this;
                                    expr->postfix.field_name      = fname;
                                    expr->postfix.field_name_len  = flen;
                                    /* Fall through to the is_field path below */
                                    goto postfix_field;
                                }
                            }
                            if (search_cls->class_decl.parent_name &&
                                search_cls->class_decl.parent_length > 0) {
                                int plen = search_cls->class_decl.parent_length < CLASS_NAME_MAX - 1
                                         ? search_cls->class_decl.parent_length : CLASS_NAME_MAX - 1;
                                memcpy(search_name, search_cls->class_decl.parent_name, plen);
                                search_name[plen] = '\0';
                                Symbol *psym = lookup_symbol(c, search_name, plen);
                                search_cls = (psym && psym->kind == SYM_CLASS) ? psym->class_decl : NULL;
                            } else {
                                search_cls = NULL;
                            }
                        }
                    }
                    return error_type(c, expr, expr->line,
                        "Undefined variable '%.*s'",
                        expr->postfix.length, expr->postfix.name);
                }
                if (sym->type.kind != TYPE_INT) {
                    return error_type(c, expr, expr->line,
                        "'%s' operator requires an int variable, got %s",
                        op_str, type_kind_name(sym->type.kind));
                }
                return resolve(expr, type_int());
            }
            postfix_field:; {
                /* Field lvalue — obj.field (explicit or rewritten from bare name) */
                Type obj_type = check_expr(c, expr->postfix.object);

                /* ── Static field lvalue: ClassName.field++ ─────────────────
                 * If the object is a class reference, look up the static field
                 * and rewrite to a static increment. */
                if (obj_type.kind == TYPE_CLASS_REF) {
                    const char *cname = obj_type.class_name;
                    const char *fname = expr->postfix.field_name;
                    int         flen  = expr->postfix.field_name_len;
                    Symbol *cls_sym = lookup_symbol(c, cname, (int)strlen(cname));
                    if (!cls_sym || cls_sym->kind != SYM_CLASS) {
                        return error_type(c, expr, expr->line, "Unknown class '%s'", cname);
                    }
                    if (!cls_sym->class_decl && !cls_sym->class_def) {
                        return error_type(c, expr, expr->line, "Unknown class '%s'", cname);
                    }
                    /* Use unified field lookup */
                    Type ft = checker_field_type(cls_sym, fname, flen, NULL, NULL, 0);
                    if (ft.kind == TYPE_UNKNOWN) {
                        return error_type(c, expr, expr->line,
                            "Class '%s' has no static field '%.*s'", cname, flen, fname);
                    }
                    /* For static postfix we also need is_static — only checkable via AST */
                    if (cls_sym->class_decl) {
                        typedef struct ClassFieldNode CFNode;
                        for (CFNode *f = cls_sym->class_decl->class_decl.fields; f; f = f->next) {
                            if (f->length == flen && memcmp(f->name, fname, flen) == 0) {
                                if (!f->is_static) {
                                    return error_type(c, expr, expr->line,
                                        "'%s' on '%.*s': field is not static", op_str, flen, fname);
                                }
                                break;
                            }
                        }
                    }
                    if (ft.kind != TYPE_INT) {
                        return error_type(c, expr, expr->line,
                            "'%s' operator requires an int field, got %s",
                            op_str, type_kind_name(ft.kind));
                    }
                    expr->postfix.is_static_field = true;
                    return resolve(expr, type_int());
                }

                if (obj_type.kind != TYPE_OBJECT) {
                    return error_type(c, expr, expr->line,
                        "'%s' operator: cannot access field on non-object type %s",
                        op_str, type_kind_name(obj_type.kind));
                }
                Symbol *cls_sym = lookup_symbol(c,
                    obj_type.class_name, (int)strlen(obj_type.class_name));
                if (!cls_sym || cls_sym->kind != SYM_CLASS) {
                    return error_type(c, expr, expr->line,
                        "Unknown class '%s'", obj_type.class_name);
                }
                {
                    const char *fname = expr->postfix.field_name;
                    int         flen  = expr->postfix.field_name_len;
                    Type ft = checker_field_type(cls_sym, fname, flen, NULL, NULL, 0);
                    if (ft.kind == TYPE_UNKNOWN) {
                        return error_type(c, expr, expr->line,
                            "Class '%s' has no field '%.*s'",
                            obj_type.class_name, flen, fname);
                    }
                    if (ft.kind != TYPE_INT) {
                        return error_type(c, expr, expr->line,
                            "'%s' operator requires an int field, got %s",
                            op_str, type_kind_name(ft.kind));
                    }
                    return resolve(expr, type_int());
                }
            }
        }

        /* ── Assignment ────────────────────────────────────────────────── */
        case EXPR_ASSIGN: {
            Symbol *sym = lookup_symbol(c,
                                        expr->assign.name,
                                        expr->assign.length);
            if (!sym) {
                /* Check if this is a bare field assignment inside a class */
                if (c->current_class) {
                    typedef struct ClassFieldNode CFNode;
                    const char *fname = expr->assign.name;
                    int         flen  = expr->assign.length;

                    Stmt *search_cls = c->current_class;
                    char  search_name[CLASS_NAME_MAX];
                    int   clen = c->current_class->class_decl.length < CLASS_NAME_MAX - 1
                               ? c->current_class->class_decl.length : CLASS_NAME_MAX - 1;
                    memcpy(search_name, c->current_class->class_decl.name, clen);
                    search_name[clen] = '\0';

                    while (search_cls) {
                        for (CFNode *f = search_cls->class_decl.fields; f; f = f->next) {
                            if (f->length == flen && memcmp(f->name, fname, flen) == 0) {
                                /* Rewrite EXPR_ASSIGN → EXPR_FIELD_SET on implicit 'this' */
                                Expr *implicit_this = expr_this(c->arena, expr->line, expr->col);
                                Expr *value         = expr->assign.value;
                                expr->kind                    = EXPR_FIELD_SET;
                                expr->field_set.object        = implicit_this;
                                expr->field_set.field_name     = fname;
                                expr->field_set.field_name_len = flen;
                                expr->field_set.value          = value;
                                return check_expr(c, expr);
                            }
                        }
                        if (search_cls->class_decl.parent_name &&
                            search_cls->class_decl.parent_length > 0) {
                            int plen = search_cls->class_decl.parent_length < CLASS_NAME_MAX - 1
                                     ? search_cls->class_decl.parent_length : CLASS_NAME_MAX - 1;
                            memcpy(search_name, search_cls->class_decl.parent_name, plen);
                            search_name[plen] = '\0';
                            Symbol *psym = lookup_symbol(c, search_name, plen);
                            search_cls = (psym && psym->kind == SYM_CLASS) ? psym->class_decl : NULL;
                        } else {
                            search_cls = NULL;
                        }
                    }
                }
                return error_type(c, expr, expr->line,
                    "Undefined variable '%.*s'",
                    expr->assign.length, expr->assign.name);
            }
            if (sym->kind == SYM_FN) {
                return error_type(c, expr, expr->line,
                    "Cannot assign to function '%.*s'",
                    expr->assign.length, expr->assign.name);
            }

            /* Check the right-hand side */
            Type rhs = check_expr(c, expr->assign.value);
            if (is_unknown(rhs)) return resolve(expr, sym->type);

            /* Types must be compatible — int-family types can mix (with wrap) */
            {
                bool is_iface_var = false;
                if (sym->type.kind == TYPE_OBJECT && sym->type.class_name) {
                    Symbol *dsym = lookup_symbol(c,
                        sym->type.class_name, (int)strlen(sym->type.class_name));
                    is_iface_var = (dsym && dsym->kind == SYM_INTERFACE);
                }
                bool ok = types_assignable(sym->type, rhs) ||
                    (sym->type.kind == TYPE_OBJECT && rhs.kind == TYPE_OBJECT &&
                     sym->type.class_name && rhs.class_name &&
                     is_subtype(c, rhs.class_name, sym->type.class_name)) ||
                    (is_iface_var && sym->type.class_name &&
                     type_is_assignable_to_interface(c, rhs, sym->type.class_name));
                if (!ok) {
                    return error_type(c, expr, expr->line,
                        "Cannot assign %s to variable '%.*s' of type %s",
                        type_kind_name(rhs.kind),
                        expr->assign.length, expr->assign.name,
                        type_kind_name(sym->type.kind));
                }
            }

            /* Warn on narrowing (e.g. assigning int to byte).
             * Suppress for literals — explicit values are intentional. */
            {
                ExprKind ek = expr->assign.value->kind;
                bool is_literal = (ek == EXPR_INT_LIT || ek == EXPR_FLOAT_LIT ||
                                   ek == EXPR_BOOL_LIT || ek == EXPR_STRING_LIT);
                if (!is_literal)
                    warn_if_narrowing(c, expr->line, sym->type.kind, rhs.kind);
            }

            /* Assignment expression produces the assigned value's type */
            return resolve(expr, sym->type);
        }

        /* ── Function call ─────────────────────────────────────────────── */
        case EXPR_CALL: {
            /* Check if this is actually an event fire: EventName(args) */
            {
                Symbol *esym = lookup_symbol(c, expr->call.name, expr->call.length);
                if (!esym && c->current_class) {
                    /* Look for a member event in the current class */
                    typedef struct ClassEventNode CEN;
                    for (CEN *e = c->current_class->class_decl.events; e; e = e->next) {
                        if (e->length == expr->call.length &&
                            memcmp(e->name, expr->call.name, expr->call.length) == 0) {
                            /* Create a temporary symbol for type checking */
                            static Symbol temp_esym;
                            temp_esym.kind = SYM_EVENT;
                            temp_esym.param_count = e->param_count;
                            ParamNode *p = e->params;
                            for (int i = 0; i < e->param_count && i < MAX_PARAMS && p; i++, p = p->next) {
                                temp_esym.param_types[i] = p->type;
                            }
                            esym = &temp_esym;
                            break;
                        }
                    }
                }
                if (esym && esym->kind == SYM_EVENT) {
                    /* Rewrite to EXPR_EVENT_FIRE and validate args */
                    int     arg_count = expr->call.arg_count;
                    ArgNode *args     = expr->call.args;
                    /* Save name/len BEFORE mutating the union */
                    const char *saved_name = expr->call.name;
                    int         saved_len  = expr->call.length;
                    expr->kind                      = EXPR_EVENT_FIRE;
                    expr->event_fire.event_name     = saved_name;
                    expr->event_fire.event_name_len = saved_len;
                    expr->event_fire.args           = args;
                    expr->event_fire.arg_count      = arg_count;
                    expr->event_fire.object         = NULL; /* member event will resolve in check_expr */
                    /* Fall into EXPR_EVENT_FIRE checking */
                    goto check_event_fire;
                }
            }
            /* Look up the function */
            Symbol *sym = lookup_symbol(c,
                                        expr->call.name,
                                        expr->call.length);
            if (!sym) {
                /* Before giving up, check if it's a method on the current class */
                if (c->current_class) {
                    typedef struct ClassMethodNode CMNode;
                    const char *mname = expr->call.name;
                    int         mlen  = expr->call.length;

                    Stmt *search_cls = c->current_class;
                    char  search_name[CLASS_NAME_MAX];
                    int   clen = c->current_class->class_decl.length < CLASS_NAME_MAX - 1
                               ? c->current_class->class_decl.length : CLASS_NAME_MAX - 1;
                    memcpy(search_name, c->current_class->class_decl.name, clen);
                    search_name[clen] = '\0';

                    while (search_cls) {
                        for (CMNode *m = search_cls->class_decl.methods; m; m = m->next) {
                            if (!m->is_constructor &&
                                m->fn->fn_decl.length == mlen &&
                                memcmp(m->fn->fn_decl.name, mname, mlen) == 0) {
                                /* Rewrite EXPR_CALL → EXPR_METHOD_CALL on implicit 'this' */
                                Expr    *implicit_this = expr_this(c->arena, expr->line, expr->col);
                                ArgNode *args          = expr->call.args;
                                int      arg_count     = expr->call.arg_count;
                                expr->kind                      = EXPR_METHOD_CALL;
                                expr->method_call.object        = implicit_this;
                                expr->method_call.method_name    = mname;
                                expr->method_call.method_name_len = mlen;
                                expr->method_call.args           = args;
                                expr->method_call.arg_count      = arg_count;
                                return check_expr(c, expr);
                            }
                        }
                        if (search_cls->class_decl.parent_name &&
                            search_cls->class_decl.parent_length > 0) {
                            int plen = search_cls->class_decl.parent_length < CLASS_NAME_MAX - 1
                                     ? search_cls->class_decl.parent_length : CLASS_NAME_MAX - 1;
                            memcpy(search_name, search_cls->class_decl.parent_name, plen);
                            search_name[plen] = '\0';
                            Symbol *psym = lookup_symbol(c, search_name, plen);
                            search_cls = (psym && psym->kind == SYM_CLASS) ? psym->class_decl : NULL;
                        } else {
                            search_cls = NULL;
                        }
                    }
                }
                return error_type(c, expr, expr->line,
                    "Undefined function '%.*s'",
                    expr->call.length, expr->call.name);
            }
            if (sym->kind != SYM_FN) {
                return error_type(c, expr, expr->line,
                    "'%.*s' is a variable, not a function",
                    expr->call.length, expr->call.name);
            }
            record_usage(c, sym, expr->line, expr->col, expr->call.length);

            /* Check argument count — allow fewer args than params if trailing
             * params have default values. */
            {
                ParamNode *fn_params = sym->fn_decl_node
                                     ? sym->fn_decl_node->fn_decl.params : NULL;
                int required = fn_params ? count_required_params(fn_params)
                                         : sym->param_count;
                int total    = fn_params ? count_params(fn_params)
                                         : sym->param_count;
                if (expr->call.arg_count < required ||
                    expr->call.arg_count > total) {
                    return error_type(c, expr, expr->line,
                        required == total
                            ? "Function '%.*s' expects %d argument(s), got %d"
                            : "Function '%.*s' expects %d-%d argument(s), got %d",
                        expr->call.length, expr->call.name,
                        required, total, expr->call.arg_count);
                }
                /* Store param list so compiler can emit default args */
                expr->call.resolved_params = fn_params;
            }

            /* ── Generic function call: identity<int>(x) ────────────────
             * If the called function is generic (has type params in its
             * fn_decl), collect the concrete type args and substitute them
             * when checking parameter types and determining the return type. */
            TypeParamNode *fn_type_params = NULL;
            Type fn_concrete_args[MAX_TYPE_ARGS];
            int  fn_concrete_count = 0;

            if (sym->fn_decl_node) {
                fn_type_params = sym->fn_decl_node->fn_decl.type_params;
                int fn_tp_count = sym->fn_decl_node->fn_decl.type_param_count;

                if (fn_tp_count > 0) {
                    if (expr->call.type_arg_count == 0) {
                        type_error(c, expr->line,
                            "Generic function '%.*s' requires explicit type arguments",
                            expr->call.length, expr->call.name);
                    } else if (expr->call.type_arg_count != fn_tp_count) {
                        type_error(c, expr->line,
                            "Function '%.*s' expects %d type argument(s), got %d",
                            expr->call.length, expr->call.name,
                            fn_tp_count, expr->call.type_arg_count);
                    } else {
                        TypeArgNode *ta = expr->call.type_args;
                        while (ta && fn_concrete_count < MAX_TYPE_ARGS) {
                            fn_concrete_args[fn_concrete_count++] = ta->type;
                            ta = ta->next;
                        }
                        /* Check constraints */
                        TypeParamNode *tp = fn_type_params;
                        for (int i = 0; i < fn_concrete_count && tp; i++, tp = tp->next) {
                            if (!check_type_constraint(c, fn_concrete_args[i],
                                                       tp->constraint, tp->constraint_len))
                            {
                                type_error(c, expr->line,
                                    "Type argument %d for '%.*s': does not satisfy constraint '%.*s'",
                                    i + 1, expr->call.length, expr->call.name,
                                    tp->constraint_len, tp->constraint);
                            }
                        }
                    }
                } else if (expr->call.type_arg_count > 0) {
                    type_error(c, expr->line,
                        "Function '%.*s' is not generic but was given type arguments",
                        expr->call.length, expr->call.name);
                }
            }

            /* Check each argument's type against the parameter type,
             * substituting type params if this is a generic function call. */
            ArgNode *arg  = expr->call.args;
            int      idx  = 0;
            bool     args_ok = true;
            while (arg) {
                Type arg_type = check_expr(c, arg->expr);
                Type expected = sym->param_types[idx];
                /* Substitute type params → concrete types if generic */
                if (fn_type_params && fn_concrete_count > 0) {
                    expected = substitute_type(expected, fn_type_params,
                                               fn_concrete_args, fn_concrete_count);
                }
                if (!is_unknown(arg_type) &&
                    expected.kind != TYPE_ANY && expected.kind != TYPE_PARAM &&
                    !types_assignable(expected, arg_type))
                {
                    type_error(c, expr->line,
                        "Argument %d to '%.*s': expected %s, got %s",
                        idx + 1,
                        expr->call.length, expr->call.name,
                        type_kind_name(expected.kind),
                        type_kind_name(arg_type.kind));
                    args_ok = false;
                }
                arg = arg->next;
                idx++;
            }

            if (!args_ok) {
                expr->resolved_type.kind = TYPE_UNKNOWN;
                return expr->resolved_type;
            }

            /* Return type: substitute type params if generic */
            Type ret_type = sym->type;
            if (fn_type_params && fn_concrete_count > 0) {
                ret_type = substitute_type(ret_type, fn_type_params,
                                           fn_concrete_args, fn_concrete_count);
            }
            return resolve(expr, ret_type);
        }

        /* ── new ClassName(args) / new ClassName<T>(args) ──────────────── */
        case EXPR_NEW: {
            /* class_name is a raw source pointer (not null-terminated).
             * Copy to a stable null-terminated buffer before any string ops. */
            char cname_buf[128];
            int  clen = expr->new_expr.class_name_len;
            int  clen_safe = clen < 127 ? clen : 127;
            memcpy(cname_buf, expr->new_expr.class_name, clen_safe);
            cname_buf[clen_safe] = '\0';
            const char *cname = cname_buf;

            /* Handle generic instantiation: new Stack<int>()
             * The class name may be "Stack" (with type_args set) or
             * the canonical "Stack<int>" form from var decl parsing.
             * Normalise: if type_args present, look up base name only. */
            const char *lookup_name = cname;
            int         lookup_len  = clen;

            /* If class name itself contains '<' (came from EXPR_NEW with inline type args
             * parsed into name), extract base. Otherwise use type_args if present. */
            char base_buf[64];
            bool is_generic_new = (expr->new_expr.type_arg_count > 0) ||
                                   is_generic_type_name(cname);
            if (is_generic_new) {
                if (generic_base_name(cname, base_buf, sizeof(base_buf))) {
                    lookup_name = base_buf;
                    lookup_len  = (int)strlen(base_buf);
                } else if (expr->new_expr.type_arg_count > 0) {
                    /* cname is already the bare name */
                    int l = clen < 63 ? clen : 63;
                    memcpy(base_buf, cname, l);
                    base_buf[l] = '\0';
                    lookup_name = base_buf;
                    lookup_len  = l;
                }
            }

            Symbol *cls_sym = lookup_symbol(c, lookup_name, lookup_len);
            if (!cls_sym || cls_sym->kind != SYM_CLASS) {
                return error_type(c, expr, expr->line,
                    "Unknown class '%.*s'", lookup_len, lookup_name);
            }

            Stmt *cls_decl = cls_sym->class_decl;

            /* Generic validation: if the class has type params, we must have
             * matching type args. Works from both AST (cls_decl) and ClassDef
             * (class_def) so XAR-loaded generic classes are handled correctly. */
            Type concrete_args[MAX_TYPE_ARGS];
            int  concrete_count = 0;

            /* Determine type_param_count from whichever source is available */
            int cls_type_param_count = 0;
            if (cls_decl)
                cls_type_param_count = cls_decl->class_decl.type_param_count;
            else if (cls_sym->class_def)
                cls_type_param_count = ((ClassDef *)cls_sym->class_def)->type_param_count;

            if (cls_type_param_count > 0) {
                if (!is_generic_new) {
                    type_error(c, expr->line,
                        "Class '%s' is generic and requires type arguments",
                        cls_sym->class_name_buf);
                } else {
                    /* Collect concrete args from either type_args list or canonical name */
                    if (expr->new_expr.type_arg_count > 0) {
                        TypeArgNode *ta = expr->new_expr.type_args;
                        while (ta && concrete_count < MAX_TYPE_ARGS) {
                            concrete_args[concrete_count++] = ta->type;
                            ta = ta->next;
                        }
                    } else {
                        /* Parse from canonical name "Stack<int>" */
                        concrete_count = parse_canonical_type_args(cname,
                                            concrete_args, MAX_TYPE_ARGS);
                    }

                    if (concrete_count != cls_type_param_count) {
                        type_error(c, expr->line,
                            "Class '%s' expects %d type argument(s), got %d",
                            cls_sym->class_name_buf,
                            cls_type_param_count,
                            concrete_count);
                    } else if (cls_decl) {
                        /* Constraint checking requires AST (only for user-defined classes) */
                        TypeParamNode *tp = cls_decl->class_decl.type_params;
                        for (int i = 0; i < concrete_count && tp; i++, tp = tp->next) {
                            if (!check_type_constraint(c, concrete_args[i],
                                                       tp->constraint, tp->constraint_len))
                            {
                                type_error(c, expr->line,
                                    "Type argument %d for '%s': '%s' does not satisfy constraint '%.*s'",
                                    i + 1, cls_sym->class_name_buf,
                                    type_kind_name(concrete_args[i].kind),
                                    tp->constraint_len, tp->constraint);
                            }
                        }
                    }
                }
            } else if (is_generic_new && expr->new_expr.type_arg_count > 0) {
                type_error(c, expr->line,
                    "Class '%s' is not generic but was given type arguments",
                    cls_sym->class_name_buf);
            }

            /* ── Type-check constructor arguments ───────────────────────
             * This resolves enum/class expressions in args (e.g. Target.Class)
             * so the compiler sees EXPR_ENUM_ACCESS not EXPR_FIELD_GET.
             * Also validates arg count against constructor params (with defaults). */
            {
                /* Find the constructor's ParamNode list */
                ParamNode *ctor_params = NULL;
                if (cls_sym->class_decl) {
                    typedef struct ClassMethodNode CMNode;
                    for (CMNode *m = cls_sym->class_decl->class_decl.methods;
                         m; m = m->next) {
                        if (m->is_constructor) {
                            ctor_params = m->fn->fn_decl.params;
                            break;
                        }
                    }
                }
                /* Validate arg count */
                if (ctor_params) {
                    int req   = count_required_params(ctor_params);
                    int total = count_params(ctor_params);
                    if (expr->new_expr.arg_count < req ||
                        expr->new_expr.arg_count > total) {
                        type_error(c, expr->line,
                            req == total
                                ? "Constructor for '%s' expects %d argument(s), got %d"
                                : "Constructor for '%s' expects %d-%d argument(s), got %d",
                            cls_sym->class_name_buf, req, total,
                            expr->new_expr.arg_count);
                    }
                }
                /* Store param list for compiler default-arg emission */
                expr->new_expr.resolved_params = ctor_params;
            }
            for (ArgNode *arg = expr->new_expr.args; arg; arg = arg->next)
                check_expr(c, arg->expr);

            /* Use the null-terminated class_name_buf so type_equals works.
             * For generic types, the resolved type is still the base class
             * (erasure: Stack<int> and Stack<string> are both "Stack" at runtime). */
            return resolve(expr, type_object(cls_sym->class_name_buf));
        }

        /* ── this ──────────────────────────────────────────────────────── */
        case EXPR_THIS: {
            if (!c->current_class) {
                return error_type(c, expr, expr->line,
                    "'this' used outside of a class method");
            }
            if (c->in_static_method) {
                return error_type(c, expr, expr->line,
                    "'this' cannot be used in a static method");
            }
            Symbol *cls_sym = lookup_symbol(c,
                c->current_class->class_decl.name,
                c->current_class->class_decl.length);
            const char *cls_name = (cls_sym && cls_sym->kind == SYM_CLASS)
                                 ? cls_sym->class_name_buf
                                 : c->current_class->class_decl.name;
            return resolve(expr, type_object(cls_name));
        }

        /* ── Array expressions ─────────────────────────────────────────── */
        case EXPR_NEW_ARRAY: {
            /* new ElementType[length] */
            Type elem  = expr->new_array.element_type;
            Type len_t = check_expr(c, expr->new_array.length);
            if (!is_unknown(len_t) && !type_is_int_family(len_t)) {
                return error_type(c, expr, expr->line,
                    "Array length must be an integer, got %s",
                    type_kind_name(len_t.kind));
            }
            /* Allocate element_type on arena so the Type pointer lives long enough */
            Type *elem_ptr = arena_alloc(c->arena, sizeof(Type));
            *elem_ptr = elem;
            return resolve(expr, type_array(elem_ptr));
        }

        case EXPR_IS: {
            /* expr is TypeName -> bool
             * Check the operand, validate the target type exists, return bool. */
            check_expr(c, expr->type_op.operand);
            /* Validate the target type is meaningful (not void/unknown) */
            if (expr->type_op.check_type.kind == TYPE_VOID) {
                type_error(c, expr->line, "Cannot use 'is' with void type");
            }
            return resolve(expr, (Type){.kind = TYPE_BOOL});
        }

        case EXPR_AS: {
            /* expr as TypeName -> target type (runtime error on failure) */
            Type src = check_expr(c, expr->type_op.operand);
            Type tgt = expr->type_op.check_type;
            if (tgt.kind == TYPE_VOID) {
                type_error(c, expr->line, "Cannot cast to void");
            }
            (void)src; /* runtime check; compiler trusts the cast */
            return resolve(expr, tgt);
        }

        case EXPR_TYPEOF: {
            /* typeof(expr) -> Type object */
            check_expr(c, expr->type_of.operand);
            /* Return TYPE_OBJECT with class name "Type" so it prints nicely
             * and allows .name / .isArray / etc field access */
            return resolve(expr, type_class_ref("Type"));
        }

        /* ── Nullable operators ──────────────────────────────────────────── */

        case EXPR_NULL_LIT:
            return resolve(expr, type_null());

        case EXPR_NULL_ASSERT: {
            /* expr! — operand must be nullable, result is the non-nullable base type */
            Type inner = check_expr(c, expr->null_assert.operand);
            if (inner.kind == TYPE_NULL) {
                type_error(c, expr->line,
                    "Cannot assert non-null on a null literal");
                return resolve(expr, type_any());
            }
            if (!inner.is_nullable && inner.kind != TYPE_ANY) {
                type_error(c, expr->line,
                    "Unnecessary null assertion: '%s' is already non-nullable",
                    type_kind_name(inner.kind));
            }
            Type result = inner;
            result.is_nullable = false;
            return resolve(expr, result);
        }

        case EXPR_NULL_COALESCE: {
            /* left ?? right — left must be nullable, result type is non-nullable base */
            Type left  = check_expr(c, expr->null_coalesce.left);
            Type right = check_expr(c, expr->null_coalesce.right);
            if (!left.is_nullable && left.kind != TYPE_NULL && left.kind != TYPE_ANY) {
                type_error(c, expr->line,
                    "'?\?' operator: left side is already non-nullable — operand of '?\?' must be nullable");
            }
            /* Result type is non-nullable base type of left (or right if null literal) */
            Type result = (left.kind == TYPE_NULL) ? right : left;
            result.is_nullable = false;
            /* Right must be compatible with left's base type */
            if (left.kind != TYPE_NULL && right.kind != TYPE_NULL &&
                !is_unknown(left) && !is_unknown(right)) {
                Type lb = left;  lb.is_nullable = false;
                Type rb = right; rb.is_nullable = false;
                if (!type_equals(lb, rb)) {
                    type_error(c, expr->line,
                        "'?\?' operator: both sides must have the same base type");
                }
            }
            return resolve(expr, result);
        }

        case EXPR_NULL_SAFE_GET: {
            /* obj?.field — obj must be nullable, result is nullable version of field type */
            Type obj_type = check_expr(c, expr->null_safe_get.object);
            if (!obj_type.is_nullable && obj_type.kind != TYPE_ANY) {
                type_error(c, expr->line,
                    "'?.' operator: object is not nullable — use '.' instead");
            }
            if (obj_type.kind != TYPE_OBJECT && obj_type.kind != TYPE_ANY) {
                return error_type(c, expr, expr->line,
                    "'?.' operator: expected object type, got %s",
                    type_kind_name(obj_type.kind));
            }
            if (obj_type.kind == TYPE_ANY) return resolve(expr, type_any());
            /* Look up the field type same as EXPR_FIELD_GET */
            const char *cn = obj_type.class_name;
            if (!cn) return resolve(expr, type_any());
            Symbol *sym = lookup_symbol(c, cn, (int)strlen(cn));
            if (!sym || sym->kind != SYM_CLASS || !sym->class_decl)
                return resolve(expr, type_any());
            typedef struct ClassFieldNode CFNode;
            for (CFNode *f = sym->class_decl->class_decl.fields; f; f = f->next) {
                if (f->length == expr->null_safe_get.field_name_len &&
                    memcmp(f->name, expr->null_safe_get.field_name, f->length) == 0) {
                    Type ft = f->type;
                    ft.is_nullable = true; /* result is always nullable */
                    return resolve(expr, ft);
                }
            }
            return error_type(c, expr, expr->line,
                "Class '%s' has no field '%.*s'",
                cn, expr->null_safe_get.field_name_len, expr->null_safe_get.field_name);
        }

        case EXPR_NULL_SAFE_CALL: {
            /* obj?.method(args) — obj must be nullable, result is nullable method return type */
            Type obj_type = check_expr(c, expr->null_safe_call.object);
            if (!obj_type.is_nullable && obj_type.kind != TYPE_ANY) {
                type_error(c, expr->line,
                    "'?.' operator: object is not nullable — use '.' instead");
            }
            /* Check args (for side effects) */
            for (ArgNode *arg = expr->null_safe_call.args; arg; arg = arg->next)
                check_expr(c, arg->expr);
            if (obj_type.kind == TYPE_ANY) return resolve(expr, type_any());
            if (obj_type.kind != TYPE_OBJECT)
                return error_type(c, expr, expr->line,
                    "'?.' method call: expected object type, got %s",
                    type_kind_name(obj_type.kind));
            const char *cn = obj_type.class_name;
            if (!cn) return resolve(expr, type_any());
            Symbol *sym = lookup_symbol(c, cn, (int)strlen(cn));
            if (!sym || sym->kind != SYM_CLASS || !sym->class_decl)
                return resolve(expr, type_any());
            typedef struct ClassMethodNode CMNode;
            for (CMNode *m = sym->class_decl->class_decl.methods; m; m = m->next) {
                if (m->is_constructor) continue;
                if (m->fn->fn_decl.length == expr->null_safe_call.method_name_len &&
                    memcmp(m->fn->fn_decl.name,
                           expr->null_safe_call.method_name,
                           m->fn->fn_decl.length) == 0) {
                    Type ret = m->fn->fn_decl.return_type;
                    ret.is_nullable = true;  /* null-safe call always returns nullable */
                    return resolve(expr, ret);
                }
            }
            return error_type(c, expr, expr->line,
                "Class '%s' has no method '%.*s'",
                cn, expr->null_safe_call.method_name_len, expr->null_safe_call.method_name);
        }

        case EXPR_ARRAY_LIT: {
            /* {e0, e1, e2} — all elements must have compatible types.
             * For object types, different concrete classes are allowed as
             * long as they share a common ancestor or will be assigned to
             * an interface-typed array (checked in var_decl).
             * When types differ but are all TYPE_OBJECT, use a generic
             * TYPE_OBJECT (no class_name) so the declared type in var_decl
             * can impose the interface constraint. */
            if (expr->array_lit.count == 0) {
                return error_type(c, expr, expr->line,
                    "Cannot infer type of empty array literal — use new Type[0] instead");
            }
            Type elem_type = (Type){.kind=TYPE_UNKNOWN};
            bool all_objects = true;
            for (struct ArgNode *arg = expr->array_lit.elements; arg; arg = arg->next) {
                Type t = check_expr(c, arg->expr);
                if (t.kind != TYPE_OBJECT) all_objects = false;
                if (elem_type.kind == TYPE_UNKNOWN) {
                    elem_type = t;
                } else if (!types_assignable(elem_type, t) && !is_unknown(t)) {
                    /* If all elements are objects with different class names,
                     * use a generic TYPE_OBJECT — the declared type will
                     * impose the compatibility constraint in var_decl. */
                    if (elem_type.kind == TYPE_OBJECT && t.kind == TYPE_OBJECT) {
                        /* Check if t is a subtype of elem_type or vice versa */
                        if (elem_type.class_name && t.class_name) {
                            if (is_subtype(c, t.class_name, elem_type.class_name)) {
                                /* keep elem_type — it's the wider type */
                            } else if (is_subtype(c, elem_type.class_name, t.class_name)) {
                                elem_type = t; /* t is wider */
                            } else {
                                /* No class relationship — erase to generic object */
                                elem_type = (Type){.kind = TYPE_OBJECT, .class_name = NULL};
                            }
                        }
                    } else {
                        return error_type(c, expr, expr->line,
                            "Array literal elements have inconsistent types: %s and %s",
                            type_kind_name(elem_type.kind), type_kind_name(t.kind));
                    }
                }
            }
            (void)all_objects;
            Type *elem_ptr = arena_alloc(c->arena, sizeof(Type));
            *elem_ptr = elem_type;
            return resolve(expr, type_array(elem_ptr));
        }

        case EXPR_INDEX: {
            /* arr[i] */
            Type arr_t = check_expr(c, expr->index_expr.array);
            Type idx_t = check_expr(c, expr->index_expr.index);
            if (!is_unknown(arr_t) && arr_t.kind != TYPE_ARRAY) {
                return error_type(c, expr, expr->line,
                    "Cannot index into non-array type %s",
                    type_kind_name(arr_t.kind));
            }
            if (!is_unknown(idx_t) && !type_is_int_family(idx_t)) {
                return error_type(c, expr, expr->line,
                    "Array index must be an integer, got %s",
                    type_kind_name(idx_t.kind));
            }
            if (arr_t.kind == TYPE_ARRAY && arr_t.element_type)
                return resolve(expr, *arr_t.element_type);
            return resolve(expr, type_int()); /* fallback */
        }

        case EXPR_INDEX_ASSIGN: {
            /* arr[i] = value */
            Type arr_t = check_expr(c, expr->index_assign.array);
            Type idx_t = check_expr(c, expr->index_assign.index);
            Type val_t = check_expr(c, expr->index_assign.value);
            if (!is_unknown(arr_t) && arr_t.kind != TYPE_ARRAY) {
                return error_type(c, expr, expr->line,
                    "Cannot index-assign into non-array type %s",
                    type_kind_name(arr_t.kind));
            }
            if (!is_unknown(idx_t) && !type_is_int_family(idx_t)) {
                return error_type(c, expr, expr->line,
                    "Array index must be an integer, got %s",
                    type_kind_name(idx_t.kind));
            }
            if (arr_t.kind == TYPE_ARRAY && arr_t.element_type) {
                if (!is_unknown(val_t) &&
                    !types_assignable(*arr_t.element_type, val_t)) {
                    return error_type(c, expr, expr->line,
                        "Cannot store %s into array of %s",
                        type_kind_name(val_t.kind),
                        type_kind_name(arr_t.element_type->kind));
                }
                return resolve(expr, *arr_t.element_type);
            }
            return resolve(expr, val_t);
        }

        /* ── EnumName.Member ────────────────────────────────────────────── */
        case EXPR_ENUM_ACCESS:
            /* Already resolved by the EXPR_FIELD_GET rewrite path. */
            return resolve(expr, type_enum(expr->enum_access.enum_name));

        /* ── ClassName.staticField (already resolved) ────────────────── */
        case EXPR_STATIC_GET:
            return expr->resolved_type;

        /* ── ClassName.staticField = value (already resolved) ──────────── */
        case EXPR_STATIC_SET:
            return expr->resolved_type;

        /* ── ClassName.staticMethod(...) (already resolved) ────────────── */
        case EXPR_STATIC_CALL:
            return expr->resolved_type;

        /* ── obj.field ─────────────────────────────────────────────────── */
        case EXPR_FIELD_GET: {
            Type obj_type = check_expr(c, expr->field_get.object);

            /* ── Array .length ─────────────────────────────────────────────
             * arr.length is a special built-in property on arrays. */
                        /* ── Type object fields ──────────────────────────────────
             * typeof(x).name / .isArray / .isPrimitive / .isEnum / .isClass */
            if (obj_type.kind == TYPE_CLASS_REF &&
                strncmp(obj_type.class_name, "Type", 4) == 0 &&
                obj_type.class_name[4] == '\0') {
                const char *fn   = expr->field_get.field_name;
                int         flen = expr->field_get.field_name_len;
                if (flen == 4 && strncmp(fn, "name", 4) == 0)
                    return resolve(expr, (Type){.kind = TYPE_STRING});
                if ((flen==7  && strncmp(fn,"isArray",7)==0)     ||
                    (flen==11 && strncmp(fn,"isPrimitive",11)==0) ||
                    (flen==6  && strncmp(fn,"isEnum",6)==0)       ||
                    (flen==7  && strncmp(fn,"isClass",7)==0))
                    return resolve(expr, (Type){.kind = TYPE_BOOL});
                return error_type(c, expr, expr->line,
                    "Type has no field '%.*s' (available: name, isArray, isPrimitive, isEnum, isClass)",
                    flen, fn);
            }

if (obj_type.kind == TYPE_ARRAY) {
                const char *fname = expr->field_get.field_name;
                int         flen  = expr->field_get.field_name_len;
                if (flen == 6 && strncmp(fname, "length", 6) == 0) {
                    return resolve(expr, type_int());
                }
                return error_type(c, expr, expr->line,
                    "Array type has no field '%.*s' (only 'length' is available)",
                    flen, fname);
            }

            /* ── Static field access: ClassName.field ─────────────────────
             * If the object resolves to TYPE_CLASS_REF, this is a static
             * field access. Rewrite to EXPR_STATIC_GET. */
            if (obj_type.kind == TYPE_CLASS_REF) {
                const char *cname = obj_type.class_name;
                const char *fname = expr->field_get.field_name;
                int         flen  = expr->field_get.field_name_len;

                Symbol *cls_sym = lookup_symbol(c, cname, (int)strlen(cname));
                if (!cls_sym || cls_sym->kind != SYM_CLASS || !cls_sym->class_decl) {
                    return error_type(c, expr, expr->line,
                        "Unknown class '%s'", cname);
                }

                typedef struct ClassFieldNode CFNode;
                for (CFNode *f = cls_sym->class_decl->class_decl.fields; f; f = f->next) {
                    if (f->length == flen && memcmp(f->name, fname, flen) == 0) {
                        if (!f->is_static) {
                            return error_type(c, expr, expr->line,
                                "Field '%.*s' of class '%s' is not static — "
                                "use an instance to access it", flen, fname, cname);
                        }
                        expr->kind                     = EXPR_STATIC_GET;
                        expr->static_get.class_name     = cname;
                        expr->static_get.class_name_len = (int)strlen(cname);
                        expr->static_get.field_name     = fname;
                        expr->static_get.field_name_len = flen;
                        expr->static_get.class_idx      = -1; /* filled by compiler */
                        expr->static_get.field_idx      = -1;
                        return resolve(expr, f->type);
                    }
                }
                return error_type(c, expr, expr->line,
                    "Class '%s' has no static field '%.*s'", cname, flen, fname);
            }

            /* ── Enum member access: Direction.North ─────────────────────
             * If the object resolves to TYPE_ENUM, this is an enum member
             * access, not a field access. Rewrite to EXPR_ENUM_ACCESS. */
            if (obj_type.kind == TYPE_ENUM) {
                const char *ename     = obj_type.enum_name;
                const char *mname     = expr->field_get.field_name;
                int         mlen      = expr->field_get.field_name_len;

                Symbol *esym = lookup_symbol(c, ename, (int)strlen(ename));
                if (!esym || esym->kind != SYM_ENUM || !esym->enum_decl) {
                    return error_type(c, expr, expr->line,
                        "Internal: enum '%s' not found", ename);
                }

                typedef struct EnumMemberNode EMNode;
                for (EMNode *m = esym->enum_decl->enum_decl.members; m; m = m->next) {
                    if (m->length == mlen && memcmp(m->name, mname, mlen) == 0) {
                        /* Rewrite in-place to EXPR_ENUM_ACCESS */
                        expr->kind                      = EXPR_ENUM_ACCESS;
                        expr->enum_access.enum_name     = ename;
                        expr->enum_access.enum_name_len = (int)strlen(ename);
                        expr->enum_access.member_name    = mname;
                        expr->enum_access.member_name_len = mlen;
                        expr->enum_access.value          = m->value;
                        return resolve(expr, type_enum(ename));
                    }
                }
                return error_type(c, expr, expr->line,
                    "Enum '%s' has no member '%.*s'", ename, mlen, mname);
            }

            if (obj_type.kind != TYPE_OBJECT) {
                return error_type(c, expr, expr->line,
                    "Cannot access field on non-object type %s",
                    type_kind_name(obj_type.kind));
            }

            /* Generic class: resolve base name and collect concrete type args */
            const char *fg_lookup_name = obj_type.class_name;
            char        fg_base_buf[64];
            Type        fg_concrete_args[MAX_TYPE_ARGS];
            int         fg_concrete_count = 0;
            TypeParamNode *fg_type_params = NULL;
            if (is_generic_type_name(obj_type.class_name)) {
                if (generic_base_name(obj_type.class_name, fg_base_buf, sizeof(fg_base_buf))) {
                    fg_lookup_name = fg_base_buf;
                    fg_concrete_count = parse_canonical_type_args(
                        obj_type.class_name, fg_concrete_args, MAX_TYPE_ARGS);
                }
            }

            Symbol *cls_sym = lookup_symbol(c,
                fg_lookup_name, (int)strlen(fg_lookup_name));
            if (!cls_sym || cls_sym->kind != SYM_CLASS) {
                return error_type(c, expr, expr->line,
                    "Unknown class '%s'", fg_lookup_name);
            }
            /* Build type context from whichever source is available */
            fg_concrete_count = checker_class_type_context(c, obj_type.class_name,
                                    cls_sym, &fg_type_params, fg_concrete_args);

            /* First try the unified helper (works for both AST and ClassDef) */
            {
                const char *fname = expr->field_get.field_name;
                int         flen  = expr->field_get.field_name_len;
                Type ft = checker_field_type(cls_sym, fname, flen,
                                              fg_type_params, fg_concrete_args,
                                              fg_concrete_count);
                if (ft.kind != TYPE_UNKNOWN)
                    return resolve(expr, ft);
                /* Not found on this class -- fall through to inheritance walk (AST path) */
                if (!cls_sym->class_decl) {
                    /* XAR-only class with no AST -- can't walk inheritance, field not found */
                    return error_type(c, expr, expr->line,
                        "Class '%s' has no field '%.*s'",
                        fg_lookup_name, flen, fname);
                }
            }

            typedef struct ClassFieldNode CFNode;
            /* Walk the inheritance chain (AST path) for user-defined classes */
            Stmt *search_cls = cls_sym->class_decl;
            char  search_name[CLASS_NAME_MAX];
            int   search_len = (int)strlen(fg_lookup_name) < CLASS_NAME_MAX - 1
                             ? (int)strlen(fg_lookup_name) : CLASS_NAME_MAX - 1;
            memcpy(search_name, fg_lookup_name, search_len);
            search_name[search_len] = '\0';

            while (search_cls) {
                for (CFNode *f = search_cls->class_decl.fields; f; f = f->next) {
                    if (f->length == expr->field_get.field_name_len &&
                        memcmp(f->name, expr->field_get.field_name, f->length) == 0) {
                        /* Access check: private → same class only.
                         *               protected → same class or subclass. */
                        if (f->access != ACCESS_PUBLIC) {
                            bool allow_subclass = (f->access == ACCESS_PROTECTED);
                            if (!same_class_check(c, search_name, allow_subclass)) {
                                return error_type(c, expr, expr->line,
                                    "%s field '%.*s' of class '%s' is not accessible here",
                                    f->access == ACCESS_PRIVATE ? "Private" : "Protected",
                                    expr->field_get.field_name_len,
                                    expr->field_get.field_name,
                                    search_name);
                            }
                        }
                        Type fg_ftype = (fg_type_params && fg_concrete_count > 0)
                            ? substitute_type(f->type, fg_type_params,
                                              fg_concrete_args, fg_concrete_count)
                            : f->type;
                        return resolve(expr, fg_ftype);
                    }
                }
                /* Move to parent class -- propagating generic type args */
                if (search_cls->class_decl.parent_name && search_cls->class_decl.parent_length > 0) {
                    int plen = search_cls->class_decl.parent_length < CLASS_NAME_MAX - 1
                             ? search_cls->class_decl.parent_length : CLASS_NAME_MAX - 1;
                    memcpy(search_name, search_cls->class_decl.parent_name, plen);
                    search_name[plen] = '\0';
                    Symbol *psym = lookup_symbol(c, search_name, plen);
                    Stmt *next_cls = (psym && psym->kind == SYM_CLASS) ? psym->class_decl : NULL;
                    if (!next_cls && psym && psym->kind == SYM_CLASS && psym->class_def) {
                        /* Parent is a stdlib-only class (no AST) — search its ClassDef fields */
                        const ClassDef *cdef = (const ClassDef *)psym->class_def;
                        const char *fname = expr->field_get.field_name;
                        int flen = expr->field_get.field_name_len;
                        for (int _fi = 0; _fi < cdef->field_count; _fi++) {
                            const FieldDef *fd = &cdef->fields[_fi];
                            if ((int)strlen(fd->name) == flen && memcmp(fd->name, fname, flen) == 0) {
                                Type ft = {0};
                                ft.kind = fd->type_kind;
                                if (ft.kind == TYPE_OBJECT && fd->class_name[0])
                                    ft.class_name = fd->class_name;
                                return resolve(expr, ft);
                            }
                        }
                        /* Not in ClassDef — stop walking */
                        search_cls = NULL;
                        break;
                    }
                    if (next_cls && search_cls->class_decl.parent_type_arg_count > 0) {
                        fg_type_params    = next_cls->class_decl.type_params;
                        fg_concrete_count = search_cls->class_decl.parent_type_arg_count;
                        if (fg_concrete_count > MAX_TYPE_ARGS) fg_concrete_count = MAX_TYPE_ARGS;
                        for (int _ti = 0; _ti < fg_concrete_count; _ti++)
                            fg_concrete_args[_ti] = search_cls->class_decl.parent_type_args[_ti];
                    }
                    search_cls = next_cls;
                } else {
                    search_cls = NULL;
                }
            }
            return error_type(c, expr, expr->line,
                "Class '%s' has no field '%.*s'",
                fg_lookup_name,
                expr->field_get.field_name_len, expr->field_get.field_name);
        }

        /* ── obj.field = value ─────────────────────────────────────────── */
        case EXPR_FIELD_SET: {
            Type obj_type = check_expr(c, expr->field_set.object);
            Type val_type = check_expr(c, expr->field_set.value);

            /* ── Static field assignment: ClassName.field = value ──────── */
            if (obj_type.kind == TYPE_CLASS_REF) {
                const char *cname = obj_type.class_name;
                const char *fname = expr->field_set.field_name;
                int         flen  = expr->field_set.field_name_len;

                Symbol *cls_sym = lookup_symbol(c, cname, (int)strlen(cname));
                if (!cls_sym || cls_sym->kind != SYM_CLASS) {
                    return error_type(c, expr, expr->line, "Unknown class '%s'", cname);
                }
                /* Use unified helper for field type; fall back to AST for is_static/is_final */
                {
                    Type ft = checker_field_type(cls_sym, fname, flen, NULL, NULL, 0);
                    if (ft.kind == TYPE_UNKNOWN) {
                        return error_type(c, expr, expr->line,
                            "Class '%s' has no static field '%.*s'", cname, flen, fname);
                    }
                    bool is_static = true, is_final = false;
                    if (cls_sym->class_decl) {
                        typedef struct ClassFieldNode CFN;
                        for (CFN *f = cls_sym->class_decl->class_decl.fields; f; f = f->next) {
                            if (f->length == flen && memcmp(f->name, fname, flen) == 0) {
                                is_static = f->is_static;
                                is_final  = f->is_final;
                                break;
                            }
                        }
                    } else if (cls_sym->class_def) {
                        ClassDef *def = (ClassDef *)cls_sym->class_def;
                        for (int _fi = 0; _fi < def->field_count; _fi++) {
                            if ((int)strlen(def->fields[_fi].name) == flen &&
                                memcmp(def->fields[_fi].name, fname, flen) == 0) {
                                is_static = def->fields[_fi].is_static;
                                is_final  = def->fields[_fi].is_final;
                                break;
                            }
                        }
                    }
                    if (!is_static) {
                        return error_type(c, expr, expr->line,
                            "Field '%.*s' of '%s' is not static", flen, fname, cname);
                    }
                    if (is_final) {
                        return error_type(c, expr, expr->line,
                            "Cannot assign to final field '%.*s'", flen, fname);
                    }
                    if (!types_assignable(ft, val_type) && !is_unknown(val_type)) {
                        return error_type(c, expr, expr->line,
                            "Cannot assign %s to static field '%.*s' of type %s",
                            type_kind_name(val_type.kind), flen, fname,
                            type_kind_name(ft.kind));
                    }
                    Expr *saved_value              = expr->field_set.value;
                    expr->kind                     = EXPR_STATIC_SET;
                    expr->static_set.class_name     = cname;
                    expr->static_set.class_name_len = (int)strlen(cname);
                    expr->static_set.field_name     = fname;
                    expr->static_set.field_name_len = flen;
                    expr->static_set.value          = saved_value;
                    expr->static_set.class_idx      = -1;
                    expr->static_set.field_idx      = -1;
                    return resolve(expr, ft);
                }
            }

            if (obj_type.kind != TYPE_OBJECT) {
                return error_type(c, expr, expr->line,
                    "Cannot set field on non-object type %s",
                    type_kind_name(obj_type.kind));
            }

            /* Generic class: resolve base name */
            const char *fs_lookup_name = obj_type.class_name;
            char        fs_base_buf[64];
            Type        fs_concrete_args[MAX_TYPE_ARGS];
            int         fs_concrete_count = 0;
            TypeParamNode *fs_type_params = NULL;
            if (is_generic_type_name(obj_type.class_name)) {
                if (generic_base_name(obj_type.class_name, fs_base_buf, sizeof(fs_base_buf))) {
                    fs_lookup_name = fs_base_buf;
                    fs_concrete_count = parse_canonical_type_args(
                        obj_type.class_name, fs_concrete_args, MAX_TYPE_ARGS);
                }
            }

            Symbol *cls_sym = lookup_symbol(c,
                fs_lookup_name, (int)strlen(fs_lookup_name));
            if (!cls_sym || cls_sym->kind != SYM_CLASS) {
                return error_type(c, expr, expr->line,
                    "Unknown class '%s'", fs_lookup_name);
            }
            /* Build type context (works for AST and ClassDef) */
            fs_concrete_count = checker_class_type_context(c, obj_type.class_name,
                                    cls_sym, &fs_type_params, fs_concrete_args);

            /* Unified field lookup */
            {
                const char *fname = expr->field_set.field_name;
                int         flen  = expr->field_set.field_name_len;
                Type ft = checker_field_type(cls_sym, fname, flen,
                                              fs_type_params, fs_concrete_args,
                                              fs_concrete_count);
                if (ft.kind != TYPE_UNKNOWN && !cls_sym->class_decl) {
                    /* XAR-only class -- no AST to walk, field found via ClassDef */
                    if (!types_assignable(ft, val_type) && !is_unknown(val_type)) {
                        return error_type(c, expr, expr->line,
                            "Cannot assign %s to field '%.*s' of type %s",
                            type_kind_name(val_type.kind), flen, fname,
                            type_kind_name(ft.kind));
                    }
                    return resolve(expr, ft);
                }
                if (ft.kind == TYPE_UNKNOWN && !cls_sym->class_decl) {
                    return error_type(c, expr, expr->line,
                        "Class '%s' has no field '%.*s'", fs_lookup_name, flen, fname);
                }
            }

            typedef struct ClassFieldNode CFNode;
            /* Walk the inheritance chain (AST path) */
            Stmt *search_cls = cls_sym->class_decl;
            char  search_name[CLASS_NAME_MAX];
            int   search_len = (int)strlen(fs_lookup_name) < CLASS_NAME_MAX - 1
                             ? (int)strlen(fs_lookup_name) : CLASS_NAME_MAX - 1;
            memcpy(search_name, fs_lookup_name, search_len);
            search_name[search_len] = '\0';

            while (search_cls) {
                for (CFNode *f = search_cls->class_decl.fields; f; f = f->next) {
                    if (f->length == expr->field_set.field_name_len &&
                        memcmp(f->name, expr->field_set.field_name, f->length) == 0) {
                        /* Access check */
                        if (f->access != ACCESS_PUBLIC) {
                            bool allow_subclass = (f->access == ACCESS_PROTECTED);
                            if (!same_class_check(c, search_name, allow_subclass)) {
                                return error_type(c, expr, expr->line,
                                    "%s field '%.*s' of class '%s' is not accessible here",
                                    f->access == ACCESS_PRIVATE ? "Private" : "Protected",
                                    expr->field_set.field_name_len,
                                    expr->field_set.field_name,
                                    search_name);
                            }
                        }
                        /* final fields may only be assigned inside the constructor */
                        if (f->is_final) {
                            if (!c->in_constructor) {
                                return error_type(c, expr, expr->line,
                                    "Cannot assign to final field '%.*s' outside constructor",
                                    expr->field_set.field_name_len,
                                    expr->field_set.field_name);
                            }
                            /* Mark this field as definitely assigned in the constructor */
                            typedef struct ClassFieldNode CFNode2;
                            int fidx = 0;
                            if (c->current_class) {
                                for (CFNode2 *ff = c->current_class->class_decl.fields;
                                     ff; ff = ff->next, fidx++) {
                                    if (ff == f) {
                                        if (fidx < 64) c->final_field_assigned[fidx] = true;
                                        break;
                                    }
                                }
                            }
                        }
                        Type fs_field_type = (fs_type_params && fs_concrete_count > 0)
                            ? substitute_type(f->type, fs_type_params,
                                              fs_concrete_args, fs_concrete_count)
                            : f->type;
                        if (!types_assignable(fs_field_type, val_type) && !is_unknown(val_type)
                            && val_type.kind != TYPE_ANY) {
                            return error_type(c, expr, expr->line,
                                "Cannot assign %s to field '%.*s' of type %s",
                                type_kind_name(val_type.kind),
                                expr->field_set.field_name_len, expr->field_set.field_name,
                                type_kind_name(f->type.kind));
                        }
                        Type fs_ftype = (fs_type_params && fs_concrete_count > 0)
                            ? substitute_type(f->type, fs_type_params,
                                              fs_concrete_args, fs_concrete_count)
                            : f->type;
                        return resolve(expr, fs_ftype);
                    }
                }
                /* Move to parent class -- propagating generic type args */
                if (search_cls->class_decl.parent_name && search_cls->class_decl.parent_length > 0) {
                    int plen = search_cls->class_decl.parent_length < CLASS_NAME_MAX - 1
                             ? search_cls->class_decl.parent_length : CLASS_NAME_MAX - 1;
                    memcpy(search_name, search_cls->class_decl.parent_name, plen);
                    search_name[plen] = '\0';
                    Symbol *psym = lookup_symbol(c, search_name, plen);
                    Stmt *next_cls = (psym && psym->kind == SYM_CLASS) ? psym->class_decl : NULL;
                    if (!next_cls && psym && psym->kind == SYM_CLASS && psym->class_def) {
                        /* Parent is a stdlib-only class — search ClassDef fields */
                        const ClassDef *cdef = (const ClassDef *)psym->class_def;
                        const char *fname = expr->field_set.field_name;
                        int flen = expr->field_set.field_name_len;
                        for (int _fi = 0; _fi < cdef->field_count; _fi++) {
                            const FieldDef *fd = &cdef->fields[_fi];
                            if ((int)strlen(fd->name) == flen && memcmp(fd->name, fname, flen) == 0) {
                                Type ft = {0}; ft.kind = fd->type_kind;
                                if (ft.kind == TYPE_OBJECT && fd->class_name[0]) ft.class_name = fd->class_name;
                                return resolve(expr, ft);
                            }
                        }
                        search_cls = NULL; break;
                    }
                    if (next_cls && search_cls->class_decl.parent_type_arg_count > 0) {
                        fs_type_params    = next_cls->class_decl.type_params;
                        fs_concrete_count = search_cls->class_decl.parent_type_arg_count;
                        if (fs_concrete_count > MAX_TYPE_ARGS) fs_concrete_count = MAX_TYPE_ARGS;
                        for (int _ti = 0; _ti < fs_concrete_count; _ti++)
                            fs_concrete_args[_ti] = search_cls->class_decl.parent_type_args[_ti];
                    }
                    search_cls = next_cls;
                } else {
                    search_cls = NULL;
                }
            }
            return error_type(c, expr, expr->line,
                "Class '%s' has no field '%.*s'",
                fs_lookup_name,
                expr->field_set.field_name_len, expr->field_set.field_name);
        }

        /* ── obj.method(args) ──────────────────────────────────────────── */
        case EXPR_METHOD_CALL: {
            Type obj_type = check_expr(c, expr->method_call.object);

            /* ── Type object methods: typeof(x).hasAttribute / .getAttributeArg ── */
            if (obj_type.kind == TYPE_CLASS_REF &&
                obj_type.class_name &&
                strcmp(obj_type.class_name, "Type") == 0) {
                const char *mname = expr->method_call.method_name;
                int         mlen  = expr->method_call.method_name_len;
                if (mlen == 12 && memcmp(mname, "hasAttribute", 12) == 0) {
                    /* hasAttribute(name: string) -> bool */
                    if (expr->method_call.arg_count != 1) {
                        return error_type(c, expr, expr->line,
                            "hasAttribute expects 1 argument (attribute name), got %d",
                            expr->method_call.arg_count);
                    }
                    check_expr(c, expr->method_call.args->expr);
                    return resolve(expr, type_bool());
                }
                if (mlen == 15 && memcmp(mname, "getAttributeArg", 15) == 0) {
                    /* getAttributeArg(name: string, index: int) -> string? */
                    if (expr->method_call.arg_count != 2) {
                        return error_type(c, expr, expr->line,
                            "getAttributeArg expects 2 arguments (name, index), got %d",
                            expr->method_call.arg_count);
                    }
                    check_expr(c, expr->method_call.args->expr);
                    if (expr->method_call.args->next)
                        check_expr(c, expr->method_call.args->next->expr);
                    Type ret = type_string();
                    ret.is_nullable = true;
                    return resolve(expr, ret);
                }
                return error_type(c, expr, expr->line,
                    "Type has no method '%.*s' (available: hasAttribute, getAttributeArg)",
                    mlen, mname);
            }

            /* ── Static method call: ClassName.method(args) ─────────────── */
            if (obj_type.kind == TYPE_CLASS_REF) {
                const char *cname  = obj_type.class_name;
                const char *mname  = expr->method_call.method_name;
                int         mlen   = expr->method_call.method_name_len;

                Symbol *cls_sym = lookup_symbol(c, cname, (int)strlen(cname));
                if (!cls_sym || cls_sym->kind != SYM_CLASS) {
                    return error_type(c, expr, expr->line, "Unknown class '%s'", cname);
                }

                /* Type-check all arguments first */
                for (ArgNode *arg = expr->method_call.args; arg; arg = arg->next)
                    check_expr(c, arg->expr);

                /* AST path: full static method resolution with arg checking */
                if (cls_sym->class_decl) {
                    typedef struct ClassMethodNode CMNode;
                    for (CMNode *m = cls_sym->class_decl->class_decl.methods; m; m = m->next) {
                        if (m->is_constructor) continue;
                        if (!m->is_static) continue;
                        if (m->fn->fn_decl.length == mlen &&
                            memcmp(m->fn->fn_decl.name, mname, mlen) == 0) {
                            int required = count_required_params(m->fn->fn_decl.params);
                            int total    = count_params(m->fn->fn_decl.params);
                            if (expr->method_call.arg_count < required ||
                                expr->method_call.arg_count > total) {
                                return error_type(c, expr, expr->line,
                                    required == total
                                        ? "Static method '%.*s' expects %d argument(s), got %d"
                                        : "Static method '%.*s' expects %d-%d argument(s), got %d",
                                    mlen, mname, required, total,
                                    expr->method_call.arg_count);
                            }
                            ArgNode *saved_args = expr->method_call.args;
                            int      saved_argc = expr->method_call.arg_count;
                            expr->kind                       = EXPR_STATIC_CALL;
                            expr->static_call.class_name      = cname;
                            expr->static_call.class_name_len  = (int)strlen(cname);
                            expr->static_call.method_name     = mname;
                            expr->static_call.method_name_len = mlen;
                            expr->static_call.args            = saved_args;
                            expr->static_call.arg_count       = saved_argc;
                            expr->static_call.fn_index        = -1;
                            return resolve(expr, m->fn->fn_decl.return_type);
                        }
                    }
                    return error_type(c, expr, expr->line,
                        "Class '%s' has no static method '%.*s'", cname, mlen, mname);
                }
                /* ClassDef path (XAR-loaded): use unified helper */
                {
                    Type rt = checker_method_return_type(cls_sym, mname, mlen, NULL, NULL, 0);
                    if (rt.kind == TYPE_UNKNOWN) {
                        return error_type(c, expr, expr->line,
                            "Class '%s' has no static method '%.*s'", cname, mlen, mname);
                    }
                    ArgNode *saved_args = expr->method_call.args;
                    int      saved_argc = expr->method_call.arg_count;
                    expr->kind                       = EXPR_STATIC_CALL;
                    expr->static_call.class_name      = cname;
                    expr->static_call.class_name_len  = (int)strlen(cname);
                    expr->static_call.method_name     = mname;
                    expr->static_call.method_name_len = mlen;
                    expr->static_call.args            = saved_args;
                    expr->static_call.arg_count       = saved_argc;
                    expr->static_call.fn_index        = -1;
                    return resolve(expr, rt);
                }
            }

            /* ── Primitive extension methods ─────────────────────────────
             * x.method(args) on int/float/bool/string desugars to a call
             * to {type_name}_{method_name}(x, args...) — a free function
             * defined in the core stdlib (e.g. int_toString, float_clamp).
             *
             * The checker rewrites the AST node in-place from EXPR_METHOD_CALL
             * to EXPR_CALL so the compiler emits a plain OP_CALL. */
            if (obj_type.kind == TYPE_INT   || obj_type.kind == TYPE_FLOAT ||
                obj_type.kind == TYPE_BOOL  || obj_type.kind == TYPE_STRING) {

                const char *type_prefix;
                switch (obj_type.kind) {
                    case TYPE_INT:    type_prefix = "int";    break;
                    case TYPE_FLOAT:  type_prefix = "float";  break;
                    case TYPE_BOOL:   type_prefix = "bool";   break;
                    default:          type_prefix = "string"; break;
                }
                const char *mname = expr->method_call.method_name;
                int         mlen  = expr->method_call.method_name_len;

                /* Build "{type_prefix}_{method_name}" in a buffer */
                char fn_name[128];
                int prefix_len = (int)strlen(type_prefix);
                if (prefix_len + 1 + mlen >= (int)sizeof(fn_name)) {
                    return error_type(c, expr, expr->line,
                        "Primitive extension method name too long");
                }
                memcpy(fn_name, type_prefix, prefix_len);
                fn_name[prefix_len] = '_';
                memcpy(fn_name + prefix_len + 1, mname, mlen);
                fn_name[prefix_len + 1 + mlen] = '\0';

                /* Look up the free function in scope */
                Symbol *fn_sym = lookup_symbol(c, fn_name, (int)strlen(fn_name));
                if (!fn_sym || fn_sym->kind != SYM_FN) {
                    return error_type(c, expr, expr->line,
                        "'%s' has no extension method '%.*s' (looked for '%s')",
                        type_prefix, mlen, mname, fn_name);
                }

                /* Build the rewritten arg list: receiver first, then original args */
                /* We reuse the existing expr node — rebuild as EXPR_CALL */
                Expr *receiver = expr->method_call.object;
                ArgNode *orig_args = expr->method_call.args;
                int       orig_argc = expr->method_call.arg_count;

                /* Prepend receiver to arg list */
                ArgNode *receiver_arg = arena_alloc(c->arena, sizeof(ArgNode));
                receiver_arg->expr = receiver;
                receiver_arg->next = orig_args;

                /* Intern the function name string in the arena */
                char *fn_name_copy = arena_alloc(c->arena, strlen(fn_name) + 1);
                memcpy(fn_name_copy, fn_name, strlen(fn_name) + 1);

                /* Rewrite in-place to EXPR_CALL */
                expr->kind                   = EXPR_CALL;
                expr->call.name              = fn_name_copy;
                expr->call.length            = (int)strlen(fn_name_copy);
                expr->call.args              = receiver_arg;
                expr->call.arg_count         = orig_argc + 1;
                expr->call.type_args         = NULL;
                expr->call.type_arg_count    = 0;

                /* Type-check all arguments (receiver already checked above) */
                int ai = 0;
                for (ArgNode *arg = orig_args; arg; arg = arg->next, ai++)
                    check_expr(c, arg->expr);

                /* Return the function's declared return type.
                 * We use fn_sym->type directly — do NOT recurse into check_expr
                 * for this node, because declare_staging registers stdlib fns
                 * with type_any() which would lose the real return type. */
                if (fn_sym->fn_decl_node) {
                    /* Symbol was declared from parsed source — use AST return type */
                    return resolve(expr, fn_sym->fn_decl_node->fn_decl.return_type);
                }
                /* Fallback: use the type stored on the symbol */
                return resolve(expr, fn_sym->type);
            }

            if (obj_type.kind != TYPE_OBJECT) {
                return error_type(c, expr, expr->line,
                    "Cannot call method on non-object type %s",
                    type_kind_name(obj_type.kind));
            }

            /* Generic class: resolve base name and collect type args for substitution */
            const char *mc_lookup_name = obj_type.class_name;
            char        mc_base_buf[64];
            Type        mc_concrete_args[MAX_TYPE_ARGS];
            int         mc_concrete_count = 0;
            TypeParamNode *mc_type_params = NULL;
            if (obj_type.class_name && is_generic_type_name(obj_type.class_name)) {
                if (generic_base_name(obj_type.class_name, mc_base_buf, sizeof(mc_base_buf))) {
                    mc_lookup_name = mc_base_buf;
                    mc_concrete_count = parse_canonical_type_args(
                        obj_type.class_name, mc_concrete_args, MAX_TYPE_ARGS);
                }
            }

            /* ── Interface-typed variable: obj is IFoo, call its method ──
             * Look up the method in the interface's signature table and
             * return the declared return type. The concrete class's method
             * is resolved at runtime; the checker only verifies the call
             * is valid according to the interface contract. */
            {
                Symbol *maybe_iface = lookup_symbol(c,
                    obj_type.class_name, (int)strlen(obj_type.class_name));
                if (maybe_iface && maybe_iface->kind == SYM_INTERFACE) {
                    const char *mname = expr->method_call.method_name;
                    int         mlen  = expr->method_call.method_name_len;
                    IMNodeT *sig = iface_lookup_method(c,
                        maybe_iface->iface_name_buf, mname, mlen);
                    if (!sig) {
                        return error_type(c, expr, expr->line,
                            "Interface '%s' has no method '%.*s'",
                            maybe_iface->iface_name_buf, mlen, mname);
                    }
                    /* Type-check arguments against the interface signature */
                    ArgNode   *arg   = expr->method_call.args;
                    ParamNode *param = sig->params;
                    int idx = 0;
                    while (arg && param) {
                        Type at = check_expr(c, arg->expr);
                        if (!is_unknown(at) && !types_assignable(param->type, at)) {
                            type_error(c, expr->line,
                                "Method '%.*s' argument %d: expected %s, got %s",
                                mlen, mname, idx + 1,
                                type_kind_name(param->type.kind),
                                type_kind_name(at.kind));
                        }
                        arg = arg->next; param = param->next; idx++;
                    }
                    if (expr->method_call.arg_count < count_required_params(sig->params) ||
                        expr->method_call.arg_count > sig->param_count) {
                        int req = count_required_params(sig->params);
                        type_error(c, expr->line,
                            req == sig->param_count
                                ? "Method '%.*s' expects %d argument(s), got %d"
                                : "Method '%.*s' expects %d-%d argument(s), got %d",
                            mlen, mname, req, sig->param_count,
                            expr->method_call.arg_count);
                    }
                    return resolve(expr, sig->return_type);
                }
            }

            /* Set up mc_type_params now that we have the class symbol */
            /* ── Type parameter: T where T : IFoo — method call via constraint ──
             * If mc_lookup_name resolves to a SYM_VAR with TYPE_PARAM, the
             * caller is invoking a method on a generic type-param variable.
             * Validate the call against the constraint interface if one exists. */
            {
                Symbol *tp_sym = lookup_symbol(c, mc_lookup_name,
                                               (int)strlen(mc_lookup_name));
                if (tp_sym && tp_sym->kind == SYM_VAR &&
                    tp_sym->type.kind == TYPE_PARAM)
                {
                    /* Find the TypeParamNode for this parameter in current class
                     * or current function to get the constraint. */
                    const char *constraint = NULL;
                    int         constr_len = 0;
                    /* Check class-level type params */
                    if (c->current_class) {
                        for (TypeParamNode *tp = c->current_class->class_decl.type_params;
                             tp; tp = tp->next) {
                            if (tp->length == (int)strlen(mc_lookup_name) &&
                                strncmp(tp->name, mc_lookup_name, tp->length) == 0) {
                                constraint = tp->constraint;
                                constr_len = tp->constraint_len;
                                break;
                            }
                        }
                    }
                    /* Check function-level type params if not found in class */
                    if (!constraint && c->current_fn_stmt) {
                        for (TypeParamNode *tp = c->current_fn_stmt->fn_decl.type_params;
                             tp; tp = tp->next) {
                            if (tp->length == (int)strlen(mc_lookup_name) &&
                                strncmp(tp->name, mc_lookup_name, tp->length) == 0) {
                                constraint = tp->constraint;
                                constr_len = tp->constraint_len;
                                break;
                            }
                        }
                    }
                    if (!constraint) {
                        /* No constraint — only built-in methods allowed, error otherwise */
                        return error_type(c, expr, expr->line,
                            "Cannot call method '%.*s' on unconstrained type parameter '%s'",
                            expr->method_call.method_name_len,
                            expr->method_call.method_name,
                            mc_lookup_name);
                    }
                    /* Look up the constraint interface and validate the method */
                    char cbuf[64];
                    int cbl = constr_len < 63 ? constr_len : 63;
                    memcpy(cbuf, constraint, cbl); cbuf[cbl] = '\0';
                    IMNodeT *sig = iface_lookup_method(c, cbuf,
                        expr->method_call.method_name,
                        expr->method_call.method_name_len);
                    if (!sig) {
                        return error_type(c, expr, expr->line,
                            "Constraint '%s' has no method '%.*s'",
                            cbuf,
                            expr->method_call.method_name_len,
                            expr->method_call.method_name);
                    }
                    /* Type-check args */
                    ArgNode *arg = expr->method_call.args;
                    ParamNode *param = sig->params;
                    int idx = 0;
                    while (arg && param) {
                        Type at = check_expr(c, arg->expr);
                        if (!is_unknown(at) && !types_assignable(param->type, at)) {
                            type_error(c, expr->line,
                                "Method '%.*s' argument %d: expected %s, got %s",
                                expr->method_call.method_name_len,
                                expr->method_call.method_name, idx + 1,
                                type_kind_name(param->type.kind),
                                type_kind_name(at.kind));
                        }
                        arg = arg->next; param = param->next; idx++;
                    }
                    return resolve(expr, sig->return_type);
                }
            }

            Symbol *cls_sym = lookup_symbol(c,
                mc_lookup_name, (int)strlen(mc_lookup_name));
            if (cls_sym && cls_sym->kind == SYM_INTERFACE) {
                /* Object is typed as an interface — resolve method through interface.
                 * The concrete type args are in mc_concrete_args (from generic name). */
                IMNodeT *sig = iface_lookup_method(c, mc_lookup_name,
                    expr->method_call.method_name,
                    expr->method_call.method_name_len);
                if (!sig) {
                    return error_type(c, expr, expr->line,
                        "Interface '%s' has no method '%.*s'",
                        mc_lookup_name,
                        expr->method_call.method_name_len,
                        expr->method_call.method_name);
                }
                /* Substitute generic type params with concrete args */
                TypeParamNode *iface_tps = cls_sym->interface_decl
                    ? cls_sym->interface_decl->interface_decl.type_params : NULL;
                int iface_tpc = cls_sym->interface_decl
                    ? cls_sym->interface_decl->interface_decl.type_param_count : 0;
                Type ret = substitute_type(sig->return_type,
                                           iface_tps, mc_concrete_args, iface_tpc);
                /* Also substitute using the parsed type args from the generic name */
                if (mc_concrete_count > 0)
                    ret = substitute_type(sig->return_type,
                                          iface_tps, mc_concrete_args, mc_concrete_count);
                /* Type-check args */
                ArgNode *a = expr->method_call.args;
                ParamNode *p = sig->params;
                int idx = 0;
                while (a && p) {
                    Type at = check_expr(c, a->expr);
                    Type pt = substitute_type(p->type, iface_tps, mc_concrete_args,
                                              mc_concrete_count > 0 ? mc_concrete_count : iface_tpc);
                    if (!is_unknown(at) && !types_assignable(pt, at)) {
                        type_error(c, expr->line,
                            "Method '%.*s' argument %d: expected %s, got %s",
                            expr->method_call.method_name_len,
                            expr->method_call.method_name, idx + 1,
                            type_kind_name(pt.kind), type_kind_name(at.kind));
                    }
                    a = a->next; p = p->next; idx++;
                }
                return resolve(expr, ret);
            }
            if (!cls_sym || cls_sym->kind != SYM_CLASS) {
                return error_type(c, expr, expr->line,
                    "Unknown class '%s'", mc_lookup_name);
            }
            /* Build type context using unified helper */
            mc_concrete_count = checker_class_type_context(c, obj_type.class_name,
                                    cls_sym, &mc_type_params, mc_concrete_args);

            /* ClassDef path (XAR-loaded): resolve via unified helper */
            if (!cls_sym->class_decl) {
                const char *mname = expr->method_call.method_name;
                int         mlen  = expr->method_call.method_name_len;
                for (ArgNode *a = expr->method_call.args; a; a = a->next)
                    check_expr(c, a->expr);
                Type rt = checker_method_return_type(cls_sym, mname, mlen,
                                                      mc_type_params, mc_concrete_args,
                                                      mc_concrete_count);
                if (rt.kind == TYPE_UNKNOWN)
                    return resolve(expr, type_any()); /* method not found -- allow gracefully */
                return resolve(expr, rt);
            }
            typedef struct ClassMethodNode CMNode;
            /* Walk the inheritance chain looking for the method (AST path) */
            Stmt *search_cls = cls_sym->class_decl;
            char  search_name[CLASS_NAME_MAX];
            int   search_len = (int)strlen(mc_lookup_name) < CLASS_NAME_MAX - 1
                             ? (int)strlen(mc_lookup_name) : CLASS_NAME_MAX - 1;
            memcpy(search_name, mc_lookup_name, search_len);
            search_name[search_len] = '\0';

            while (search_cls) {
                for (CMNode *m = search_cls->class_decl.methods; m; m = m->next) {
                    if (m->is_constructor) continue;
                    if (m->fn->fn_decl.length == expr->method_call.method_name_len &&
                        memcmp(m->fn->fn_decl.name, expr->method_call.method_name,
                               m->fn->fn_decl.length) == 0) {
                        /* Access check */
                        if (m->access != ACCESS_PUBLIC) {
                            bool allow_subclass = (m->access == ACCESS_PROTECTED);
                            if (!same_class_check(c, search_name, allow_subclass)) {
                                return error_type(c, expr, expr->line,
                                    "%s method '%.*s' of class '%s' is not accessible here",
                                    m->access == ACCESS_PRIVATE ? "Private" : "Protected",
                                    expr->method_call.method_name_len,
                                    expr->method_call.method_name,
                                    search_name);
                            }
                        }
                        /* Type-check arguments — substitute T->concrete if generic */
                        ArgNode *arg = expr->method_call.args;
                        ParamNode *param = m->fn->fn_decl.params;
                        int idx = 0;
                        while (arg && param) {
                            Type at = check_expr(c, arg->expr);
                            Type expected_param = (mc_type_params && mc_concrete_count > 0)
                                ? substitute_type(param->type, mc_type_params,
                                                  mc_concrete_args, mc_concrete_count)
                                : param->type;
                            if (!is_unknown(at) && !is_unknown(expected_param) &&
                                !type_equals(at, expected_param) &&
                                !types_assignable(expected_param, at)) {
                                type_error(c, expr->line,
                                    "Method '%.*s' argument %d: expected %s, got %s",
                                    expr->method_call.method_name_len,
                                    expr->method_call.method_name,
                                    idx + 1,
                                    type_kind_name(expected_param.kind),
                                    type_kind_name(at.kind));
                            }
                            arg = arg->next; param = param->next; idx++;
                        }
                        /* Arg count check — allow omitting trailing defaulted params */
                        {
                            int req   = count_required_params(m->fn->fn_decl.params);
                            int total = count_params(m->fn->fn_decl.params);
                            if (expr->method_call.arg_count < req ||
                                expr->method_call.arg_count > total) {
                                type_error(c, expr->line,
                                    req == total
                                        ? "Method '%.*s' expects %d argument(s), got %d"
                                        : "Method '%.*s' expects %d-%d argument(s), got %d",
                                    expr->method_call.method_name_len,
                                    expr->method_call.method_name,
                                    req, total, expr->method_call.arg_count);
                            }
                            /* Store param list for compiler default-arg emission */
                            expr->method_call.resolved_params = m->fn->fn_decl.params;
                        }
                        /* Return type: substitute T->concrete if generic */
                        Type ret = (mc_type_params && mc_concrete_count > 0)
                            ? substitute_type(m->fn->fn_decl.return_type, mc_type_params,
                                              mc_concrete_args, mc_concrete_count)
                            : m->fn->fn_decl.return_type;
                        return resolve(expr, ret);
                    }
                }
                /* Move to parent class.
                 * If the parent is a generic class (e.g. Box<int>), update
                 * mc_type_params / mc_concrete_args for that level so that
                 * T -> int substitution works on inherited methods. */
                if (search_cls->class_decl.parent_name && search_cls->class_decl.parent_length > 0) {
                    int plen = search_cls->class_decl.parent_length < CLASS_NAME_MAX - 1
                             ? search_cls->class_decl.parent_length : CLASS_NAME_MAX - 1;
                    memcpy(search_name, search_cls->class_decl.parent_name, plen);
                    search_name[plen] = '\0';
                    Symbol *psym = lookup_symbol(c, search_name, plen);
                    Stmt *next_cls = (psym && psym->kind == SYM_CLASS) ? psym->class_decl : NULL;
                    if (!next_cls && psym && psym->kind == SYM_CLASS && psym->class_def) {
                        /* Parent is a stdlib-only class — check ClassDef methods */
                        const ClassDef *cdef = (const ClassDef *)psym->class_def;
                        const char *mname = expr->method_call.method_name;
                        int mlen = expr->method_call.method_name_len;
                        /* Type-check args (best effort without AST params) */
                        for (ArgNode *arg = expr->method_call.args; arg; arg = arg->next)
                            check_expr(c, arg->expr);
                        for (int _mi = 0; _mi < cdef->method_count; _mi++) {
                            if ((int)strlen(cdef->methods[_mi].name) == mlen &&
                                memcmp(cdef->methods[_mi].name, mname, mlen) == 0) {
                                int rtk = cdef->methods[_mi].return_type_kind;
                                Type raw = rtk == TYPE_INT    ? type_int()    :
                                           rtk == TYPE_FLOAT  ? type_float()  :
                                           rtk == TYPE_BOOL   ? type_bool()   :
                                           rtk == TYPE_STRING ? type_string() :
                                           rtk == TYPE_VOID   ? type_void()   : type_any();
                                return resolve(expr, raw);
                            }
                        }
                        return resolve(expr, type_any()); /* method not found in stdlib parent */
                    }
                    /* Activate generic substitution if entering a generic parent */
                    if (next_cls && search_cls->class_decl.parent_type_arg_count > 0) {
                        mc_type_params  = next_cls->class_decl.type_params;
                        mc_concrete_count = search_cls->class_decl.parent_type_arg_count;
                        if (mc_concrete_count > MAX_TYPE_ARGS) mc_concrete_count = MAX_TYPE_ARGS;
                        for (int _ti = 0; _ti < mc_concrete_count; _ti++)
                            mc_concrete_args[_ti] = search_cls->class_decl.parent_type_args[_ti];
                    }
                    search_cls = next_cls;
                } else {
                    search_cls = NULL;
                }
            }
            return error_type(c, expr, expr->line,
                "Class '%s' has no method '%.*s'",
                mc_lookup_name,
                expr->method_call.method_name_len,
                expr->method_call.method_name);
        }

        /* ── super(args) ────────────────────────────────────────────────── */
        case EXPR_SUPER_CALL: {
            if (!c->current_class) {
                return error_type(c, expr, expr->line,
                    "'super' can only be used inside a class");
            }
            if (!c->current_class->class_decl.parent_name ||
                 c->current_class->class_decl.parent_length == 0) {
                return error_type(c, expr, expr->line,
                    "'super' used in class '%.*s' which has no parent",
                    c->current_class->class_decl.length,
                    c->current_class->class_decl.name);
            }
            /* Look up the parent class */
            int plen = c->current_class->class_decl.parent_length;
            Symbol *psym = lookup_symbol(c,
                c->current_class->class_decl.parent_name, plen);
            if (!psym || psym->kind != SYM_CLASS) {
                return error_type(c, expr, expr->line,
                    "Parent class '%.*s' not found",
                    plen, c->current_class->class_decl.parent_name);
            }
            Stmt *pcls = psym->class_decl;
            /* Find the parent constructor and type-check args.
             * If parent is a stdlib class (loaded from XAR, no AST), we cannot
             * inspect the constructor signature — just check each arg and accept. */
            if (!pcls) {
                for (ArgNode *_arg = expr->super_call.args; _arg; _arg = _arg->next)
                    check_expr(c, _arg->expr);
                return resolve(expr, type_void());
            }
            /* If parent is generic, substitute T->concrete using parent_type_args. */
            TypeParamNode *super_tp  = NULL;
            Type           super_ca[8];
            int            super_cc  = 0;
            if (c->current_class->class_decl.parent_type_arg_count > 0) {
                super_tp = pcls->class_decl.type_params;
                super_cc = c->current_class->class_decl.parent_type_arg_count;
                if (super_cc > MAX_TYPE_ARGS) super_cc = MAX_TYPE_ARGS;
                for (int _ti = 0; _ti < super_cc; _ti++)
                    super_ca[_ti] = c->current_class->class_decl.parent_type_args[_ti];
            }
            typedef struct ClassMethodNode CMNode;
            for (CMNode *m = pcls->class_decl.methods; m; m = m->next) {
                if (!m->is_constructor) continue;
                ArgNode   *arg   = expr->super_call.args;
                ParamNode *param = m->fn->fn_decl.params;
                int idx = 0;
                while (arg && param) {
                    Type at = check_expr(c, arg->expr);
                    Type expected = (super_tp && super_cc > 0)
                        ? substitute_type(param->type, super_tp, super_ca, super_cc)
                        : param->type;
                    if (!type_equals(at, expected) &&
                        !is_unknown(at) && !is_unknown(expected)) {
                        type_error(c, expr->line,
                            "super() argument %d: expected %s, got %s",
                            idx + 1,
                            type_kind_name(expected.kind),
                            type_kind_name(at.kind));
                    }
                    arg = arg->next; param = param->next; idx++;
                }
                return resolve(expr, type_void());
            }
            /* Parent has no explicit constructor -- zero-arg super() is fine */
            return resolve(expr, type_void());
            /* Parent has no explicit constructor — zero-arg super() is fine */
            return resolve(expr, type_void());
        }

        case EXPR_EVENT_SUBSCRIBE:
        case EXPR_EVENT_UNSUBSCRIBE: {
            /* event += handler  /  event -= handler
             * handler may be:
             *   - bare name (static/top-level function)
             *   - this.method or obj.method (bound delegate) */
            const char *ename = expr->event_sub.event_name;
            int         elen  = expr->event_sub.event_name_len;
            const char *hname = expr->event_sub.handler_name;
            int         hlen  = expr->event_sub.handler_name_len;

            Symbol *esym = lookup_symbol(c, ename, elen);
            if (!esym || esym->kind != SYM_EVENT) {
                if (c->current_class) {
                    typedef struct ClassEventNode CEN;
                    for (CEN *e = c->current_class->class_decl.events; e; e = e->next) {
                        if (e->length == elen && memcmp(e->name, ename, elen) == 0) {
                            static Symbol temp_esym;
                            temp_esym.kind = SYM_EVENT;
                            temp_esym.param_count = e->param_count;
                            ParamNode *p = e->params;
                            for (int i = 0; i < e->param_count && i < MAX_PARAMS && p; i++, p = p->next) {
                                temp_esym.param_types[i] = p->type;
                            }
                            esym = &temp_esym;
                            break;
                        }
                    }
                }
                if (!esym || esym->kind != SYM_EVENT) {
                    return error_type(c, expr, expr->line,
                        "'%.*s' is not a declared event", elen, ename);
                }
            }

            if (expr->event_sub.object != NULL) {
                /* Bound delegate — check_expr the receiver, then look up the
                 * method on its class. */
                Type recv_type = check_expr(c, expr->event_sub.object);
                if (recv_type.kind != TYPE_OBJECT) {
                    return error_type(c, expr, expr->line,
                        "Bound delegate receiver must be an object");
                }
                /* Find the method on the receiver's class */
                Symbol *cls_sym = lookup_symbol(c, recv_type.class_name,
                                                (int)strlen(recv_type.class_name));
                bool found = false;
                if (cls_sym && cls_sym->class_decl) {
                    for (struct ClassMethodNode *m = cls_sym->class_decl->class_decl.methods;
                         m; m = m->next) {
                        if (m->is_constructor) continue;
                        if (m->fn->fn_decl.length == hlen &&
                            memcmp(m->fn->fn_decl.name, hname, hlen) == 0) {
                            /* Check param count matches event */
                            if (m->fn->fn_decl.param_count != esym->param_count) {
                                return error_type(c, expr, expr->line,
                                    "Handler '%.*s' has %d parameter(s) but event '%.*s' requires %d",
                                    hlen, hname, m->fn->fn_decl.param_count,
                                    elen, ename, esym->param_count);
                            }
                            found = true;
                            break;
                        }
                    }
                } else if (cls_sym && cls_sym->class_def) {
                    /* xar-loaded class */
                    const ClassDef *cdef = (const ClassDef *)cls_sym->class_def;
                    for (int mi = 0; mi < cdef->method_count; mi++) {
                        if ((int)strlen(cdef->methods[mi].name) == hlen &&
                            memcmp(cdef->methods[mi].name, hname, hlen) == 0) {
                            found = true;
                            break;
                        }
                    }
                }
                if (!found) {
                    return error_type(c, expr, expr->line,
                        "Method '%.*s' not found on type '%s'",
                        hlen, hname, recv_type.class_name);
                }
                return resolve(expr, type_void());
            }

            /* Static handler path */
            Symbol *hsym = lookup_symbol(c, hname, hlen);
            if (!hsym || hsym->kind != SYM_FN) {
                if (c->current_class) {
                    typedef struct ClassMethodNode CMN;
                    for (CMN *m = c->current_class->class_decl.methods; m; m = m->next) {
                        if (m->is_static && m->fn->fn_decl.length == hlen && memcmp(m->fn->fn_decl.name, hname, hlen) == 0) {
                            /* Check parameter count */
                            if (m->fn->fn_decl.param_count != esym->param_count) {
                                return error_type(c, expr, expr->line,
                                    "Handler '%.*s' has %d parameter(s) but event '%.*s' requires %d",
                                    hlen, hname, m->fn->fn_decl.param_count,
                                    elen, ename, esym->param_count);
                            }
                            /* Check parameter types */
                            ParamNode *p = m->fn->fn_decl.params;
                            for (int pi = 0; pi < esym->param_count; pi++) {
                                if (!p || !type_equals(p->type, esym->param_types[pi])) {
                                    return error_type(c, expr, expr->line,
                                        "Handler '%.*s' parameter %d type mismatch: "
                                        "event expects %s, handler has %s",
                                        hlen, hname, pi + 1,
                                        type_kind_name(esym->param_types[pi].kind),
                                        p ? type_kind_name(p->type.kind) : "none");
                                }
                                if (p) p = p->next;
                            }
                            return resolve(expr, type_void());
                        }
                    }
                }
                return error_type(c, expr, expr->line,
                    "'%.*s' is not a declared function — use 'this.method' for instance methods",
                    hlen, hname);
            }
            /* Check parameter count */
            if (hsym->param_count != esym->param_count) {
                return error_type(c, expr, expr->line,
                    "Handler '%.*s' has %d parameter(s) but event '%.*s' requires %d",
                    hlen, hname, hsym->param_count,
                    elen, ename, esym->param_count);
            }
            /* Check parameter types */
            for (int pi = 0; pi < esym->param_count; pi++) {
                if (!type_equals(hsym->param_types[pi], esym->param_types[pi])) {
                    return error_type(c, expr, expr->line,
                        "Handler '%.*s' parameter %d type mismatch: "
                        "event expects %s, handler has %s",
                        hlen, hname, pi + 1,
                        type_kind_name(esym->param_types[pi].kind),
                        type_kind_name(hsym->param_types[pi].kind));
                }
            }
            return resolve(expr, type_void());
        }

        case EXPR_EVENT_FIRE: {
            check_event_fire:; /* also reached via goto from EXPR_CALL rewrite */
            /* event(args) — verify event exists and args match signature */
            const char *ename = expr->event_fire.event_name;
            int         elen  = expr->event_fire.event_name_len;

            Symbol *esym = lookup_symbol(c, ename, elen);
            if (!esym || esym->kind != SYM_EVENT) {
                if (c->current_class) {
                    typedef struct ClassEventNode CEN;
                    for (CEN *e = c->current_class->class_decl.events; e; e = e->next) {
                        if (e->length == elen && memcmp(e->name, ename, elen) == 0) {
                            static Symbol temp_esym;
                            temp_esym.kind = SYM_EVENT;
                            temp_esym.param_count = e->param_count;
                            ParamNode *p = e->params;
                            for (int i = 0; i < e->param_count && i < MAX_PARAMS && p; i++, p = p->next) {
                                temp_esym.param_types[i] = p->type;
                            }
                            esym = &temp_esym;
                            break;
                        }
                    }
                }
                if (!esym || esym->kind != SYM_EVENT) {
                    return error_type(c, expr, expr->line,
                        "'%.*s' is not a declared event", elen, ename);
                }
            }
            /* Check arg count */
            if (expr->event_fire.arg_count != esym->param_count) {
                return error_type(c, expr, expr->line,
                    "Event '%.*s' requires %d argument(s), got %d",
                    elen, ename, esym->param_count, expr->event_fire.arg_count);
            }
            /* Check each arg type */
            ArgNode *arg = expr->event_fire.args;
            for (int ai = 0; ai < esym->param_count && arg; ai++, arg = arg->next) {
                Type got = check_expr(c, arg->expr);
                if (!types_assignable(esym->param_types[ai], got)) {
                    type_error(c, expr->line,
                        "Event '%.*s' argument %d: expected %s, got %s",
                        elen, ename, ai + 1,
                        type_kind_name(esym->param_types[ai].kind),
                        type_kind_name(got.kind));
                }
            }
            return resolve(expr, type_void());
        }

        default:
            return error_type(c, expr, expr->line,
                "Internal: unhandled expression kind %d", expr->kind);
    }
}

/* ─────────────────────────────────────────────────────────────────────────────
 * INTERNAL: TYPE CHECKING — STATEMENTS
 * ───────────────────────────────────────────────────────────────────────────*/

static void check_stmt(Checker *c, Stmt *stmt);  /* forward decl */

static void check_block(Checker *c, Stmt *block_stmt) {
    /* Each block gets its own scope */
    push_scope(c);
    for (StmtNode *n = block_stmt->block.stmts; n; n = n->next) {
        check_stmt(c, n->stmt);
    }
    pop_scope(c);
}

static void check_stmt(Checker *c, Stmt *stmt) {
    switch (stmt->kind) {

        /* ── Variable declaration ──────────────────────────────────────── */
        case STMT_VAR_DECL: {
            Type declared = stmt->var_decl.type;

            /* If the declared type looks like a class name (TYPE_OBJECT) but
             * resolves to an enum in the symbol table, patch it to TYPE_ENUM. */
            if (declared.kind == TYPE_OBJECT && declared.class_name) {
                const char *dname = declared.class_name;

                /* Generic type: "Stack<int>" — resolve base name and validate */
                if (is_generic_type_name(dname)) {
                    char base[64];
                    if (generic_base_name(dname, base, sizeof(base))) {
                        Symbol *cls_sym = lookup_symbol(c, base, (int)strlen(base));
                        if (!cls_sym || (cls_sym->kind != SYM_CLASS && cls_sym->kind != SYM_INTERFACE)) {
                            type_error(c, stmt->line,
                                "Unknown generic class '%s'", base);
                        } else if (cls_sym->kind == SYM_INTERFACE) {
                            /* Generic interface type (e.g. IEnumerator<int>) — valid as
                             * a declared type, no further type-arg validation needed here. */
                            (void)0;
                        } else {
                            /* Validate type arg count — works from AST or ClassDef */
                            Type targs[MAX_TYPE_ARGS];
                            int  targ_count = parse_canonical_type_args(dname, targs, MAX_TYPE_ARGS);
                            int  tp_count = 0;
                            if (cls_sym->class_decl)
                                tp_count = cls_sym->class_decl->class_decl.type_param_count;
                            else if (cls_sym->class_def)
                                tp_count = ((ClassDef *)cls_sym->class_def)->type_param_count;
                            if (tp_count > 0 && targ_count != tp_count) {
                                type_error(c, stmt->line,
                                    "Class '%s' expects %d type argument(s), got %d",
                                    base, tp_count, targ_count);
                            } else if (cls_sym->class_decl && tp_count > 0) {
                                /* Constraint checking requires AST */
                                TypeParamNode *tp = cls_sym->class_decl->class_decl.type_params;
                                for (int i = 0; i < targ_count && tp; i++, tp = tp->next) {
                                    if (!check_type_constraint(c, targs[i],
                                                               tp->constraint, tp->constraint_len))
                                    {
                                        type_error(c, stmt->line,
                                            "Type argument %d for '%s': does not satisfy constraint '%.*s'",
                                            i + 1, base, tp->constraint_len, tp->constraint);
                                    }
                                }
                            }
                        }
                    }
                    /* The declared type stays as TYPE_OBJECT "Stack<int>" — fine for
                     * assignment compatibility checking below (erasure model). */
                }
                else {
                    Symbol *maybe_sym = lookup_symbol(c,
                        dname, (int)strlen(dname));
                    if (maybe_sym && maybe_sym->kind == SYM_ENUM) {
                        declared = type_enum(maybe_sym->enum_name_buf);
                        stmt->var_decl.type = declared;
                    }
                    /* Interface type: keep as TYPE_OBJECT but validate it exists */
                    else if (maybe_sym && maybe_sym->kind == SYM_INTERFACE) {
                        /* already TYPE_OBJECT with class_name = interface name — fine */
                    }
                }
            }

            /* If the declared type is an array of TYPE_OBJECT that resolves to an
             * enum (e.g. Target[]), upgrade the element type to TYPE_ENUM. */
            if (declared.kind == TYPE_ARRAY && declared.element_type &&
                declared.element_type->kind == TYPE_OBJECT &&
                declared.element_type->class_name) {
                Symbol *esym = lookup_symbol(c,
                    declared.element_type->class_name,
                    (int)strlen(declared.element_type->class_name));
                if (esym && esym->kind == SYM_ENUM) {
                    /* Patch element type in place — element_type is arena-allocated */
                    Type *elem_heap = (Type *)declared.element_type;
                    elem_heap->kind      = TYPE_ENUM;
                    elem_heap->enum_name = esym->enum_name_buf;
                }
            }

            /* void variables are not allowed */
            if (declared.kind == TYPE_VOID) {
                type_error(c, stmt->line,
                    "Variable '%.*s' cannot have type void",
                    stmt->var_decl.length, stmt->var_decl.name);
                return;
            }

            /* Type-check the initializer if present */
            if (stmt->var_decl.init) {
                Type init_type = check_expr(c, stmt->var_decl.init);
                /* Allow: exact match, int-family mix, subclass, OR
                 *        concrete class that implements the declared interface */
                bool is_iface_var = false;
                if (declared.kind == TYPE_OBJECT && declared.class_name) {
                    Symbol *dsym = lookup_symbol(c,
                        declared.class_name, (int)strlen(declared.class_name));
                    is_iface_var = (dsym && dsym->kind == SYM_INTERFACE);
                }
                /* Check if declared is IThing[] and init is a generic object array */
                bool iface_array_ok = false;
                if (declared.kind == TYPE_ARRAY && init_type.kind == TYPE_ARRAY &&
                    declared.element_type && init_type.element_type &&
                    declared.element_type->kind == TYPE_OBJECT &&
                    declared.element_type->class_name &&
                    init_type.element_type->kind == TYPE_OBJECT &&
                    init_type.element_type->class_name == NULL) {
                    Symbol *esym = lookup_symbol(c,
                        declared.element_type->class_name,
                        (int)strlen(declared.element_type->class_name));
                    iface_array_ok = (esym && esym->kind == SYM_INTERFACE);
                }
                /* Generic type compatibility: Box<int> declared, Box assigned.
                 * Extract base name from declared and compare to init type. */
                bool generic_compat = false;
                if (declared.kind == TYPE_OBJECT && init_type.kind == TYPE_OBJECT &&
                    declared.class_name && init_type.class_name &&
                    is_generic_type_name(declared.class_name))
                {
                    char base[64];
                    if (generic_base_name(declared.class_name, base, sizeof(base)))
                        generic_compat = (strcmp(base, init_type.class_name) == 0);
                }
                bool compatible = types_assignable(declared, init_type) ||
                    (declared.kind == TYPE_OBJECT && init_type.kind == TYPE_OBJECT &&
                     declared.class_name && init_type.class_name &&
                     is_subtype(c, init_type.class_name, declared.class_name)) ||
                    (is_iface_var && declared.class_name &&
                     type_is_assignable_to_interface(c, init_type, declared.class_name)) ||
                    iface_array_ok ||
                    generic_compat;
                if (!is_unknown(init_type) && !compatible)
                {
                    type_error(c, stmt->line,
                        "Cannot initialize '%.*s' (type %s) with %s",
                        stmt->var_decl.length, stmt->var_decl.name,
                        type_kind_name(declared.kind),
                        type_kind_name(init_type.kind));
                }
            }

            /* Warn on narrowing conversions (e.g. int -> byte).
             * Suppress for integer literals — the programmer typed an explicit
             * value and knows what they're doing. Warn for variable/expression
             * sources where a runtime value could be out of range. */
            if (stmt->var_decl.init) {
                ExprKind ek = stmt->var_decl.init->kind;
                bool is_literal = (ek == EXPR_INT_LIT || ek == EXPR_FLOAT_LIT ||
                                   ek == EXPR_BOOL_LIT || ek == EXPR_STRING_LIT);
                if (!is_literal) {
                    Type init_t = stmt->var_decl.init->resolved_type;
                    warn_if_narrowing(c, stmt->line, declared.kind, init_t.kind);
                }
            }

            /* Register the variable in the current scope */
            Symbol sym = {0};
            sym.kind   = SYM_VAR;
            sym.name   = stmt->var_decl.name;
            sym.length = stmt->var_decl.length;
            sym.type   = declared;
            sym_set_loc(&sym, c->source_file, stmt->line, stmt->col);

            if (!define_symbol(c, sym)) {
                type_error(c, stmt->line,
                    "Variable '%.*s' already declared in this scope",
                    stmt->var_decl.length, stmt->var_decl.name);
            } else {
                /* Record a usage at the declaration site so LSP hover/definition
                 * work when the cursor is on the variable name being declared. */
                Scope *sc = &c->scopes[c->scope_depth];
                Symbol *stored = &sc->symbols[sc->count - 1];
                record_usage(c, stored, stmt->line, stmt->col, stmt->var_decl.length);
            }
            break;
        }

        /* ── Expression statement ──────────────────────────────────────── */
        case STMT_EXPR:
            check_expr(c, stmt->expr.expr);
            break;

        /* ── Block ─────────────────────────────────────────────────────── */
        case STMT_BLOCK:
            check_block(c, stmt);
            break;

        /* ── If statement ──────────────────────────────────────────────── */
        case STMT_IF: {
            Type cond_type = check_expr(c, stmt->if_stmt.condition);
            if (!is_unknown(cond_type) && cond_type.kind != TYPE_BOOL) {
                type_error(c, stmt->line,
                    "If condition must be bool, got %s",
                    type_kind_name(cond_type.kind));
            }
            check_block(c, stmt->if_stmt.then_branch);
            if (stmt->if_stmt.else_branch) {
                /* else branch can be a block OR another if (else if) */
                if (stmt->if_stmt.else_branch->kind == STMT_BLOCK)
                    check_block(c, stmt->if_stmt.else_branch);
                else
                    check_stmt(c, stmt->if_stmt.else_branch);
            }
            break;
        }

        /* ── While loop ────────────────────────────────────────────────── */
        case STMT_WHILE: {
            Type cond_type = check_expr(c, stmt->while_stmt.condition);
            if (!is_unknown(cond_type) && cond_type.kind != TYPE_BOOL) {
                type_error(c, stmt->line,
                    "While condition must be bool, got %s",
                    type_kind_name(cond_type.kind));
            }
            c->loop_depth++;
            check_block(c, stmt->while_stmt.body);
            c->loop_depth--;
            break;
        }

        /* ── For loop ──────────────────────────────────────────────────── */
        case STMT_FOR: {
            /* The for loop introduces a new scope for its init variable.
             * e.g. `int i` in `for (int i = 0; ...)` is scoped to the loop. */
            push_scope(c);

            if (stmt->for_stmt.init)
                check_stmt(c, stmt->for_stmt.init);

            if (stmt->for_stmt.condition) {
                Type cond_type = check_expr(c, stmt->for_stmt.condition);
                if (!is_unknown(cond_type) && cond_type.kind != TYPE_BOOL) {
                    type_error(c, stmt->line,
                        "For condition must be bool, got %s",
                        type_kind_name(cond_type.kind));
                }
            }

            if (stmt->for_stmt.step)
                check_expr(c, stmt->for_stmt.step);

            c->loop_depth++;
            /* Body gets its OWN scope inside the for's scope */
            check_block(c, stmt->for_stmt.body);
            c->loop_depth--;

            pop_scope(c);
            break;
        }

        case STMT_FOREACH: {
            /* Patch elem_type: interface name parses as TYPE_OBJECT — keep it,
             * but also check for enum (same pattern as var_decl). */
            if (stmt->foreach_stmt.elem_type.kind == TYPE_OBJECT &&
                stmt->foreach_stmt.elem_type.class_name) {
                Symbol *maybe_sym = lookup_symbol(c,
                    stmt->foreach_stmt.elem_type.class_name,
                    (int)strlen(stmt->foreach_stmt.elem_type.class_name));
                if (maybe_sym && maybe_sym->kind == SYM_ENUM) {
                    stmt->foreach_stmt.elem_type = type_enum(maybe_sym->enum_name_buf);
                }
            }
            Type arr_type = check_expr(c, stmt->foreach_stmt.array);
            /* ── IEnumerable<T> path ────────────────────────────────────────
             * If the collection is a TYPE_OBJECT that implements IEnumerable<T>,
             * rewrite the STMT_FOREACH in-place into a STMT_BLOCK:
             *
             *   { IEnumerator<T> __enum = collection.getEnumerator();
             *     while (__enum.moveNext()) { T var = __enum.current(); body } }
             *
             * The rewritten block is then re-checked as STMT_BLOCK so the
             * compiler never needs to know about IEnumerable at all.
             * ──────────────────────────────────────────────────────────────── */
            if (!is_unknown(arr_type) && arr_type.kind == TYPE_OBJECT &&
                arr_type.class_name) {
                /* Walk the class's interface list for IEnumerable */
                Symbol *csym = lookup_symbol(c, arr_type.class_name,
                                             (int)strlen(arr_type.class_name));
                Type enum_elem_type = {0}; /* T from IEnumerable<T> */
                bool found_ienumerable = false;
                if (csym && csym->kind == SYM_CLASS && csym->class_decl) {
                    typedef struct IfaceNameNode IFNode;
                    for (IFNode *in = csym->class_decl->class_decl.interfaces;
                         in; in = in->next) {
                        int nlen = in->length < 11 ? in->length : 11;
                        if (nlen == 11 && strncmp(in->name, "IEnumerable", 11) == 0 &&
                            in->type_arg_count >= 1) {
                            enum_elem_type    = in->type_args[0];
                            found_ienumerable = true;
                            break;
                        }
                    }
                } else if (csym && csym->kind == SYM_CLASS && csym->class_def) {
                    /* xar-loaded class — check serialized interface_names.
                     * Format: "IEnumerable<int>", "IEnumerable<string>", etc. */
                    const ClassDef *cdef = (const ClassDef *)csym->class_def;
                    for (int ii = 0; ii < cdef->interface_count && !found_ienumerable; ii++) {
                        const char *iname = cdef->interface_names[ii];
                        if (strncmp(iname, "IEnumerable<", 12) == 0) {
                            /* Extract type arg from "IEnumerable<T>" */
                            const char *targ_start = iname + 12;
                            const char *targ_end   = strchr(targ_start, '>');
                            if (targ_end) {
                                int tlen = (int)(targ_end - targ_start);
                                /* Map primitive names to TypeKind */
                                if (tlen == 3 && strncmp(targ_start, "int", 3) == 0)
                                    enum_elem_type.kind = TYPE_INT;
                                else if (tlen == 6 && strncmp(targ_start, "string", 6) == 0)
                                    enum_elem_type.kind = TYPE_STRING;
                                else if (tlen == 5 && strncmp(targ_start, "float", 5) == 0)
                                    enum_elem_type.kind = TYPE_FLOAT;
                                else if (tlen == 4 && strncmp(targ_start, "bool", 4) == 0)
                                    enum_elem_type.kind = TYPE_BOOL;
                                else {
                                    /* Object type */
                                    char *tname = arena_alloc(c->arena, tlen + 1);
                                    memcpy(tname, targ_start, tlen);
                                    tname[tlen] = '\0';
                                    enum_elem_type.kind       = TYPE_OBJECT;
                                    enum_elem_type.class_name = tname;
                                }
                                found_ienumerable = true;
                            }
                        }
                    }
                }
                if (found_ienumerable) {
                    int ln = stmt->line;
                    Expr *coll = stmt->foreach_stmt.array;

                    /* __enum = collection.getEnumerator() */
                    Expr *get_enum = arena_alloc(c->arena, sizeof(Expr));
                    get_enum->kind = EXPR_METHOD_CALL;
                    get_enum->line = ln;
                    get_enum->method_call.object          = coll;
                    get_enum->method_call.method_name     = "getEnumerator";
                    get_enum->method_call.method_name_len = 13;
                    get_enum->method_call.args            = NULL;
                    get_enum->method_call.arg_count       = 0;
                    get_enum->method_call.resolved_params = NULL;

                    /* Infer the concrete enumerator type by type-checking
                     * the getEnumerator() call now, so __enum is typed as
                     * RangeEnumerator (not just IEnumerator), letting the
                     * checker resolve current() with the concrete return type. */
                    Type enum_type = check_expr(c, get_enum);
                    /* Fallback: if type-check failed, use IEnumerator */
                    if (enum_type.kind == 0) {
                        enum_type.kind       = TYPE_OBJECT;
                        enum_type.class_name = "IEnumerator";
                    }

                    Stmt *enum_decl = stmt_var_decl(c->arena, enum_type,
                                                    "__enum", 6, get_enum, ln, 1);

                    /* __enum.moveNext() */
                    Expr *enum_var = arena_alloc(c->arena, sizeof(Expr));
                    enum_var->kind = EXPR_IDENT;
                    enum_var->line = ln;
                    enum_var->ident.name   = "__enum";
                    enum_var->ident.length = 6;

                    Expr *move_next = arena_alloc(c->arena, sizeof(Expr));
                    move_next->kind = EXPR_METHOD_CALL;
                    move_next->line = ln;
                    move_next->method_call.object          = enum_var;
                    move_next->method_call.method_name     = "moveNext";
                    move_next->method_call.method_name_len = 8;
                    move_next->method_call.args            = NULL;
                    move_next->method_call.arg_count       = 0;
                    move_next->method_call.resolved_params = NULL;

                    /* __enum.current() */
                    Expr *enum_var2 = arena_alloc(c->arena, sizeof(Expr));
                    enum_var2->kind = EXPR_IDENT;
                    enum_var2->line = ln;
                    enum_var2->ident.name   = "__enum";
                    enum_var2->ident.length = 6;

                    Expr *current_call = arena_alloc(c->arena, sizeof(Expr));
                    current_call->kind = EXPR_METHOD_CALL;
                    current_call->line = ln;
                    current_call->method_call.object          = enum_var2;
                    current_call->method_call.method_name     = "current";
                    current_call->method_call.method_name_len = 7;
                    current_call->method_call.args            = NULL;
                    current_call->method_call.arg_count       = 0;
                    current_call->method_call.resolved_params = NULL;

                    /* T var = __enum.current() */
                    Type decl_type = stmt->foreach_stmt.elem_type;
                    /* If elem_type was unspecified (kind==0) use the inferred T */
                    if (decl_type.kind == 0) decl_type = enum_elem_type;
                    Stmt *elem_decl = stmt_var_decl(c->arena, decl_type,
                                                    stmt->foreach_stmt.var_name,
                                                    stmt->foreach_stmt.var_len,
                                                    current_call, ln, 1);

                    /* Build while body: elem_decl + original body stmts */
                    StmtNode *while_stmts = stmt_node(c->arena, elem_decl,
                                               (StmtNode*)stmt->foreach_stmt.body->block.stmts);
                    Stmt *while_body = stmt_block(c->arena, while_stmts, ln, 1);

                    Stmt *while_stmt = stmt_while(c->arena, move_next, while_body, ln, 1);

                    /* Outer block: enum_decl + while */
                    StmtNode *outer = stmt_node(c->arena, enum_decl,
                                        stmt_node(c->arena, while_stmt, NULL));
                    /* Rewrite this stmt in-place from STMT_FOREACH to STMT_BLOCK */
                    stmt->kind = STMT_BLOCK;
                    stmt->block.stmts = outer;
                    /* Re-check as STMT_BLOCK */
                    check_stmt(c, stmt);
                    break;
                }
                /* Not IEnumerable — fall through to toArray() attempt */
                /* Synthesize: stmt->foreach_stmt.array = array_expr.toArray() */
                Expr *obj_expr = stmt->foreach_stmt.array;
                Expr *to_arr   = arena_alloc(c->arena, sizeof(Expr));
                to_arr->kind = EXPR_METHOD_CALL;
                to_arr->line = stmt->line;
                to_arr->method_call.object           = obj_expr;
                to_arr->method_call.method_name      = "toArray";
                to_arr->method_call.method_name_len  = 7;
                to_arr->method_call.args             = NULL;
                to_arr->method_call.arg_count        = 0;
                to_arr->method_call.resolved_params  = NULL;
                arr_type = check_expr(c, to_arr);
                if (arr_type.kind == TYPE_ARRAY) {
                    stmt->foreach_stmt.array = to_arr;
                } else {
                    type_error(c, stmt->line,
                        "foreach requires an array or IEnumerable<T> collection, got %s",
                        type_kind_name(arr_type.kind));
                    break;
                }
            } else if (!is_unknown(arr_type) && arr_type.kind != TYPE_ARRAY) {
                type_error(c, stmt->line, "foreach requires an array, got %s",
                           type_kind_name(arr_type.kind));
                break;
            }
            if (arr_type.kind == TYPE_ARRAY && arr_type.element_type) {
                Type decl = stmt->foreach_stmt.elem_type;
                Type elem = *arr_type.element_type;
                /* Check if decl is an interface type */
                bool is_iface_decl = false;
                if (decl.kind == TYPE_OBJECT && decl.class_name) {
                    Symbol *dsym = lookup_symbol(c, decl.class_name, (int)strlen(decl.class_name));
                    is_iface_decl = (dsym && dsym->kind == SYM_INTERFACE);
                }
                bool ok = is_unknown(elem) ||
                    types_assignable(decl, elem) ||
                    (decl.kind == TYPE_OBJECT && elem.kind == TYPE_OBJECT &&
                     decl.class_name && elem.class_name &&
                     is_subtype(c, elem.class_name, decl.class_name)) ||
                    (is_iface_decl && decl.class_name &&
                     type_is_assignable_to_interface(c, elem, decl.class_name));
                if (!ok) {
                    type_error(c, stmt->line, "foreach element type %s does not match array element type %s",
                        type_kind_name(decl.kind), type_kind_name(elem.kind)); break;
                }
            }
            push_scope(c);
            Symbol var_sym = {0};
            var_sym.kind = SYM_VAR; var_sym.name = stmt->foreach_stmt.var_name;
            var_sym.length = stmt->foreach_stmt.var_len; var_sym.type = stmt->foreach_stmt.elem_type;
            sym_set_loc(&var_sym, c->source_file, stmt->line, stmt->col);
            define_symbol(c, var_sym);
            c->loop_depth++; check_block(c, stmt->foreach_stmt.body); c->loop_depth--;
            pop_scope(c); break;
        }

        /* ── Return ────────────────────────────────────────────────────── */
        case STMT_RETURN: {
            if (!c->inside_function) {
                type_error(c, stmt->line,
                    "Return statement outside of function");
                break;
            }

            Type expected = c->current_fn_return_type;

            if (stmt->return_stmt.value) {
                /* Return with a value */
                if (expected.kind == TYPE_VOID) {
                    type_error(c, stmt->line,
                        "Void function cannot return a value");
                    break;
                }
                Type actual = check_expr(c, stmt->return_stmt.value);
                if (!is_unknown(actual) && !types_assignable(expected, actual)
                    && actual.kind != TYPE_ANY) {
                    /* Also allow subtype for object returns */
                    bool subtype_ok = (expected.kind == TYPE_OBJECT && actual.kind == TYPE_OBJECT
                                       && expected.class_name && actual.class_name
                                       && is_subtype(c, actual.class_name, expected.class_name));
                    /* Also allow returning a class that implements the declared interface
                     * (handles generic interfaces like IEnumerator<int>) */
                    if (!subtype_ok && expected.kind == TYPE_OBJECT && actual.kind == TYPE_OBJECT
                        && expected.class_name && actual.class_name) {
                        char base[64];
                        const char *iface_name = expected.class_name;
                        if (generic_base_name(expected.class_name, base, sizeof(base)))
                            iface_name = base;
                        Symbol *isym = lookup_symbol(c, iface_name, (int)strlen(iface_name));
                        if (isym && isym->kind == SYM_INTERFACE)
                            subtype_ok = type_is_assignable_to_interface(c, actual, iface_name);
                    }
                    if (!subtype_ok) {
                        type_error(c, stmt->line,
                            "Return type mismatch: function returns %s, got %s",
                            type_kind_name(expected.kind),
                            type_kind_name(actual.kind));
                    }
                }
            } else {
                /* Bare return — only valid in void functions */
                if (expected.kind != TYPE_VOID) {
                    type_error(c, stmt->line,
                        "Non-void function must return a %s value",
                        type_kind_name(expected.kind));
                }
            }
            break;
        }

        /* ── Break / Continue ──────────────────────────────────────────── */
        case STMT_BREAK:
            /* break is valid inside a loop OR a match arm */
            if (c->loop_depth == 0 && c->match_depth == 0) {
                type_error(c, stmt->line, "'break' outside of loop or match");
            }
            break;
        case STMT_CONTINUE:
            if (c->loop_depth == 0) {
                type_error(c, stmt->line, "'continue' outside of loop");
            }
            break;

        /* ── throw statement ─────────────────────────────────────────────── */
        case STMT_THROW: {
            Type thrown = check_expr(c, stmt->throw_stmt.value);
            /* Must be an object (Exception or subclass).
             * We accept TYPE_ANY for dep-loaded classes. */
            if (thrown.kind != TYPE_OBJECT && thrown.kind != TYPE_ANY
                    && !is_unknown(thrown)) {
                type_error(c, stmt->line,
                    "throw requires an object type, got %s",
                    type_kind_name(thrown.kind));
            }
            /* Optionally verify it descends from Exception */
            if (thrown.kind == TYPE_OBJECT && thrown.class_name) {
                if (!is_subtype(c, thrown.class_name, "Exception")) {
                    type_error(c, stmt->line,
                        "thrown type '%s' does not extend Exception",
                        thrown.class_name);
                }
            }
            break;
        }

        /* ── try/catch/finally statement ────────────────────────────────── */
        case STMT_TRY: {
            /* Check try body */
            check_stmt(c, stmt->try_stmt.body);

            /* Check each catch clause */
            for (int _ci = 0; _ci < stmt->try_stmt.catch_count; _ci++) {
                const char *ctype    = stmt->try_stmt.catch_types    [_ci];
                int         ctype_len= stmt->try_stmt.catch_type_lens[_ci];
                const char *cvar     = stmt->try_stmt.catch_vars     [_ci];
                int         cvar_len = stmt->try_stmt.catch_var_lens [_ci];
                Stmt       *cbody    = stmt->try_stmt.catch_bodies    [_ci];

                /* Verify catch type exists and is an Exception subclass */
                char ctype_buf[128];
                int  cn = ctype_len < 127 ? ctype_len : 127;
                memcpy(ctype_buf, ctype, cn); ctype_buf[cn] = '\0';

                Symbol *cls_sym = lookup_symbol(c, ctype_buf, cn);
                if (!cls_sym && strcmp(ctype_buf, "Exception") != 0) {
                    type_error(c, stmt->line,
                        "Unknown exception type '%s' in catch clause", ctype_buf);
                } else if (cls_sym && cls_sym->kind == SYM_CLASS) {
                    if (!is_subtype(c, ctype_buf, "Exception")) {
                        type_error(c, stmt->line,
                            "Catch type '%s' does not extend Exception", ctype_buf);
                    }
                }

                /* Introduce the catch variable into scope for the catch body.
                 * We push a scope, declare the var as TYPE_OBJECT(ctype), check body. */
                push_scope(c);

                /* Allocate a null-terminated copy of the type name on the arena */
                char *ctype_perm = arena_alloc(c->arena, cn + 1);
                if (ctype_perm) {
                    memcpy(ctype_perm, ctype_buf, cn);
                    ctype_perm[cn] = '\0';
                } else {
                    ctype_perm = "Exception"; /* fallback */
                }
                Type ex_type = type_object(ctype_perm);

                char cvar_buf[128];
                int  vn = cvar_len < 127 ? cvar_len : 127;
                memcpy(cvar_buf, cvar, vn); cvar_buf[vn] = '\0';
                Symbol _catch_sym = {0};
                _catch_sym.kind   = SYM_VAR;
                _catch_sym.name   = stmt->try_stmt.catch_vars[_ci];
                _catch_sym.length = vn;
                _catch_sym.type   = ex_type;
                sym_set_loc(&_catch_sym, c->source_file, stmt->line, 0);
                define_symbol(c, _catch_sym);
                check_stmt(c, cbody);
                pop_scope(c);
            }

            /* Check finally body */
            if (stmt->try_stmt.finally_body) {
                check_stmt(c, stmt->try_stmt.finally_body);
            }
            break;
        }

        /* ── Match statement ────────────────────────────────────────────── */
        case STMT_MATCH: {
            typedef struct MatchArmNode MANode;

            Type subject_type = check_expr(c, stmt->match_stmt.subject);
            bool subject_is_enum = (subject_type.kind == TYPE_ENUM);
            bool subject_is_int  = (subject_type.kind == TYPE_INT);
            bool subject_is_bool = (subject_type.kind == TYPE_BOOL);

            if (!subject_is_enum && !subject_is_int && !subject_is_bool) {
                type_error(c, stmt->line,
                    "match subject must be an enum, int, or bool, got %s",
                    type_kind_name(subject_type.kind));
                break;
            }

            /* ── Type-check each arm (this also resolves patterns via check_expr) ── */
            c->match_depth++;
            for (MANode *arm = stmt->match_stmt.arms; arm; arm = arm->next) {
                if (!arm->is_default && arm->pattern) {
                    Type pt = check_expr(c, arm->pattern);
                    if (!is_unknown(pt) && !type_equals(pt, subject_type)) {
                        type_error(c, arm->pattern->line,
                            "case pattern type %s does not match match subject type %s",
                            type_kind_name(pt.kind),
                            type_kind_name(subject_type.kind));
                    }
                }
                if (arm->body) check_stmt(c, arm->body);
            }
            c->match_depth--;

            /* ── Exhaustiveness check for enums ──────────────────────────
             * Runs AFTER arm type-checking so patterns are resolved to
             * EXPR_ENUM_ACCESS (via the EXPR_FIELD_GET rewrite in check_expr).
             * If subject is a known enum and there is no 'default', warn if
             * not all members are covered. */
            if (subject_is_enum && !stmt->match_stmt.has_default &&
                subject_type.enum_name) {
                Symbol *esym = lookup_symbol(c, subject_type.enum_name,
                                             (int)strlen(subject_type.enum_name));
                if (esym && esym->kind == SYM_ENUM && esym->enum_decl) {
                    typedef struct EnumMemberNode EMNode;
                    int covered = 0, total = 0;
                    for (EMNode *em = esym->enum_decl->enum_decl.members;
                         em; em = em->next) {
                        total++;
                        for (MANode *arm = stmt->match_stmt.arms; arm; arm = arm->next) {
                            if (arm->is_default || !arm->pattern) continue;
                            if (arm->pattern->kind == EXPR_ENUM_ACCESS &&
                                arm->pattern->enum_access.value == em->value) {
                                covered++;
                                break;
                            }
                        }
                    }
                    if (covered < total) {
                        if (c->error_count < CHECKER_MAX_ERRORS) {
                            CheckError *e = &c->errors[c->error_count++];
                            snprintf(e->message, sizeof(e->message),
                                "match on enum '%s' does not cover all members "
                                "(%d of %d covered) — consider adding a 'default' arm",
                                subject_type.enum_name, covered, total);
                            e->line       = stmt->line;
                            e->is_warning = true;
                        }
                    }
                }
            }
            break;
        }

        /* ── Function declaration inside a block (nested functions) ────── */
        case STMT_FN_DECL:
            /* We forbid nested function declarations for simplicity.
             * All functions must be at the top level.
             * This keeps scoping rules simple and the VM call model clean. */
            type_error(c, stmt->line,
                "Function '%.*s' must be declared at the top level",
                stmt->fn_decl.length, stmt->fn_decl.name);
            break;

        case STMT_CLASS_DECL:
            type_error(c, stmt->line,
                "Class '%.*s' must be declared at the top level",
                stmt->class_decl.length, stmt->class_decl.name);
            break;

        case STMT_ENUM_DECL:
            type_error(c, stmt->line,
                "Enum '%.*s' must be declared at the top level",
                stmt->enum_decl.length, stmt->enum_decl.name);
            break;

        case STMT_INTERFACE_DECL:
            type_error(c, stmt->line,
                "Interface '%.*s' must be declared at the top level",
                stmt->interface_decl.length, stmt->interface_decl.name);
            break;

        case STMT_IMPORT:
            /* Import statements are fully resolved by xenoc_main before the
             * checker runs — the imported source is merged into the program.
             * If a STMT_IMPORT node survives to check_stmt it means it appeared
             * inside a function body, which is invalid. */
            type_error(c, stmt->line,
                "Import declarations must appear at the top level");
            break;

        case STMT_EVENT_DECL:
            /* Top-level event declarations are registered in Pass 1.
             * Nothing further to check at the statement level. */
            break;
    }
}

/*
 * check_fn_body — type-check a single function's body.
 * Sets up the function scope (parameters) and checks all statements.
 */
static void check_fn_body(Checker *c, Stmt *fn_stmt) {
    /* Set the current function context */
    c->inside_function        = true;
    c->current_fn_return_type = fn_stmt->fn_decl.return_type;
    c->current_fn_stmt        = fn_stmt;

    /* Each function body gets its own scope for parameters */
    push_scope(c);

    /* ── Generic type params: push T, K, V etc. as TYPE_PARAM symbols ──
     * This allows the checker to recognise them as types rather than
     * unknown class names inside the function body. */
    for (TypeParamNode *tp = fn_stmt->fn_decl.type_params; tp; tp = tp->next) {
        Symbol tpsym = {0};
        tpsym.kind   = SYM_VAR;      /* We use SYM_VAR so lookup works */
        tpsym.name   = tp->name;
        tpsym.length = tp->length;
        tpsym.type   = type_param(tp->name);  /* TYPE_PARAM kind */
        sym_set_loc(&tpsym, c->source_file, fn_stmt->line, 0);
        define_symbol(c, tpsym);
    }

    /* Each function body gets its own scope for parameters */
    push_scope(c);

    /* If we're inside a class, put 'this' in scope as slot 0.
     * 'this' is always the first implicit parameter for methods/constructors. */
    if (c->current_class) {
        /* Look up the class symbol to get its null-terminated name */
        Symbol *cls_sym = lookup_symbol(c,
            c->current_class->class_decl.name,
            c->current_class->class_decl.length);
        const char *cls_name = (cls_sym && cls_sym->kind == SYM_CLASS)
                             ? cls_sym->class_name_buf
                             : c->current_class->class_decl.name; /* fallback */

        Symbol this_sym = {0};
        this_sym.kind   = SYM_VAR;
        this_sym.name   = "this";
        this_sym.length = 4;
        this_sym.type   = type_object(cls_name);
        sym_set_loc(&this_sym, c->source_file, fn_stmt->line, 0);
        define_symbol(c, this_sym);
    }

    /* Define parameters as variables in the function's scope */
    for (ParamNode *p = fn_stmt->fn_decl.params; p; p = p->next) {
        Symbol sym = {0};
        sym.kind   = SYM_VAR;
        sym.name   = p->name;
        sym.length = p->length;
        sym.type   = p->type;

        /* If the declared type is TYPE_OBJECT but resolves to a known enum,
         * upgrade the type to TYPE_ENUM so body type-checks work correctly. */
        if (sym.type.kind == TYPE_OBJECT && sym.type.class_name) {
            Symbol *maybe_enum = lookup_symbol(c,
                sym.type.class_name, (int)strlen(sym.type.class_name));
            if (maybe_enum && maybe_enum->kind == SYM_ENUM) {
                sym.type    = type_enum(maybe_enum->enum_name_buf);
                p->type     = sym.type;   /* patch AST too so return-type checks work */
            }
        }

        sym_set_loc(&sym, c->source_file, fn_stmt->line, 0);
        if (!define_symbol(c, sym)) {
            type_error(c, fn_stmt->line,
                "Duplicate parameter name '%.*s'",
                p->length, p->name);
        }
    }

    /* Check the body — note: the body is a STMT_BLOCK but we DON'T call
     * check_block() because that would push ANOTHER scope on top of the
     * parameter scope. Instead we directly iterate the block's statements
     * inside the same parameter scope. */
    for (StmtNode *n = fn_stmt->fn_decl.body->block.stmts; n; n = n->next) {
        check_stmt(c, n->stmt);
    }

    pop_scope(c);  /* param scope */
    pop_scope(c);  /* type param scope */
    c->inside_function = false;
    c->current_fn_stmt = NULL;
}

/* ─────────────────────────────────────────────────────────────────────────────
 * PUBLIC INTERFACE
 * ───────────────────────────────────────────────────────────────────────────*/

void checker_init(Checker *c, Arena *arena) {
    memset(c, 0, sizeof(Checker));
    c->arena = arena;
    /* Global scope (depth 0) starts with count = 0 — already zeroed */
}

void checker_declare_host(Checker *c,
                          const char *name,
                          Type return_type,
                          Type *param_types,
                          int param_count) {
    Symbol sym = {0};
    sym.kind        = SYM_FN;
    sym.name        = name;
    sym.length      = (int)strlen(name);
    sym.type        = return_type;
    sym.param_count = param_count < MAX_PARAMS ? param_count : MAX_PARAMS;
    for (int i = 0; i < sym.param_count; i++)
        sym.param_types[i] = param_types[i];
    sym_set_loc(&sym, NULL, 0, 0);
    bool ok = define_symbol(c, sym);
    (void)ok;
}

void checker_declare_class_from_def(Checker *c, const ClassDef *def) {
    /* Skip if already declared (e.g. core loaded twice) */
    int nlen = (int)strlen(def->name);
    if (lookup_symbol(c, def->name, nlen)) return;

    Symbol sym = {0};
    sym.kind   = SYM_CLASS;
    sym.length = nlen;
    /* Copy name into the stable buf FIRST so sym.name doesn't dangle
     * if the staging module is freed before the checker uses the symbol. */
    int blen   = nlen < 63 ? nlen : 63;
    memcpy(sym.class_name_buf, def->name, blen);
    sym.class_name_buf[blen] = '\0';
    sym.name   = sym.class_name_buf;   /* point at stable copy, not staging data */
    sym.type   = type_object(sym.class_name_buf);

    sym_set_loc(&sym, NULL, 0, 0); /* XAR-loaded — no source location */
    if (define_symbol(c, sym)) {
        Symbol *stored = lookup_symbol(c, sym.class_name_buf, nlen);
        if (stored) {
            stored->name = stored->class_name_buf; /* fix dangling pointer after copy */
            stored->type.class_name = stored->class_name_buf;
            stored->class_def = (void *)def;
        }
    }
}

bool checker_check(Checker *c, Program *program) {

    /* ── PASS 1: Register all top-level declarations ───────────────────────
     *
     * Walk top-level statements registering:
     *   - STMT_FN_DECL    → SYM_FN  in global scope
     *   - STMT_CLASS_DECL → SYM_CLASS in global scope
     *
     * This makes forward references work: a function can call another
     * declared later, and code can instantiate a class declared later.
     * ────────────────────────────────────────────────────────────────────*/
    for (StmtNode *n = program->stmts; n; n = n->next) {
        Stmt *s = n->stmt;

        if (s->kind == STMT_FN_DECL) {
            type_error(c, s->line, "Top-level functions are not allowed in XenoScript");
        } else if (s->kind == STMT_EVENT_DECL) {
            type_error(c, s->line, "Top-level events are not allowed in XenoScript");
        }

        if (s->kind == STMT_IMPORT) continue; /* resolved before checker */

        if (s->kind == STMT_FN_DECL) {
            Symbol sym = {0};
            sym.kind          = SYM_FN;
            sym.name          = s->fn_decl.name;
            sym.length        = s->fn_decl.length;
            sym.fn_decl_node  = s;

            /* Patch return type and param types: if a type is TYPE_OBJECT
             * and its class_name matches a declared type parameter, convert
             * it to TYPE_PARAM so substitute_type works at call sites. */
            TypeParamNode *tp_list = s->fn_decl.type_params;

            /* Helper: given a TYPE_OBJECT whose class_name matches a type param,
             * return a TYPE_PARAM with a stable arena-allocated name. */
#define PATCH_TO_PARAM(t_) do { \
    if ((t_).kind == TYPE_OBJECT && (t_).class_name && tp_list) { \
        for (TypeParamNode *_tp = tp_list; _tp; _tp = _tp->next) { \
            int _tl = _tp->length < 63 ? _tp->length : 63; \
            if (strncmp((t_).class_name, _tp->name, _tl) == 0 && \
                (t_).class_name[_tl] == '\0') { \
                char *_stab = arena_alloc(c->arena, _tl + 1); \
                memcpy(_stab, _tp->name, _tl); _stab[_tl] = '\0'; \
                (t_) = type_param(_stab); break; \
            } \
        } \
    } \
} while(0)

            Type ret = s->fn_decl.return_type;
            PATCH_TO_PARAM(ret);
            sym.type        = ret;
            sym.param_count = s->fn_decl.param_count;

            int i = 0;
            for (ParamNode *p = s->fn_decl.params; p && i < MAX_PARAMS; p = p->next) {
                Type pt = p->type;
                PATCH_TO_PARAM(pt);
                sym.param_types[i++] = pt;
            }
#undef PATCH_TO_PARAM
            sym_set_loc(&sym, c->source_file, s->line, s->col);

            if (!define_symbol(c, sym))
                type_error(c, s->line, "Function '%.*s' already declared",
                           s->fn_decl.length, s->fn_decl.name);
        }
        else if (s->kind == STMT_CLASS_DECL) {
            Symbol sym = {0};
            sym.kind       = SYM_CLASS;
            sym.name       = s->class_decl.name;
            sym.length     = s->class_decl.length;
            sym.class_decl = s;

            /* Build null-terminated name for strlen-safe usage */
            int nlen = s->class_decl.length < 63 ? s->class_decl.length : 63;
            memcpy(sym.class_name_buf, s->class_decl.name, nlen);
            sym.class_name_buf[nlen] = '\0';
            sym.type = type_object(sym.class_name_buf);
            sym_set_loc(&sym, c->source_file, s->line, s->col);

            if (!define_symbol(c, sym)) {
                /* Class was pre-seeded from staging (stdlib/dep ClassDef).
                 * Now that we have the AST (inlined source), update class_decl
                 * so generic type params and field/method lookup work correctly. */
                Symbol *existing = lookup_symbol(c, s->class_decl.name, s->class_decl.length);
                if (existing && existing->kind == SYM_CLASS && !existing->class_decl)
                    existing->class_decl = s;
            } else {
                Symbol *stored = lookup_symbol(c, s->class_decl.name, s->class_decl.length);
                if (stored) stored->type.class_name = stored->class_name_buf;
            }
        }
        else if (s->kind == STMT_ENUM_DECL) {
            Symbol sym = {0};
            sym.kind      = SYM_ENUM;
            sym.name      = s->enum_decl.name;
            sym.length    = s->enum_decl.length;
            sym.enum_decl = s;

            int nlen = s->enum_decl.length < 63 ? s->enum_decl.length : 63;
            memcpy(sym.enum_name_buf, s->enum_decl.name, nlen);
            sym.enum_name_buf[nlen] = '\0';
            sym.type = type_enum(sym.enum_name_buf);
            sym_set_loc(&sym, c->source_file, s->line, s->col);

            if (!define_symbol(c, sym)) {
                type_error(c, s->line, "Enum '%.*s' already declared",
                           s->enum_decl.length, s->enum_decl.name);
            } else {
                /* Fix type.enum_name to point at the stored symbol's buffer,
                 * not the local stack copy (which goes out of scope). */
                Symbol *stored = lookup_symbol(c, s->enum_decl.name, s->enum_decl.length);
                if (stored) stored->type.enum_name = stored->enum_name_buf;
            }
        }
        else if (s->kind == STMT_INTERFACE_DECL) {
            Symbol sym = {0};
            sym.kind           = SYM_INTERFACE;
            sym.name           = s->interface_decl.name;
            sym.length         = s->interface_decl.length;
            sym.interface_decl = s;

            int nlen = s->interface_decl.length < 63 ? s->interface_decl.length : 63;
            memcpy(sym.iface_name_buf, s->interface_decl.name, nlen);
            sym.iface_name_buf[nlen] = '\0';
            sym_set_loc(&sym, c->source_file, s->line, s->col);

            if (!define_symbol(c, sym)) {
                type_error(c, s->line, "Interface '%.*s' already declared",
                           s->interface_decl.length, s->interface_decl.name);
            }
        }
        else if (s->kind == STMT_EVENT_DECL) {
            Symbol sym = {0};
            sym.kind            = SYM_EVENT;
            sym.name            = s->event_decl.name;
            sym.length          = s->event_decl.length;
            sym.event_decl_node = s;
            /* Store param types for subscriber signature checking */
            sym.param_count = s->event_decl.param_count;
            ParamNode *p2 = s->event_decl.params;
            for (int pi = 0; pi < sym.param_count && p2; pi++, p2 = p2->next) {
                sym.param_types[pi] = p2->type;
            }
            sym_set_loc(&sym, c->source_file, s->line, s->col);
            if (!define_symbol(c, sym)) {
                type_error(c, s->line, "Event '%.*s' already declared",
                           s->event_decl.length, s->event_decl.name);
            }
        }
    }

    /* ── PASS 1b: Resolve parent class and implemented interfaces ───────── */
    for (StmtNode *n = program->stmts; n; n = n->next) {
        Stmt *s = n->stmt;

        if (s->kind == STMT_IMPORT) continue; /* resolved before checker */
        if (s->kind != STMT_CLASS_DECL) continue;
        if (!s->class_decl.interfaces) continue;

        typedef struct IfaceNameNode IFNode;
        int parent_count = 0;

        for (IFNode *in = s->class_decl.interfaces; in; in = in->next) {
            /* If this is a generic parent (class Foo : Bar<int>), look up
             * the base name "Bar" rather than the literal token "Bar<int>". */
            const char *lookup_name = in->name;
            int         lookup_len  = in->length;
            char        base_buf[64];
            if (in->type_arg_count > 0) {
                /* name is the bare name already (parsed as IDENT before '<') */
                int bl = in->length < 63 ? in->length : 63;
                memcpy(base_buf, in->name, bl);
                base_buf[bl] = '\0';
                lookup_name = base_buf;
                lookup_len  = bl;
            }
            Symbol *sym = lookup_symbol(c, lookup_name, lookup_len);
            if (!sym) {
                /* During stdlib bootstrap, parent classes may not be in scope yet.
                 * Only error if we have a full stdlib loaded (Attribute in scope),
                 * to avoid false failures when packing core.xar. */
                if (lookup_symbol(c, "Attribute", 9)) {
                    type_error(c, s->line,
                        "Class '%.*s': '%.*s' is not declared",
                        s->class_decl.length, s->class_decl.name,
                        in->length, in->name);
                }
                continue;
            }
            if (sym->kind == SYM_CLASS) {
                parent_count++;
                if (parent_count > 1) {
                    type_error(c, s->line,
                        "Class '%.*s': cannot inherit from more than one class",
                        s->class_decl.length, s->class_decl.name);
                } else {
                    s->class_decl.parent_name   = in->name;
                    s->class_decl.parent_length = in->length;
                    /* Store concrete type args for generic parent inheritance */
                    if (in->type_arg_count > 0) {
                        /* Copy type args into arena for stable lifetime */
                        Type *targs = arena_alloc(c->arena,
                            in->type_arg_count * sizeof(Type));
                        if (targs) {
                            memcpy(targs, in->type_args,
                                   in->type_arg_count * sizeof(Type));
                            s->class_decl.parent_type_args      = targs;
                            s->class_decl.parent_type_arg_count = in->type_arg_count;
                        }
                }
                    }
            } else if (sym->kind == SYM_INTERFACE) {
                /* Accepted */
            } else {
                type_error(c, s->line,
                    "Class '%.*s': '%.*s' is neither a class nor an interface",
                    s->class_decl.length, s->class_decl.name,
                    in->length, in->name);
            }
        }
    }

    /* ── PASS 1b_iface: Validate interface parent names ─────────────────── */
    for (StmtNode *n = program->stmts; n; n = n->next) {
        Stmt *s = n->stmt;

        if (s->kind == STMT_IMPORT) continue; /* resolved before checker */
        if (s->kind != STMT_INTERFACE_DECL) continue;
        if (!s->interface_decl.parent_name || s->interface_decl.parent_length == 0) continue;
        Symbol *psym = lookup_symbol(c,
            s->interface_decl.parent_name, s->interface_decl.parent_length);
        if (!psym || psym->kind != SYM_INTERFACE) {
            type_error(c, s->line,
                "Interface '%.*s': parent '%.*s' is not a declared interface",
                s->interface_decl.length, s->interface_decl.name,
                s->interface_decl.parent_length, s->interface_decl.parent_name);
        }
    }

    /* ── PASS 1b2: Interface compliance check ─────────────────────────────
     * For every class that lists interfaces, verify all required methods
     * are implemented (with matching signatures).
     * We collect required methods from the FULL interface hierarchy
     * (interface + all its parent interfaces). */
    for (StmtNode *n = program->stmts; n; n = n->next) {
        Stmt *s = n->stmt;

        if (s->kind == STMT_IMPORT) continue; /* resolved before checker */
        if (s->kind != STMT_CLASS_DECL) continue;

        typedef struct IfaceNameNode  IFNode;
        typedef struct IfaceMethodNode IMNode;
        typedef struct ClassMethodNode CMNode;

        for (IFNode *in = s->class_decl.interfaces; in; in = in->next) {
            Symbol *isym = lookup_symbol(c, in->name, in->length);
            if (!isym || isym->kind != SYM_INTERFACE) continue;

            /* Walk the interface hierarchy collecting every required method */
            char iface_walk[64];
            int  iwlen = in->length < 63 ? in->length : 63;
            memcpy(iface_walk, in->name, iwlen);
            iface_walk[iwlen] = '\0';
            int iface_safety = 32;

            while (iface_walk[0] && iface_safety-- > 0) {
                Symbol *cur_isym = lookup_symbol(c, iface_walk, (int)strlen(iface_walk));
                if (!cur_isym || cur_isym->kind != SYM_INTERFACE || !cur_isym->interface_decl)
                    break;
                Stmt *iface = cur_isym->interface_decl;

                for (IMNode *req = iface->interface_decl.methods; req; req = req->next) {
                    bool found = false;
                    char search_buf[64];
                    int  slen = s->class_decl.length < 63 ? s->class_decl.length : 63;
                    memcpy(search_buf, s->class_decl.name, slen);
                    search_buf[slen] = '\0';

                    /* Build substituted return/param types for generic interfaces.
                     * e.g. IEnumerator<int>: T -> int in current(): T. */
                    TypeParamNode *iface_tps = iface->interface_decl.type_params;
                    int            iface_tpc = iface->interface_decl.type_param_count;
                    Type req_ret = substitute_type(req->return_type,
                                                   iface_tps, in->type_args, iface_tpc);

                    /* Walk class + class ancestors for implementation */
                    while (!found) {
                        Symbol *csym = lookup_symbol(c, search_buf, (int)strlen(search_buf));
                        if (!csym || csym->kind != SYM_CLASS || !csym->class_decl) break;
                        Stmt *cls = csym->class_decl;

                        for (CMNode *m = cls->class_decl.methods; m; m = m->next) {
                            if (m->is_static || m->is_constructor) continue;
                            Stmt *fn = m->fn;
                            if (fn->fn_decl.length != req->length) continue;
                            if (memcmp(fn->fn_decl.name, req->name, req->length) != 0) continue;
                            /* Name matches — check return type (with substitution) */
                            /* Check return type with substitution.
                             * For generic interface return types like IEnumerator<T>
                             * substituted to IEnumerator<int>, compare base names
                             * when both sides are TYPE_OBJECT generic instantiations
                             * of the same interface — the type args are validated
                             * when the value is actually used. */
                            bool ret_ok = type_equals(fn->fn_decl.return_type, req_ret);
                            if (!ret_ok &&
                                fn->fn_decl.return_type.kind == TYPE_OBJECT &&
                                req_ret.kind == TYPE_OBJECT &&
                                fn->fn_decl.return_type.class_name &&
                                req_ret.class_name) {
                                /* Extract base names and compare */
                                char fn_base[64], req_base[64];
                                bool fn_g  = generic_base_name(fn->fn_decl.return_type.class_name,
                                                               fn_base, sizeof(fn_base));
                                bool req_g = generic_base_name(req_ret.class_name,
                                                               req_base, sizeof(req_base));
                                const char *fn_cmp  = fn_g  ? fn_base  : fn->fn_decl.return_type.class_name;
                                const char *req_cmp = req_g ? req_base : req_ret.class_name;
                                /* Also handle req_ret still being TYPE_OBJECT "IEnumerator<T>" —
                                 * strip type params from req_ret class_name too */
                                char req_raw_base[64];
                                bool raw_g = generic_base_name(req_ret.class_name, req_raw_base, sizeof(req_raw_base));
                                if (raw_g) req_cmp = req_raw_base;
                                Symbol *fn_sym  = lookup_symbol(c, fn_cmp,  (int)strlen(fn_cmp));
                                Symbol *req_sym = lookup_symbol(c, req_cmp, (int)strlen(req_cmp));
                                /* Pass if both resolve to the same interface */
                                if (fn_sym && req_sym && fn_sym == req_sym &&
                                    fn_sym->kind == SYM_INTERFACE)
                                    ret_ok = true;
                            }
                            if (!ret_ok) {
                                type_error(c, s->line,
                                    "Class '%.*s' implements '%.*s': method '%.*s' "
                                    "has wrong return type (expected %s, got %s)",
                                    s->class_decl.length, s->class_decl.name,
                                    in->length, in->name,
                                    req->length, req->name,
                                    type_kind_name(req_ret.kind),
                                    type_kind_name(fn->fn_decl.return_type.kind));
                            }
                            if (fn->fn_decl.param_count != req->param_count) {
                                type_error(c, s->line,
                                    "Class '%.*s' implements '%.*s': method '%.*s' "
                                    "has wrong parameter count (expected %d, got %d)",
                                    s->class_decl.length, s->class_decl.name,
                                    in->length, in->name,
                                    req->length, req->name,
                                    req->param_count, fn->fn_decl.param_count);
                            } else {
                                ParamNode *cp = fn->fn_decl.params;
                                ParamNode *rp = req->params;
                                int pidx = 0;
                                while (cp && rp) {
                                    Type req_pt = substitute_type(rp->type,
                                                      iface_tps, in->type_args, iface_tpc);
                                    if (!type_equals(cp->type, req_pt)) {
                                        type_error(c, s->line,
                                            "Class '%.*s' implements '%.*s': "
                                            "method '%.*s' parameter %d type mismatch "
                                            "(expected %s, got %s)",
                                            s->class_decl.length, s->class_decl.name,
                                            in->length, in->name,
                                            req->length, req->name, pidx + 1,
                                            type_kind_name(req_pt.kind),
                                            type_kind_name(cp->type.kind));
                                    }
                                    cp = cp->next; rp = rp->next; pidx++;
                                }
                            }
                            found = true;
                            break;
                        }
                        if (found) break;
                        if (!cls->class_decl.parent_name || cls->class_decl.parent_length == 0)
                            break;
                        int plen = cls->class_decl.parent_length < 63
                                 ? cls->class_decl.parent_length : 63;
                        memcpy(search_buf, cls->class_decl.parent_name, plen);
                        search_buf[plen] = '\0';
                    }

                    if (!found) {
                        type_error(c, s->line,
                            "Class '%.*s' claims to implement '%.*s' but is missing "
                            "method '%.*s'",
                            s->class_decl.length, s->class_decl.name,
                            in->length, in->name,
                            req->length, req->name);
                    }
                }

                /* Advance to parent interface */
                if (!iface->interface_decl.parent_name || iface->interface_decl.parent_length == 0)
                    break;
                int plen = iface->interface_decl.parent_length < 63
                         ? iface->interface_decl.parent_length : 63;
                memcpy(iface_walk, iface->interface_decl.parent_name, plen);
                iface_walk[plen] = '\0';
            }
        }
    }
    /* ── PASS 1c: Validate annotations ──────────────────────────────────── */

    /* Target enum ordinals — must match Target enum declaration order in stdlib:
     *   enum Target { Class, Method, Field, Constructor, Enum }  */
    enum { TARGET_CLASS = 0, TARGET_METHOD = 1, TARGET_FIELD = 2,
           TARGET_CONSTRUCTOR = 3, TARGET_ENUM = 4 };

    /* check_attribute_usage — looks up the annotation class, finds its
     * @AttributeUsage annotation, and verifies that `placement` is one of the
     * declared targets.  Emits a type_error if not.
     * Returns false if the annotation class cannot be found (already reported). */
#define CHECK_ATTR_USAGE(ann_, placement_, line_, subject_fmt_, ...)            \
    do {                                                                         \
        AnnotationNode *_ann = (ann_);                                           \
        int _placement = (placement_);                                           \
        Symbol *_asym = lookup_symbol(c, _ann->name, _ann->name_len);           \
        if (!_asym || _asym->kind != SYM_CLASS) {                               \
            /* Unknown annotation — only an error if we have stdlib loaded.     \
             * During bootstrap (no stdlib), silently skip. */                  \
            Symbol *_attr_base = lookup_symbol(c, "Attribute", 9);              \
            if (_attr_base) {                                                    \
                type_error(c, (line_),                                           \
                    "Unknown annotation '@%.*s' on " subject_fmt_               \
                    " (no matching class found)",                                \
                    _ann->name_len, _ann->name, ##__VA_ARGS__);                 \
            }                                                                    \
        } else if (_asym->class_decl) {                                         \
                /* Walk the annotation class's own annotations looking for      \
                 * @AttributeUsage.  If found, read its targets[] arg and       \
                 * verify our placement is listed. */                            \
                Stmt *_acls = _asym->class_decl;                                \
                bool _found_usage = false;                                       \
                for (AnnotationNode *_ua = _acls->class_decl.annotations;       \
                     _ua; _ua = _ua->next) {                                     \
                    bool _is_au = (_ua->name_len == 14 &&                       \
                        memcmp(_ua->name, "AttributeUsage", 14) == 0);          \
                    if (!_is_au) continue;                                       \
                    _found_usage = true;                                          \
                    /* First arg is the Target[] array */                        \
                    AnnotationKVNode *_kv = _ua->args;                           \
                    if (!_kv || !_kv->value ||                                   \
                        _kv->value->kind != EXPR_ARRAY_LIT) break;              \
                    /* Empty array [] = unrestricted (bootstrap/fallback) */     \
                    if (!_kv->value->array_lit.elements) break;                  \
                    /* If any element is unresolved (not EXPR_ENUM_ACCESS),      \
                     * the Target enum wasn't in scope — treat as unrestricted   \
                     * (happens during bootstrap of attribute_usage.xeno). */    \
                    bool _has_unresolved = false;                                 \
                    for (ArgNode *_ex = _kv->value->array_lit.elements;          \
                         _ex; _ex = _ex->next) {                                 \
                        if (_ex->expr->kind != EXPR_ENUM_ACCESS) {              \
                            _has_unresolved = true; break;                       \
                        }                                                         \
                    }                                                             \
                    if (_has_unresolved) break;                                   \
                    bool _allowed = false;                                        \
                    for (ArgNode *_el = _kv->value->array_lit.elements;          \
                         _el; _el = _el->next) {                                 \
                        Expr *_e = _el->expr;                                    \
                        if (_e->kind == EXPR_ENUM_ACCESS &&                     \
                            _e->enum_access.value == _placement) {              \
                            _allowed = true; break;                              \
                        }                                                         \
                    }                                                             \
                    if (!_allowed) {                                              \
                        /* Build a readable list of allowed targets */           \
                        char _tlist[128] = {0}; int _tpos = 0;                  \
                        static const char *_tnames[] =                           \
                            { "Class","Method","Field","Constructor","Enum" };   \
                        for (ArgNode *_el = _kv->value->array_lit.elements;     \
                             _el; _el = _el->next) {                             \
                            Expr *_e = _el->expr;                                \
                            if (_e->kind == EXPR_ENUM_ACCESS &&                 \
                                _e->enum_access.value >= 0 &&                   \
                                _e->enum_access.value <= 4) {                   \
                                if (_tpos > 0 && _tpos < 120)                   \
                                    _tlist[_tpos++] = ',', _tlist[_tpos++] = ' '; \
                                const char *_tn =                                \
                                    _tnames[_e->enum_access.value];             \
                                int _tl = (int)strlen(_tn);                     \
                                if (_tpos + _tl < 126) {                        \
                                    memcpy(_tlist + _tpos, _tn, _tl);           \
                                    _tpos += _tl;                               \
                                }                                                \
                            }                                                    \
                        }                                                         \
                        _tlist[_tpos] = '\0';                                    \
                        type_error(c, (line_),                                   \
                            "'@%.*s' cannot be applied to " subject_fmt_        \
                            " (allowed targets: %s)",                            \
                            _ann->name_len, _ann->name,                         \
                            ##__VA_ARGS__, _tlist);                              \
                    }                                                             \
                    break;                                                        \
                }                                                                 \
                (void)_found_usage; /* no @AttributeUsage = unrestricted */     \
        } else if (_asym->class_def) {                                           \
                const ClassDef *_cdef = (const ClassDef *)_asym->class_def;     \
                for (int _ai = 0; _ai < _cdef->attribute_count; _ai++) {        \
                    const AttributeInstance *_au = &_cdef->attributes[_ai];     \
                    if (strcmp(_au->class_name, "AttributeUsage") != 0) continue;\
                    if (_au->arg_count == 0) break;                              \
                    const AttrArg *_arr = &_au->args[0];                         \
                    if (_arr->kind != ATTR_ARG_ARRAY) break;                     \
                    /* Empty array = unrestricted (bootstrap/fallback) */        \
                    if (_arr->arr.count == 0) break;                             \
                    bool _allowed = false;                                        \
                    for (int _ei = 0; _ei < _arr->arr.count; _ei++) {           \
                        if (_arr->arr.elems[_ei].kind == ATTR_ARG_INT &&        \
                            _arr->arr.elems[_ei].i == (int64_t)_placement) {    \
                            _allowed = true; break;                              \
                        }                                                         \
                    }                                                             \
                    if (!_allowed) {                                              \
                        char _tlist[128] = {0}; int _tpos = 0;                  \
                        static const char *_tnames2[] =                          \
                            { "Class","Method","Field","Constructor","Enum" };   \
                        for (int _ei = 0; _ei < _arr->arr.count; _ei++) {       \
                            if (_arr->arr.elems[_ei].kind != ATTR_ARG_INT) continue;\
                            int64_t _tv = _arr->arr.elems[_ei].i;               \
                            if (_tv < 0 || _tv > 4) continue;                   \
                            if (_tpos > 0 && _tpos < 120)                       \
                                _tlist[_tpos++] = ',', _tlist[_tpos++] = ' ';  \
                            const char *_tn = _tnames2[_tv];                    \
                            int _tl = (int)strlen(_tn);                         \
                            if (_tpos + _tl < 126) {                            \
                                memcpy(_tlist + _tpos, _tn, _tl);               \
                                _tpos += _tl;                                    \
                            }                                                     \
                        }                                                         \
                        _tlist[_tpos] = '\0';                                    \
                        type_error(c, (line_),                                   \
                            "'@%.*s' cannot be applied to " subject_fmt_        \
                            " (allowed targets: %s)",                            \
                            _ann->name_len, _ann->name,                         \
                            ##__VA_ARGS__, _tlist);                              \
                    }                                                             \
                    break;                                                        \
                }                                                                 \
        }                                                                         \
    } while (0)

    {
        int mod_count = 0;  /* ensure at most one @Mod */
        /* Only run annotation resolution and @AttributeUsage enforcement when
         * the stdlib Attribute base class is in scope. During bootstrap
         * compilation of core.xar itself, Attribute/Target aren't yet
         * defined, so we skip enforcement entirely to avoid false errors. */
        bool stdlib_in_scope = (lookup_symbol(c, "Attribute", 9) != NULL);
        for (StmtNode *n = program->stmts; n; n = n->next) {
            Stmt *s = n->stmt;

            /* ── Enum annotations ─────────────────────────────────────────── */
            if (s->kind == STMT_ENUM_DECL) {
                if (stdlib_in_scope) {
                    for (AnnotationNode *ann = s->enum_decl.annotations;
                         ann; ann = ann->next) {
                        /* check_expr only for enum-access rewriting — suppress errors */
                        int saved_ec = c->error_count; bool saved_he = c->had_error;
                        for (AnnotationKVNode *kv = ann->args; kv; kv = kv->next)
                            if (kv->value) check_expr(c, kv->value);
                        c->error_count = saved_ec; c->had_error = saved_he;
                        CHECK_ATTR_USAGE(ann, TARGET_ENUM, s->line,
                            "enum '%.*s'",
                            s->enum_decl.length, s->enum_decl.name);
                    }
                }
                continue;
            }

            if (s->kind != STMT_CLASS_DECL) continue;

            /* ── Class annotations ────────────────────────────────────────── */
            for (AnnotationNode *ann = s->class_decl.annotations; ann; ann = ann->next) {
                if (stdlib_in_scope) {
                    int saved_ec = c->error_count; bool saved_he = c->had_error;
                    for (AnnotationKVNode *kv = ann->args; kv; kv = kv->next) {
                        if (kv->value) check_expr(c, kv->value);
                    }
                    c->error_count = saved_ec; c->had_error = saved_he;
                    CHECK_ATTR_USAGE(ann, TARGET_CLASS, s->line,
                        "class '%.*s'",
                        s->class_decl.length, s->class_decl.name);
                }

                bool is_mod = (ann->name_len == 3 && memcmp(ann->name, "Mod", 3) == 0);
                if (!is_mod) continue;

                mod_count++;
                if (mod_count > 1) {
                    type_error(c, s->line,
                        "@Mod can only appear on one class per file");
                }

                /* Validate args — @Mod requires at least a name. */
                bool has_name = false;
                for (AnnotationKVNode *kv = ann->args; kv; kv = kv->next) {
                    bool is_name_pos = (kv->key == NULL);
                    bool is_name_key = (kv->key_len == 4 && memcmp(kv->key, "name", 4) == 0);
                    if (is_name_pos || is_name_key) { has_name = true; break; }
                }
                if (!has_name) {
                    type_error(c, s->line,
                        "@Mod on class '%.*s' is missing required key 'name'",
                        s->class_decl.length, s->class_decl.name);
                }
            }

            /* ── Field annotations ────────────────────────────────────────── */
            if (stdlib_in_scope) {
                typedef struct ClassFieldNode CFNode;
                for (CFNode *f = s->class_decl.fields; f; f = f->next) {
                    for (AnnotationNode *ann = f->annotations; ann; ann = ann->next) {
                        int saved_ec = c->error_count; bool saved_he = c->had_error;
                        for (AnnotationKVNode *kv = ann->args; kv; kv = kv->next)
                            if (kv->value) check_expr(c, kv->value);
                        c->error_count = saved_ec; c->had_error = saved_he;
                        CHECK_ATTR_USAGE(ann, TARGET_FIELD, s->line,
                            "field '%.*s'", f->length, f->name);
                    }
                }
            }

            /* ── Method and constructor annotations ───────────────────────── */
            if (stdlib_in_scope) {
                typedef struct ClassMethodNode CMNode;
                for (CMNode *m = s->class_decl.methods; m; m = m->next) {
                    int placement = m->is_constructor ? TARGET_CONSTRUCTOR : TARGET_METHOD;
                    for (AnnotationNode *ann = m->annotations; ann; ann = ann->next) {
                        int saved_ec = c->error_count; bool saved_he = c->had_error;
                        for (AnnotationKVNode *kv = ann->args; kv; kv = kv->next)
                            if (kv->value) check_expr(c, kv->value);
                        c->error_count = saved_ec; c->had_error = saved_he;
                        if (m->is_constructor) {
                            CHECK_ATTR_USAGE(ann, placement, s->line,
                                "constructor of '%.*s'",
                                s->class_decl.length, s->class_decl.name);
                        } else {
                            CHECK_ATTR_USAGE(ann, placement, s->line,
                                "method '%.*s'",
                                s->class_decl.length, s->class_decl.name);
                        }
                    }
                }
            }
        }
    }

    /* ── PASS: override / virtual enforcement ───────────────────────────── *
     * Rules:                                                                *
     *   1. A method marked `override` must have a `virtual` ancestor method *
     *      with the same name and compatible signature.                     *
     *   2. A method NOT marked `override` must NOT shadow a `virtual`       *
     *      method in a parent class (it should use `override`).             *
     *   3. `virtual` + `override` on the same method is a parse error       *
     *      (already caught in the parser).                                  */
    for (StmtNode *n = program->stmts; n; n = n->next) {
        Stmt *s = n->stmt;
        if (s->kind != STMT_CLASS_DECL) continue;
        if (!s->class_decl.parent_name) continue;  /* no parent — nothing to check */

        typedef struct ClassMethodNode CMNode;

        for (CMNode *m = s->class_decl.methods; m; m = m->next) {
            if (m->is_constructor || m->is_static) continue;

            Stmt *fn = m->fn;
            const char *mname = fn->fn_decl.name;
            int         mlen  = fn->fn_decl.length;

            /* Walk ancestor chain looking for a method with the same name.
             * We need to find if ANY ancestor has it marked virtual.       */
            bool found_virtual_match = false;
            bool found_any_match     = false;

            char ancestor[CLASS_NAME_MAX];
            int  alen = s->class_decl.parent_length < CLASS_NAME_MAX - 1
                      ? s->class_decl.parent_length : CLASS_NAME_MAX - 1;
            memcpy(ancestor, s->class_decl.parent_name, alen);
            ancestor[alen] = '\0';

            int safety = 32;
            while (ancestor[0] && safety-- > 0) {
                Symbol *psym = lookup_symbol(c, ancestor, (int)strlen(ancestor));
                if (!psym || psym->kind != SYM_CLASS) break;

                /* Search AST methods if available */
                if (psym->class_decl) {
                    for (CMNode *pm = psym->class_decl->class_decl.methods; pm; pm = pm->next) {
                        if (pm->is_constructor || pm->is_static) continue;
                        Stmt *pfn = pm->fn;
                        if (pfn->fn_decl.length != mlen) continue;
                        if (memcmp(pfn->fn_decl.name, mname, mlen) != 0) continue;
                        found_any_match = true;
                        if (pm->is_virtual) found_virtual_match = true;
                        break;
                    }
                }
                /* Also check MethodDef (stdlib parents) */
                else if (psym->class_def) {
                    ClassDef *cd = (ClassDef *)psym->class_def;
                    for (int mi = 0; mi < cd->method_count; mi++) {
                        MethodDef *md = &cd->methods[mi];
                        if (md->is_static) continue;
                        if ((int)strlen(md->name) != mlen) continue;
                        if (memcmp(md->name, mname, mlen) != 0) continue;
                        found_any_match = true;
                        if (md->is_virtual) found_virtual_match = true;
                        break;
                    }
                }

                /* Keep climbing — virtual may be further up the chain */
                if (psym->class_decl) {
                    if (!psym->class_decl->class_decl.parent_name) break;
                    int pl = psym->class_decl->class_decl.parent_length < CLASS_NAME_MAX - 1
                           ? psym->class_decl->class_decl.parent_length : CLASS_NAME_MAX - 1;
                    memcpy(ancestor, psym->class_decl->class_decl.parent_name, pl);
                    ancestor[pl] = '\0';
                } else break;
            }

            if (m->is_override) {
                /* Rule 1: must find a virtual method in an ancestor */
                if (!found_any_match) {
                    type_error(c, fn->line,
                        "Method '%.*s' is marked 'override' but no matching method "
                        "exists in any ancestor class",
                        mlen, mname);
                } else if (!found_virtual_match) {
                    type_error(c, fn->line,
                        "Method '%.*s' is marked 'override' but the parent method "
                        "is not declared 'virtual'",
                        mlen, mname);
                }
            } else {
                /* Rule 2: silently shadowing a virtual method requires 'override' */
                if (found_virtual_match) {
                    type_error(c, fn->line,
                        "Method '%.*s' shadows a 'virtual' method in a parent class; "
                        "use 'override' to explicitly override it",
                        mlen, mname);
                }
            }
        }
    }

    /* ── PASS 2: Type-check all declarations ────────────────────────────── */
    for (StmtNode *n = program->stmts; n; n = n->next) {
        Stmt *s = n->stmt;

        if (s->kind == STMT_FN_DECL) {
            check_fn_body(c, s);
        }
        else if (s->kind == STMT_CLASS_DECL) {
            /* Type-check each method body with current_class set */
            Stmt *prev_class = c->current_class;
            c->current_class = s;

            /* Push class-level type params into a scope so method bodies can use them.
             * e.g. class Stack<T> — methods can reference T as a type. */
            bool has_class_type_params = (s->class_decl.type_param_count > 0);
            if (has_class_type_params) {
                push_scope(c);
                for (TypeParamNode *tp = s->class_decl.type_params; tp; tp = tp->next) {
                    Symbol tpsym = {0};
                    tpsym.kind   = SYM_VAR;
                    tpsym.name   = tp->name;
                    tpsym.length = tp->length;
                    tpsym.type   = type_param(tp->name);
                    sym_set_loc(&tpsym, c->source_file, s->line, 0);
                    define_symbol(c, tpsym);
                }
            }

            /* Type-check field initializers */
            typedef struct ClassFieldNode CFNode;
            for (CFNode *f = s->class_decl.fields; f; f = f->next) {
                /* Patch field type: enum names parse as TYPE_OBJECT, fix them */
                if (f->type.kind == TYPE_OBJECT && f->type.class_name) {
                    Symbol *maybe_enum = lookup_symbol(c,
                        f->type.class_name, (int)strlen(f->type.class_name));
                    if (maybe_enum && maybe_enum->kind == SYM_ENUM)
                        f->type = type_enum(maybe_enum->enum_name_buf);
                }
                if (!f->initializer) continue;
                Type init_type = check_expr(c, f->initializer);
                if (!is_unknown(init_type) && !types_assignable(f->type, init_type)) {
                    type_error(c, s->line,
                        "Field '%.*s': initializer type %s does not match declared type %s",
                        f->length, f->name,
                        type_kind_name(init_type.kind),
                        type_kind_name(f->type.kind));
                }
                /* Static fields must have constant initializers — no function
                 * calls, no new, no operators. This keeps execution order
                 * unambiguous and allows annotation constant-folding. */
                if (f->is_static && !is_const_expr(f->initializer)) {
                    type_error(c, f->initializer->line,
                        "Static field '%.*s' initializer must be a constant "
                        "(literal, enum member, or static field reference)",
                        f->length, f->name);
                }
            }

            typedef struct ClassMethodNode CMNode;
            for (CMNode *m = s->class_decl.methods; m; m = m->next) {
                /* Type-check annotation argument expressions so that
                 * EXPR_FIELD_GET on enum members gets rewritten to
                 * EXPR_ENUM_ACCESS and .value is filled in before the
                 * compiler pass reads it. */
                for (AnnotationNode *ann = m->annotations; ann; ann = ann->next) {
                    for (AnnotationKVNode *kv = ann->args; kv; kv = kv->next) {
                        if (kv->value) check_expr(c, kv->value);
                    }
                }
                bool prev_static      = c->in_static_method;
                bool prev_ctor        = c->in_constructor;
                c->in_static_method   = m->is_static;
                c->in_constructor     = m->is_constructor;

                if (m->is_constructor) {
                    /* Reset definite-assignment bitfield for this constructor */
                    memset(c->final_field_assigned, 0, sizeof(c->final_field_assigned));
                }

                check_fn_body(c, m->fn);

                if (m->is_constructor) {
                    /* Verify every final field without an inline initializer was
                     * assigned somewhere in the constructor body */
                    typedef struct ClassFieldNode CFNode2;
                    int fidx = 0;
                    for (CFNode2 *f = s->class_decl.fields; f; f = f->next, fidx++) {
                        if (!f->is_final || f->is_static) continue;
                        bool has_inline = (f->initializer != NULL);
                        bool assigned   = (fidx < 64 && c->final_field_assigned[fidx]);
                        if (!has_inline && !assigned) {
                            type_error(c, m->fn->line,
                                "Final field '%.*s' must be assigned in the constructor",
                                f->length, f->name);
                        }
                    }
                }

                c->in_static_method = prev_static;
                c->in_constructor   = prev_ctor;
            }

            if (has_class_type_params) {
                pop_scope(c);
            }

            c->current_class = prev_class;
        }
        else if (s->kind == STMT_ENUM_DECL) {
            /* Enums are compile-time only — no method bodies to check */
        }
        else if (s->kind == STMT_INTERFACE_DECL) {
            /* Interfaces are pure compile-time contracts — no bodies to check */
        }
        else if (s->kind == STMT_IMPORT) {
            /* Import declarations resolved before checker runs — skip */
        }
        else if (s->kind == STMT_EVENT_DECL) {
            /* Top-level event declarations — registered in Pass 1, nothing to type-check here */
        }
        else {
            type_error(c, s->line,
                "Only function, class, and enum declarations are allowed at the top level");
        }
    }

    return !c->had_error;
}

void checker_print_errors(const Checker *c) {
    for (int i = 0; i < c->error_count; i++) {
        if (c->errors[i].is_warning) {
            fprintf(stdout, "[line %d] Warning: %s\n",
                    c->errors[i].line,
                    c->errors[i].message);
        } else {
            fprintf(stdout, "[line %d] Type error: %s\n",
                    c->errors[i].line,
                    c->errors[i].message);
        }
    }
}
/* ── LSP query API ───────────────────────────────────────────────────────── */

/* Return the symbol whose usage record best covers (line, col).
 * "Best" means: same line, col falls within [record.col, record.col+length). */
Symbol *checker_find_symbol_at(const Checker *c, int line, int col) {
    Symbol *best = NULL;
    int     best_len = INT_MAX;
    for (int i = 0; i < c->usage_count; i++) {
        const UsageRecord *r = &c->usages[i];
        if (r->line != line) continue;
        if (col < r->col || col >= r->col + r->length) continue;
        /* Prefer narrower match (shorter token) */
        if (r->length < best_len) {
            best     = r->sym;
            best_len = r->length;
        }
    }
    return best;
}

const Symbol *checker_find_definition(const Checker *c, int line, int col) {
    return checker_find_symbol_at(c, line, col);
}

int checker_usages_of(const Checker *c, int line, int col,
                      UsageRecord *out, int max) {
    Symbol *target = checker_find_symbol_at(c, line, col);
    if (!target) return 0;
    int count = 0;
    for (int i = 0; i < c->usage_count && count < max; i++) {
        const UsageRecord *r = &c->usages[i];
        /* Match by pointer identity only — this is the only way to
         * distinguish two different variables that happen to share the same
         * name (e.g. a parameter 'n' and a local variable 'n' in a nested
         * scope). Name-text matching was previously used as a fallback but
         * caused false positives. */
        if (r->sym == target) {
            out[count++] = *r;
        }
    }
    return count;
}
