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
static bool types_assignable(const Type lhs, const Type rhs) {
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
                                Expr *implicit_this = expr_this(c->arena, expr->line);
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
                return resolve(expr, type_class_ref(sym->class_name_buf));
            }
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
                if (!type_equals(left, right)) {
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
                                    Expr *implicit_this = expr_this(c->arena, expr->line);
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
                    if (!cls_sym || cls_sym->kind != SYM_CLASS || !cls_sym->class_decl) {
                        return error_type(c, expr, expr->line, "Unknown class '%s'", cname);
                    }
                    typedef struct ClassFieldNode CFNode;
                    for (CFNode *f = cls_sym->class_decl->class_decl.fields; f; f = f->next) {
                        if (f->length == flen && memcmp(f->name, fname, flen) == 0) {
                            if (!f->is_static) {
                                return error_type(c, expr, expr->line,
                                    "'%s' on '%.*s': field is not static", op_str, flen, fname);
                            }
                            if (f->type.kind != TYPE_INT) {
                                return error_type(c, expr, expr->line,
                                    "'%s' operator requires an int field, got %s",
                                    op_str, type_kind_name(f->type.kind));
                            }
                            expr->postfix.is_static_field = true;
                            return resolve(expr, type_int());
                        }
                    }
                    return error_type(c, expr, expr->line,
                        "Class '%s' has no static field '%.*s'", cname, flen, fname);
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
                Stmt *cls = cls_sym->class_decl;
                if (!cls) {
                    return error_type(c, expr, expr->line,
                        "Internal: class '%s' has no declaration", obj_type.class_name);
                }
                typedef struct ClassFieldNode CFNode;
                for (CFNode *f = cls->class_decl.fields; f; f = f->next) {
                    if (f->length == expr->postfix.field_name_len &&
                        memcmp(f->name, expr->postfix.field_name, f->length) == 0) {
                        if (f->type.kind != TYPE_INT) {
                            return error_type(c, expr, expr->line,
                                "'%s' operator requires an int field, got %s",
                                op_str, type_kind_name(f->type.kind));
                        }
                        return resolve(expr, type_int());
                    }
                }
                return error_type(c, expr, expr->line,
                    "Class '%s' has no field '%.*s'",
                    obj_type.class_name,
                    expr->postfix.field_name_len, expr->postfix.field_name);
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
                                Expr *implicit_this = expr_this(c->arena, expr->line);
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
                                Expr    *implicit_this = expr_this(c->arena, expr->line);
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

            /* Check argument count */
            if (expr->call.arg_count != sym->param_count) {
                return error_type(c, expr, expr->line,
                    "Function '%.*s' expects %d argument(s), got %d",
                    expr->call.length, expr->call.name,
                    sym->param_count, expr->call.arg_count);
            }

            /* Check each argument's type against the parameter type */
            ArgNode *arg  = expr->call.args;
            int      idx  = 0;
            bool     args_ok = true;
            while (arg) {
                Type arg_type = check_expr(c, arg->expr);
                if (!is_unknown(arg_type) &&
                    sym->param_types[idx].kind != TYPE_ANY &&
                    !types_assignable(sym->param_types[idx], arg_type))
                {
                    type_error(c, expr->line,
                        "Argument %d to '%.*s': expected %s, got %s",
                        idx + 1,
                        expr->call.length, expr->call.name,
                        type_kind_name(sym->param_types[idx].kind),
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

            /* Call expression's type is the function's return type */
            return resolve(expr, sym->type);
        }

        /* ── new ClassName(args) ────────────────────────────────────────── */
        case EXPR_NEW: {
            const char *cname = expr->new_expr.class_name;
            int         clen  = expr->new_expr.class_name_len;

            Symbol *cls_sym = lookup_symbol(c, cname, clen);
            if (!cls_sym || cls_sym->kind != SYM_CLASS) {
                return error_type(c, expr, expr->line,
                    "Unknown class '%.*s'", clen, cname);
            }

            /* Use the null-terminated class_name_buf so type_equals works */
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

            Symbol *cls_sym = lookup_symbol(c,
                obj_type.class_name, (int)strlen(obj_type.class_name));
            if (!cls_sym || cls_sym->kind != SYM_CLASS) {
                return error_type(c, expr, expr->line,
                    "Unknown class '%s'", obj_type.class_name);
            }

            Stmt *cls = cls_sym->class_decl;
            if (!cls) {
                return error_type(c, expr, expr->line,
                    "Internal: class '%s' has no declaration", obj_type.class_name);
            }

            typedef struct ClassFieldNode CFNode;
            /* Walk the inheritance chain looking for the field */
            Stmt *search_cls = cls;
            char  search_name[CLASS_NAME_MAX];
            int   search_len = (int)strlen(obj_type.class_name) < CLASS_NAME_MAX - 1
                             ? (int)strlen(obj_type.class_name) : CLASS_NAME_MAX - 1;
            memcpy(search_name, obj_type.class_name, search_len);
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
                        return resolve(expr, f->type);
                    }
                }
                /* Move to parent class */
                if (search_cls->class_decl.parent_name && search_cls->class_decl.parent_length > 0) {
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
            return error_type(c, expr, expr->line,
                "Class '%s' has no field '%.*s'",
                obj_type.class_name,
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
                if (!cls_sym || cls_sym->kind != SYM_CLASS || !cls_sym->class_decl) {
                    return error_type(c, expr, expr->line, "Unknown class '%s'", cname);
                }

                typedef struct ClassFieldNode CFNode;
                for (CFNode *f = cls_sym->class_decl->class_decl.fields; f; f = f->next) {
                    if (f->length == flen && memcmp(f->name, fname, flen) == 0) {
                        if (!f->is_static) {
                            return error_type(c, expr, expr->line,
                                "Field '%.*s' of '%s' is not static", flen, fname, cname);
                        }
                        if (!type_equals(val_type, f->type) && !is_unknown(val_type)) {
                            return error_type(c, expr, expr->line,
                                "Cannot assign %s to static field '%.*s' of type %s",
                                type_kind_name(val_type.kind), flen, fname,
                                type_kind_name(f->type.kind));
                        }
                        /* Save value pointer BEFORE rewriting union in-place */
                        Expr *saved_value              = expr->field_set.value;
                        expr->kind                     = EXPR_STATIC_SET;
                        expr->static_set.class_name     = cname;
                        expr->static_set.class_name_len = (int)strlen(cname);
                        expr->static_set.field_name     = fname;
                        expr->static_set.field_name_len = flen;
                        expr->static_set.value          = saved_value;
                        expr->static_set.class_idx      = -1;
                        expr->static_set.field_idx      = -1;
                        return resolve(expr, f->type);
                    }
                }
                return error_type(c, expr, expr->line,
                    "Class '%s' has no static field '%.*s'", cname, flen, fname);
            }

            if (obj_type.kind != TYPE_OBJECT) {
                return error_type(c, expr, expr->line,
                    "Cannot set field on non-object type %s",
                    type_kind_name(obj_type.kind));
            }

            Symbol *cls_sym = lookup_symbol(c,
                obj_type.class_name, (int)strlen(obj_type.class_name));
            if (!cls_sym || cls_sym->kind != SYM_CLASS) {
                return error_type(c, expr, expr->line,
                    "Unknown class '%s'", obj_type.class_name);
            }

            Stmt *cls = cls_sym->class_decl;
            if (!cls) {
                return error_type(c, expr, expr->line,
                    "Internal: class '%s' has no declaration", obj_type.class_name);
            }

            typedef struct ClassFieldNode CFNode;
            /* Walk the inheritance chain looking for the field */
            Stmt *search_cls = cls;
            char  search_name[CLASS_NAME_MAX];
            int   search_len = (int)strlen(obj_type.class_name) < CLASS_NAME_MAX - 1
                             ? (int)strlen(obj_type.class_name) : CLASS_NAME_MAX - 1;
            memcpy(search_name, obj_type.class_name, search_len);
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
                        if (!type_equals(f->type, val_type) && !is_unknown(val_type)) {
                            return error_type(c, expr, expr->line,
                                "Cannot assign %s to field '%.*s' of type %s",
                                type_kind_name(val_type.kind),
                                expr->field_set.field_name_len, expr->field_set.field_name,
                                type_kind_name(f->type.kind));
                        }
                        return resolve(expr, f->type);
                    }
                }
                /* Move to parent class */
                if (search_cls->class_decl.parent_name && search_cls->class_decl.parent_length > 0) {
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
            return error_type(c, expr, expr->line,
                "Class '%s' has no field '%.*s'",
                obj_type.class_name,
                expr->field_set.field_name_len, expr->field_set.field_name);
        }

        /* ── obj.method(args) ──────────────────────────────────────────── */
        case EXPR_METHOD_CALL: {
            Type obj_type = check_expr(c, expr->method_call.object);

            /* ── Static method call: ClassName.method(args) ─────────────── */
            if (obj_type.kind == TYPE_CLASS_REF) {
                const char *cname  = obj_type.class_name;
                const char *mname  = expr->method_call.method_name;
                int         mlen   = expr->method_call.method_name_len;

                Symbol *cls_sym = lookup_symbol(c, cname, (int)strlen(cname));
                if (!cls_sym || cls_sym->kind != SYM_CLASS || !cls_sym->class_decl) {
                    return error_type(c, expr, expr->line, "Unknown class '%s'", cname);
                }

                typedef struct ClassMethodNode CMNode;
                for (CMNode *m = cls_sym->class_decl->class_decl.methods; m; m = m->next) {
                    if (m->is_constructor) continue;
                    if (!m->is_static) continue;
                    if (m->fn->fn_decl.length == mlen &&
                        memcmp(m->fn->fn_decl.name, mname, mlen) == 0) {
                        /* Check arg count */
                        int expected = 0;
                        typedef struct ParamNode PNode;
                        for (PNode *p = m->fn->fn_decl.params; p; p = p->next) expected++;
                        if (expr->method_call.arg_count != expected) {
                            return error_type(c, expr, expr->line,
                                "Static method '%.*s' expects %d argument(s), got %d",
                                mlen, mname, expected, expr->method_call.arg_count);
                        }
                        /* Type-check args */
                        int ai = 0;
                        PNode *p = m->fn->fn_decl.params;
                        for (ArgNode *arg = expr->method_call.args; arg && p;
                             arg = arg->next, p = p->next, ai++) {
                            Type at = check_expr(c, arg->expr);
                            if (!is_unknown(at) && !type_equals(at, p->type)) {
                                type_error(c, arg->expr->line,
                                    "Argument %d to '%.*s': expected %s, got %s",
                                    ai+1, mlen, mname,
                                    type_kind_name(p->type.kind),
                                    type_kind_name(at.kind));
                            }
                        }
                        /* Save args pointer and count BEFORE rewriting union in-place */
                        ArgNode *saved_args  = expr->method_call.args;
                        int      saved_argc  = expr->method_call.arg_count;
                        /* Rewrite to EXPR_STATIC_CALL */
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

            if (obj_type.kind != TYPE_OBJECT) {
                return error_type(c, expr, expr->line,
                    "Cannot call method on non-object type %s",
                    type_kind_name(obj_type.kind));
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
                    if (expr->method_call.arg_count != sig->param_count) {
                        type_error(c, expr->line,
                            "Method '%.*s' expects %d argument(s), got %d",
                            mlen, mname, sig->param_count,
                            expr->method_call.arg_count);
                    }
                    return resolve(expr, sig->return_type);
                }
            }

            Symbol *cls_sym = lookup_symbol(c,
                obj_type.class_name, (int)strlen(obj_type.class_name));
            if (!cls_sym || cls_sym->kind != SYM_CLASS) {
                return error_type(c, expr, expr->line,
                    "Unknown class '%s'", obj_type.class_name);
            }

            Stmt *cls = cls_sym->class_decl;
            if (!cls) {
                return error_type(c, expr, expr->line,
                    "Internal: class '%s' has no declaration", obj_type.class_name);
            }

            typedef struct ClassMethodNode CMNode;
            /* Walk the inheritance chain looking for the method */
            Stmt *search_cls = cls;
            char  search_name[CLASS_NAME_MAX];
            int   search_len = (int)strlen(obj_type.class_name) < CLASS_NAME_MAX - 1
                             ? (int)strlen(obj_type.class_name) : CLASS_NAME_MAX - 1;
            memcpy(search_name, obj_type.class_name, search_len);
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
                        /* Type-check arguments */
                        ArgNode *arg = expr->method_call.args;
                        ParamNode *param = m->fn->fn_decl.params;
                        int idx = 0;
                        while (arg && param) {
                            Type at = check_expr(c, arg->expr);
                            if (!type_equals(at, param->type) &&
                                !is_unknown(at) && !is_unknown(param->type)) {
                                type_error(c, expr->line,
                                    "Method '%.*s' argument %d: expected %s, got %s",
                                    expr->method_call.method_name_len,
                                    expr->method_call.method_name,
                                    idx + 1,
                                    type_kind_name(param->type.kind),
                                    type_kind_name(at.kind));
                            }
                            arg = arg->next; param = param->next; idx++;
                        }
                        return resolve(expr, m->fn->fn_decl.return_type);
                    }
                }
                /* Move to parent class */
                if (search_cls->class_decl.parent_name && search_cls->class_decl.parent_length > 0) {
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
            return error_type(c, expr, expr->line,
                "Class '%s' has no method '%.*s'",
                obj_type.class_name,
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
            /* Find the parent constructor and type-check args */
            Stmt *pcls = psym->class_decl;
            typedef struct ClassMethodNode CMNode;
            for (CMNode *m = pcls->class_decl.methods; m; m = m->next) {
                if (!m->is_constructor) continue;
                ArgNode   *arg   = expr->super_call.args;
                ParamNode *param = m->fn->fn_decl.params;
                int idx = 0;
                while (arg && param) {
                    Type at = check_expr(c, arg->expr);
                    if (!type_equals(at, param->type) &&
                        !is_unknown(at) && !is_unknown(param->type)) {
                        type_error(c, expr->line,
                            "super() argument %d: expected %s, got %s",
                            idx + 1,
                            type_kind_name(param->type.kind),
                            type_kind_name(at.kind));
                    }
                    arg = arg->next; param = param->next; idx++;
                }
                return resolve(expr, type_void());
            }
            /* Parent has no explicit constructor — zero-arg super() is fine */
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
                Symbol *maybe_sym = lookup_symbol(c,
                    declared.class_name, (int)strlen(declared.class_name));
                if (maybe_sym && maybe_sym->kind == SYM_ENUM) {
                    declared = type_enum(maybe_sym->enum_name_buf);
                    stmt->var_decl.type = declared;
                }
                /* Interface type: keep as TYPE_OBJECT but validate it exists */
                else if (maybe_sym && maybe_sym->kind == SYM_INTERFACE) {
                    /* already TYPE_OBJECT with class_name = interface name — fine */
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
                bool compatible = types_assignable(declared, init_type) ||
                    (declared.kind == TYPE_OBJECT && init_type.kind == TYPE_OBJECT &&
                     declared.class_name && init_type.class_name &&
                     is_subtype(c, init_type.class_name, declared.class_name)) ||
                    (is_iface_var && declared.class_name &&
                     type_is_assignable_to_interface(c, init_type, declared.class_name)) ||
                    iface_array_ok;
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

            if (!define_symbol(c, sym)) {
                type_error(c, stmt->line,
                    "Variable '%.*s' already declared in this scope",
                    stmt->var_decl.length, stmt->var_decl.name);
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
            if (!is_unknown(arr_type) && arr_type.kind != TYPE_ARRAY) {
                type_error(c, stmt->line, "foreach requires an array, got %s", type_kind_name(arr_type.kind)); break;
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
                if (!is_unknown(actual) && !type_equals(expected, actual)) {
                    type_error(c, stmt->line,
                        "Return type mismatch: function returns %s, got %s",
                        type_kind_name(expected.kind),
                        type_kind_name(actual.kind));
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
        define_symbol(c, this_sym);
    }

    /* Define parameters as variables in the function's scope */
    for (ParamNode *p = fn_stmt->fn_decl.params; p; p = p->next) {
        Symbol sym = {0};
        sym.kind   = SYM_VAR;
        sym.name   = p->name;
        sym.length = p->length;
        sym.type   = p->type;
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

    pop_scope(c);
    c->inside_function = false;
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
    define_symbol(c, sym);  /* global scope (depth 0) */
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
            Symbol sym = {0};
            sym.kind        = SYM_FN;
            sym.name        = s->fn_decl.name;
            sym.length      = s->fn_decl.length;
            sym.type        = s->fn_decl.return_type;
            sym.param_count = s->fn_decl.param_count;
            int i = 0;
            for (ParamNode *p = s->fn_decl.params; p && i < MAX_PARAMS; p = p->next)
                sym.param_types[i++] = p->type;
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

            if (!define_symbol(c, sym)) {
                type_error(c, s->line, "Class '%.*s' already declared",
                           s->class_decl.length, s->class_decl.name);
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

            if (!define_symbol(c, sym)) {
                type_error(c, s->line, "Interface '%.*s' already declared",
                           s->interface_decl.length, s->interface_decl.name);
            }
        }
    }

    /* ── PASS 1b: Resolve parent class and implemented interfaces ───────── */
    for (StmtNode *n = program->stmts; n; n = n->next) {
        Stmt *s = n->stmt;
        if (s->kind != STMT_CLASS_DECL) continue;
        if (!s->class_decl.interfaces) continue;

        typedef struct IfaceNameNode IFNode;
        int parent_count = 0;

        for (IFNode *in = s->class_decl.interfaces; in; in = in->next) {
            Symbol *sym = lookup_symbol(c, in->name, in->length);
            if (!sym) {
                type_error(c, s->line,
                    "Class '%.*s': '%.*s' is not declared",
                    s->class_decl.length, s->class_decl.name,
                    in->length, in->name);
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
                            /* Name matches — check return type */
                            if (!type_equals(fn->fn_decl.return_type, req->return_type)) {
                                type_error(c, s->line,
                                    "Class '%.*s' implements '%.*s': method '%.*s' "
                                    "has wrong return type (expected %s, got %s)",
                                    s->class_decl.length, s->class_decl.name,
                                    in->length, in->name,
                                    req->length, req->name,
                                    type_kind_name(req->return_type.kind),
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
                                    if (!type_equals(cp->type, rp->type)) {
                                        type_error(c, s->line,
                                            "Class '%.*s' implements '%.*s': "
                                            "method '%.*s' parameter %d type mismatch "
                                            "(expected %s, got %s)",
                                            s->class_decl.length, s->class_decl.name,
                                            in->length, in->name,
                                            req->length, req->name, pidx + 1,
                                            type_kind_name(rp->type.kind),
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
    {
        int mod_count = 0;  /* ensure at most one @Mod */
        for (StmtNode *n = program->stmts; n; n = n->next) {
            Stmt *s = n->stmt;
            if (s->kind != STMT_CLASS_DECL) continue;

            for (AnnotationNode *ann = s->class_decl.annotations; ann; ann = ann->next) {
                /* Only @Mod is defined for now — reject anything else */
                if (ann->name_len != 3 || memcmp(ann->name, "Mod", 3) != 0) {
                    type_error(c, s->line,
                        "Unknown annotation '@%.*s' on class '%.*s'",
                        ann->name_len, ann->name,
                        s->class_decl.length, s->class_decl.name);
                    continue;
                }

                mod_count++;
                if (mod_count > 1) {
                    type_error(c, s->line,
                        "@Mod can only appear on one class per file");
                }

                /* Validate keys — only name/version/author/description allowed */
                bool has_name = false;
                for (AnnotationKVNode *kv = ann->args; kv; kv = kv->next) {
                    bool valid =
                        (kv->key_len == 4 && memcmp(kv->key, "name",        4) == 0) ||
                        (kv->key_len == 7 && memcmp(kv->key, "version",     7) == 0) ||
                        (kv->key_len == 6 && memcmp(kv->key, "author",      6) == 0) ||
                        (kv->key_len == 11&& memcmp(kv->key, "description", 11) == 0);
                    if (!valid) {
                        type_error(c, s->line,
                            "@Mod: unknown key '%.*s' (allowed: name, version, author, description)",
                            kv->key_len, kv->key);
                    }
                    if (kv->key_len == 4 && memcmp(kv->key, "name", 4) == 0)
                        has_name = true;
                }

                if (!has_name) {
                    type_error(c, s->line,
                        "@Mod on class '%.*s' is missing required key 'name'",
                        s->class_decl.length, s->class_decl.name);
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
            }

            typedef struct ClassMethodNode CMNode;
            for (CMNode *m = s->class_decl.methods; m; m = m->next) {
                bool prev_static      = c->in_static_method;
                c->in_static_method   = m->is_static;
                check_fn_body(c, m->fn);
                c->in_static_method   = prev_static;
            }

            c->current_class = prev_class;
        }
        else if (s->kind == STMT_ENUM_DECL) {
            /* Enums are compile-time only — no method bodies to check */
        }
        else if (s->kind == STMT_INTERFACE_DECL) {
            /* Interfaces are pure compile-time contracts — no bodies to check */
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