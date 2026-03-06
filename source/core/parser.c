/*
 * parser.c — Recursive descent parser with Pratt expression parsing
 *
 * Structure:
 *   - Token management  (advance, consume, peek, match)
 *   - Error handling    (error_at, synchronize)
 *   - Expression parser (Pratt algorithm)
 *   - Statement parser  (recursive descent)
 *   - Top-level         (parser_parse)
 */

#include "parser.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>

/* ─────────────────────────────────────────────────────────────────────────────
 * TOKEN MANAGEMENT
 * ───────────────────────────────────────────────────────────────────────────*/

/* Consume the current token and fetch the next one.
 * The consumed token becomes `previous`, the new one becomes `current`.
 * We maintain current, next, and peek (3-token lookahead). */
static void advance(Parser *p) {
    p->previous = p->current;
    p->current  = p->next;
    p->next     = p->peek;

    /* Fill peek — keep fetching until we get a non-error token */
    for (;;) {
        p->peek = lexer_next_token(p->lexer);
        if (p->peek.type != TOK_ERROR) break;

        /* Report the lexer error but don't set panic_mode */
        if (p->error_count < PARSER_MAX_ERRORS) {
            ParseError *e = &p->errors[p->error_count++];
            snprintf(e->message, sizeof(e->message),
                     "Lexer error: %.*s", p->peek.length, p->peek.start);
            e->line = p->peek.line;
            p->had_error = true;
        }
    }
}

/* Check the current token type without consuming */
static bool check(const Parser *p, TokenType type) {
    return p->current.type == type;
}

/* If the current token matches, consume it and return true. Else false. */
static bool match(Parser *p, TokenType type) {
    if (!check(p, type)) return false;
    advance(p);
    return true;
}

/* Consume the current token if it matches type, else record an error.
 * Returns true on success. */
static bool consume(Parser *p, TokenType type, const char *message) {
    if (check(p, type)) {
        advance(p);
        return true;
    }

    if (!p->panic_mode && p->error_count < PARSER_MAX_ERRORS) {
        ParseError *e = &p->errors[p->error_count++];
        snprintf(e->message, sizeof(e->message),
                 "%s (got '%s')", message, token_type_name(p->current.type));
        e->line      = p->current.line;
        p->had_error  = true;
        p->panic_mode = true;
    }
    return false;
}


/* ─────────────────────────────────────────────────────────────────────────────
 * ERROR RECOVERY — SYNCHRONIZATION
 *
 * When we hit a parse error we enter "panic mode". In panic mode we discard
 * tokens until we find a synchronization point — a token that can safely
 * start a new statement. This prevents one error from causing a cascade of
 * meaningless follow-on errors.
 *
 * Good sync points are: semicolons (end of a statement) and statement-starting
 * keywords (fn, if, while, for, return). After a semicolon we stop AFTER
 * consuming it. At a keyword we stop BEFORE consuming it so the statement
 * parser can handle it normally.
 * ───────────────────────────────────────────────────────────────────────────*/
static void synchronize(Parser *p) {
    p->panic_mode = false;

    while (!check(p, TOK_EOF)) {
        /* A semicolon ends a statement — safe to resume after it */
        if (p->previous.type == TOK_SEMICOLON) return;

        /* These tokens start new statements — safe to resume here */
        switch (p->current.type) {
            case TOK_FN:
            case TOK_INT: case TOK_FLOAT: case TOK_BOOL:
            case TOK_STRING: case TOK_VOID:
            case TOK_IF:
            case TOK_WHILE:
            case TOK_FOR:
            case TOK_FOREACH:
            case TOK_RETURN:
            case TOK_BREAK:
            case TOK_CONTINUE:
                return;
            default:
                break;
        }
        advance(p);
    }
}


/* ─────────────────────────────────────────────────────────────────────────────
 * TYPE PARSING
 *
 * Parses a type keyword and returns the corresponding Type struct.
 * Called wherever the grammar requires an explicit type annotation.
 * ───────────────────────────────────────────────────────────────────────────*/
static Type parse_base_type(Parser *p);
static TypeParamNode *parse_type_param_list(Parser *p, int *out_count);
static TypeArgNode   *parse_type_arg_list  (Parser *p, int *out_count);

#define MAX_TYPE_ARGS 8   /* Must match checker.c */

/* is_generic_type_name — returns true if the name contains '<' */
static bool is_generic_type_name(const char *name) {
    return name && strchr(name, '<') != NULL;
}

/* generic_base_name — extract "Stack" from "Stack<int>" into buf. */
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

/* parse_canonical_type_args — extract type args from "Stack<int>" canonical name.
 * Mirrors the checker version for use in the parser. */
static int parse_canonical_type_args(const char *full_name,
                                     Type *out_args, int max_args)
{
    const char *lt = strchr(full_name, '<');
    if (!lt) return 0;
    const char *p = lt + 1;
    int count = 0;
    while (*p && *p != '>' && count < max_args) {
        while (*p == ' ') p++;
        if (*p == '>') break;
        const char *start = p;
        while (*p && *p != ',' && *p != '>' && *p != ' ') p++;
        int len = (int)(p - start);
        Type t;
        if      (len == 3 && strncmp(start, "int",    3) == 0) t = type_int();
        else if (len == 5 && strncmp(start, "float",  5) == 0) t = type_float();
        else if (len == 4 && strncmp(start, "bool",   4) == 0) t = type_bool();
        else if (len == 6 && strncmp(start, "string", 6) == 0) t = type_string();
        else if (len == 4 && strncmp(start, "void",   4) == 0) t = type_void();
        else if (len == 4 && strncmp(start, "char",   4) == 0) t = type_char();
        else {
            static char nbuf[64]; int nlen = len < 63 ? len : 63;
            memcpy(nbuf, start, nlen); nbuf[nlen] = '\0';
            t = type_object(nbuf);
        }
        out_args[count++] = t;
        while (*p == ' ') p++;
        if (*p == ',') p++;
    }
    return count;
}

static Type parse_type(Parser *p) {
    Type base = parse_base_type(p);
    /* Array suffix: int[], string[][], etc. */
    while (p->current.type == TOK_LBRACKET && p->next.type == TOK_RBRACKET) {
        advance(p); /* [ */
        advance(p); /* ] */
        Type *heap = arena_alloc(&p->arena, sizeof(Type));
        *heap = base;
        base = type_array(heap);
    }
    return base;
}

static Type
parse_base_type(Parser *p) {
    Token t = p->current;
    switch (t.type) {
        case TOK_INT:    advance(p); return type_int();
        case TOK_FLOAT:  advance(p); return type_float();
        case TOK_BOOL:   advance(p); return type_bool();
        case TOK_STRING: advance(p); return type_string();
        case TOK_VOID:   advance(p); return type_void();
        /* Numeric type keywords */
        case TOK_SBYTE:  advance(p); return type_sbyte();
        case TOK_BYTE:   advance(p); return type_byte();
        case TOK_SHORT:  advance(p); return type_short();
        case TOK_USHORT: advance(p); return type_ushort();
        case TOK_UINT:   advance(p); return type_uint();
        case TOK_LONG:   advance(p); return type_long();
        case TOK_ULONG:  advance(p); return type_ulong();
        case TOK_DOUBLE: advance(p); return type_double();
        case TOK_CHAR:   advance(p); return type_char();
        case TOK_IDENT: {
            /* A bare identifier as a type means a class name, e.g. Greeter x.
             * It might also be a generic instantiation: Stack<int> x.
             * We allocate a null-terminated copy in the arena so type_equals
             * and strlen() on class_name work correctly everywhere. */
            advance(p);
            char *name = arena_alloc(&p->arena, t.length + 1);
            memcpy(name, t.start, t.length);
            name[t.length] = '\0';

            /* Check for generic type args: Stack<int>, Pair<string, int>, etc.
             * We detect this by looking for '<' after the identifier.
             * To disambiguate from comparison operators, we only do this when
             * the token after '<' looks like a type (keyword or ident) and
             * the token after that is '>' or ','. We use a simple lookahead:
             * peek two tokens ahead. If p->next is a type-start, parse as type args.
             * We encode the type args into the class_name as "ClassName<arg1,arg2>"
             * so the checker can decode it. Actually simpler: store in class_name
             * as a canonical string that the checker recognizes. */
            if (p->current.type == TOK_LT) {
                /* Build a canonical name like "Stack<int>" or "Pair<int,string>"
                 * by parsing the type args and serializing them into a name buffer.
                 * The checker will use this canonical name to look up the generic. */
                int type_arg_count = 0;
                TypeArgNode *type_args = parse_type_arg_list(p, &type_arg_count);

                /* Build canonical name: ClassName<arg1,arg2,...> */
                /* First compute total length needed */
                int base_len = t.length;
                int total = base_len + 1; /* '<' */
                TypeArgNode *ta = type_args;
                int i = 0;
                while (ta) {
                    if (i++ > 0) total += 1; /* ',' */
                    /* type name length — approximate using type_kind_name */
                    if (ta->type.kind == TYPE_OBJECT && ta->type.class_name)
                        total += (int)strlen(ta->type.class_name);
                    else
                        total += (int)strlen(type_kind_name(ta->type.kind));
                    ta = ta->next;
                }
                total += 2; /* '>' and '\0' */

                char *full_name = arena_alloc(&p->arena, total);
                int pos = 0;
                memcpy(full_name + pos, t.start, base_len); pos += base_len;
                full_name[pos++] = '<';
                ta = type_args; i = 0;
                while (ta) {
                    if (i++ > 0) full_name[pos++] = ',';
                    const char *tname;
                    if (ta->type.kind == TYPE_OBJECT && ta->type.class_name)
                        tname = ta->type.class_name;
                    else
                        tname = type_kind_name(ta->type.kind);
                    int tlen = (int)strlen(tname);
                    memcpy(full_name + pos, tname, tlen); pos += tlen;
                    ta = ta->next;
                }
                full_name[pos++] = '>';
                full_name[pos]   = '\0';

                /* Store as TYPE_OBJECT with the canonical generic name.
                 * Also store the type args for the checker to use. */
                Type result = type_object(full_name);
                /* We embed the type_args into the type via a side channel:
                 * since Type has no TypeArgNode field, we use a naming convention
                 * and rely on the checker to parse the canonical name back.
                 * This keeps Type as a plain value type. */
                (void)type_args; /* checker reconstructs from the canonical name */
                return result;
            }

            return type_object(name);
        }
        default:
            consume(p, TOK_INT, "Expected a type (int, float, bool, string, void, or class name)");
            return type_int();
    }
}


/* ─────────────────────────────────────────────────────────────────────────────
 * PRATT EXPRESSION PARSER
 *
 * Binding powers for each operator. We use an enum so the values are named
 * and easy to change. Higher value = higher precedence = binds more tightly.
 *
 * Left binding power (left_bp) is what the operator presents to its LEFT
 * neighbor. Right binding power (right_bp) is what it passes to the recursive
 * call for its RIGHT operand.
 *
 * For left-associative ops:  right_bp = left_bp + 1
 *   This means after we parse the right side, the same operator won't
 *   "steal" that result back — it will stop because left_bp <= min_bp.
 *   So  a + b + c  parses as  (a + b) + c  ✓
 *
 * For right-associative ops: right_bp = left_bp
 *   The same operator CAN steal back the right side.
 *   So  a = b = 5  parses as  a = (b = 5)  ✓
 * ───────────────────────────────────────────────────────────────────────────*/
typedef enum {
    BP_NONE       = 0,
    BP_ASSIGN     = 1,   /* =          right-associative */
    BP_OR         = 2,   /* ||         left-associative  */
    BP_AND        = 3,   /* &&         left-associative  */
    BP_EQUALITY   = 4,   /* == !=      left-associative  */
    BP_COMPARISON = 5,   /* < <= > >=  left-associative  */
    BP_TERM       = 7,   /* + -        left-associative  (gap ensures < right parses + - ) */
    BP_FACTOR     = 9,   /* * / %      left-associative  */
    BP_UNARY      = 11,  /* - !        right-associative (prefix) */
    BP_CALL       = 13,  /* foo(...)   left-associative  */
} BindingPower;

/* Forward declaration — parse_expr and parse_prefix call each other
 * indirectly through parse_stmt -> parse_expr */
static Expr *parse_expr(Parser *p, int min_bp);

/*
 * parse_prefix — handles tokens that can START an expression:
 *   - Literals (int, float, bool, string)
 *   - Identifiers (variable reference or function call)
 *   - Unary operators (- and !)
 *   - Grouped expressions: (expr)
 *
 * This is called at the beginning of parse_expr to get the "left" side.
 */
static Expr *parse_prefix(Parser *p) {
    Token t = p->current;
    advance(p);  /* consume the token we're dispatching on */

    switch (t.type) {

        /* ── Integer literal ───────────────────────────────────────────── */
        case TOK_INT_LIT: {
            /* Convert the lexeme (pointer + length, NOT null-terminated)
             * to an int64. We copy to a temp buffer for strtoll. */
            char buf[32];
            int  len = t.length < 31 ? t.length : 31;
            memcpy(buf, t.start, len);
            buf[len] = '\0';
            int64_t value = (int64_t)strtoll(buf, NULL, 10);
            return expr_int_lit(&p->arena, value, t.line);
        }

        /* ── Float literal ─────────────────────────────────────────────── */
        case TOK_FLOAT_LIT: {
            char buf[64];
            int  len = t.length < 63 ? t.length : 63;
            memcpy(buf, t.start, len);
            buf[len] = '\0';
            double value = strtod(buf, NULL);
            return expr_float_lit(&p->arena, value, t.line);
        }

        /* ── Char literal ──────────────────────────────────────────────── */
        case TOK_CHAR_LIT: {
            /* Lexeme is 'X' or '\n' etc. Extract the codepoint. */
            const char *s = t.start + 1; /* skip opening quote */
            uint32_t codepoint;
            if (*s == '\\') {
                s++;
                switch (*s) {
                    case 'n':  codepoint = '\n'; break;
                    case 't':  codepoint = '\t'; break;
                    case 'r':  codepoint = '\r'; break;
                    case '0':  codepoint = 0;     break;
                    case '\\': codepoint = '\\'; break;
                    case '\'': codepoint = '\'';  break;
                    default:   codepoint = (unsigned char)*s; break;
                }
            } else {
                codepoint = (unsigned char)*s;
            }
            return expr_char_lit(&p->arena, codepoint, t.line);
        }

        /* ── Bool literals ─────────────────────────────────────────────── */
        case TOK_TRUE:  return expr_bool_lit(&p->arena, true,  t.line);
        case TOK_FALSE: return expr_bool_lit(&p->arena, false, t.line);

        /* ── String literal ────────────────────────────────────────────── */
        case TOK_STRING_LIT:
            /* The lexeme includes the surrounding quotes. We trim them:
             * start+1 skips the opening quote, length-2 drops both quotes. */
            return expr_string_lit(&p->arena,
                                   t.start + 1, t.length - 2,
                                   t.line);

        /* ── Interpolated string: $"Hello, {name}!" ─────────────────────
         * After TOK_INTERP_BEGIN the lexer alternates between:
         *   TOK_INTERP_TEXT   — a literal text run
         *   TOK_INTERP_LBRACE — start of an embedded expression
         *     <normal expression tokens>
         *   TOK_INTERP_RBRACE — end of embedded expression
         * finishing with TOK_INTERP_END (the closing "). */
        case TOK_INTERP_BEGIN: {
            typedef struct InterpSegment ISeg;
            Expr *node = expr_interp_string(&p->arena, t.line);
            ISeg *tail = NULL;

            while (!check(p, TOK_INTERP_END) && !check(p, TOK_EOF)) {
                ISeg *seg = arena_alloc(&p->arena, sizeof(ISeg));
                seg->next = NULL;

                if (check(p, TOK_INTERP_TEXT)) {
                    Token tt = p->current;
                    advance(p);
                    seg->is_expr  = false;
                    seg->text     = tt.start;
                    seg->text_len = tt.length;
                    seg->expr     = NULL;
                } else if (check(p, TOK_INTERP_LBRACE)) {
                    advance(p); /* consume { */
                    seg->is_expr = true;
                    seg->text    = NULL;
                    seg->expr    = parse_expr(p, BP_NONE);
                    consume(p, TOK_INTERP_RBRACE,
                            "Expected '}' to close interpolated expression");
                } else {
                    /* Unexpected token inside interpolated string */
                    if (!p->panic_mode && p->error_count < PARSER_MAX_ERRORS) {
                        ParseError *e = &p->errors[p->error_count++];
                        snprintf(e->message, sizeof(e->message),
                            "Unexpected token inside interpolated string (got '%s')",
                            token_type_name(p->current.type));
                        e->line      = p->current.line;
                        p->had_error  = true;
                        p->panic_mode = true;
                    }
                    advance(p);
                    continue;
                }

                /* Append segment to list */
                if (!node->interp_string.segments)
                    node->interp_string.segments = tail = seg;
                else { tail->next = seg; tail = seg; }
                node->interp_string.segment_count++;
            }

            consume(p, TOK_INTERP_END,
                    "Expected closing '\"' for interpolated string");
            return node;
        }

        /* ── Identifier or function call ───────────────────────────────── */
        case TOK_IDENT: {
            /* Peek at the NEXT token (current, since we already advanced).
             * Check for generic call: foo<int>(x)  or  foo<Tag>(x).
             *
             * Disambiguation: foo<int>(x)  vs  a < b
             * In the Pratt parser, comparison '<' is an INFIX operator
             * handled by parse_infix, NOT parse_prefix. parse_prefix only
             * sees '<' here when it immediately follows the callee identifier
             * (the left side is just the ident we just consumed).
             * Therefore it is safe to attempt type-arg parsing here: if the
             * '<' is a comparison, parse_expr's infix loop will have consumed
             * this ident and returned already before we get here.
             *
             * We check that after '<' the token looks like a type start to
             * avoid false positives from operators like '<=' or weird tokens. */
            TypeArgNode *call_type_args      = NULL;
            int          call_type_arg_count = 0;

            if (check(p, TOK_LT)) {
                TokenType after_lt   = p->next.type;
                TokenType after_type = p->peek.type;
                /* A type keyword or identifier after '<' is necessary but not
                 * sufficient — we also require that after the type comes:
                 *   '>'   — closes single-arg list: foo<int>(x)
                 *   ','   — separates multi-arg list: foo<int,string>(x)
                 * This prevents `a < b` from being parsed as a generic call
                 * when 'b' is followed by an operator or ';'. */
                bool looks_like_type =
                    after_lt == TOK_INT    || after_lt == TOK_FLOAT  ||
                    after_lt == TOK_BOOL   || after_lt == TOK_STRING ||
                    after_lt == TOK_VOID   || after_lt == TOK_SBYTE  ||
                    after_lt == TOK_BYTE   || after_lt == TOK_SHORT  ||
                    after_lt == TOK_USHORT || after_lt == TOK_UINT   ||
                    after_lt == TOK_LONG   || after_lt == TOK_ULONG  ||
                    after_lt == TOK_DOUBLE || after_lt == TOK_CHAR   ||
                    after_lt == TOK_IDENT;
                bool after_type_closes =
                    after_type == TOK_GT      ||  /* foo<T>( */
                    after_type == TOK_COMMA   ||  /* foo<T,U>( */
                    after_type == TOK_LBRACKET;   /* foo<T[]>( — array type arg */
                if (looks_like_type && after_type_closes) {
                    call_type_args = parse_type_arg_list(p, &call_type_arg_count);
                }
            }

            if (check(p, TOK_LPAREN)) {
                advance(p);  /* consume '(' */

                /* Parse argument list */
                ArgNode *args      = NULL;
                ArgNode *args_tail = NULL;
                int      arg_count = 0;

                if (!check(p, TOK_RPAREN)) {
                    do {
                        Expr *arg = parse_expr(p, BP_NONE);
                        /* Build the linked list in order.
                         * We track the tail to avoid O(n²) appending. */
                        ArgNode *node = arg_node(&p->arena, arg, NULL);
                        if (!args) {
                            args = args_tail = node;
                        } else {
                            args_tail->next = node;
                            args_tail       = node;
                        }
                        arg_count++;
                    } while (match(p, TOK_COMMA));
                }
                consume(p, TOK_RPAREN, "Expected ')' after arguments");

                Expr *call_e = expr_call(&p->arena,
                                 t.start, t.length,
                                 args, arg_count,
                                 t.line);
                call_e->call.type_args      = call_type_args;
                call_e->call.type_arg_count = call_type_arg_count;
                return call_e;
            }

            /* If we parsed type args but no '(' follows, that's an error */
            if (call_type_arg_count > 0) {
                if (!p->panic_mode && p->error_count < PARSER_MAX_ERRORS) {
                    ParseError *e = &p->errors[p->error_count++];
                    snprintf(e->message, sizeof(e->message),
                             "Expected '(' after generic type arguments");
                    e->line      = p->current.line;
                    p->had_error = true;
                }
            }

            /* Plain variable reference */
            return expr_ident(&p->arena, t.start, t.length, t.line);
        }

        /* ── Unary operators ───────────────────────────────────────────── */
        case TOK_MINUS:
        case TOK_BANG: {
            /* Unary operators are RIGHT-associative prefix operators.
             * We recurse with BP_UNARY so that !!x parses as !(!x). */
            Expr *operand = parse_expr(p, BP_UNARY);
            return expr_unary(&p->arena, t.type, operand, t.line);
        }

        /* ── Prefix ++ / -- ────────────────────────────────────────────── */
        case TOK_PLUS_PLUS:
        case TOK_MINUS_MINUS: {
            /* Parse the lvalue (variable or field access).
             * Use BP_CALL so that member access (.) binds before we return. */
            Expr *operand = parse_expr(p, BP_CALL);
            if (operand->kind == EXPR_IDENT) {
                return expr_prefix(&p->arena, t.type,
                                   operand->ident.name, operand->ident.length, t.line);
            }
            if (operand->kind == EXPR_FIELD_GET) {
                return expr_postfix_field(&p->arena, t.type, true,
                                          operand->field_get.object,
                                          operand->field_get.field_name,
                                          operand->field_get.field_name_len,
                                          t.line);
            }
            if (!p->panic_mode && p->error_count < PARSER_MAX_ERRORS) {
                ParseError *e = &p->errors[p->error_count++];
                snprintf(e->message, sizeof(e->message),
                         "'%s' requires a variable or field as its operand",
                         t.type == TOK_PLUS_PLUS ? "++" : "--");
                e->line       = t.line;
                p->had_error  = true;
                p->panic_mode = true;
            }
            return operand;
        }

        /* ── Grouped expression: (expr) ────────────────────────────────── */
        case TOK_LPAREN: {
            Expr *inner = parse_expr(p, BP_NONE);
            consume(p, TOK_RPAREN, "Expected ')' after expression");
            return inner;
        }

        /* ── this ──────────────────────────────────────────────────────── */
        case TOK_THIS:
            return expr_this(&p->arena, t.line);

        /* ── super(args) ─────────────────────────────────────────────── */
        case TOK_SUPER: {
            consume(p, TOK_LPAREN, "Expected '(' after 'super'");
            ArgNode *args      = NULL;
            ArgNode *args_tail = NULL;
            int      arg_count = 0;
            if (!check(p, TOK_RPAREN)) {
                do {
                    Expr    *arg  = parse_expr(p, BP_NONE);
                    ArgNode *node = arg_node(&p->arena, arg, NULL);
                    if (!args) { args = args_tail = node; }
                    else       { args_tail->next = node; args_tail = node; }
                    arg_count++;
                } while (match(p, TOK_COMMA));
            }
            consume(p, TOK_RPAREN, "Expected ')' after super arguments");
            return expr_super_call(&p->arena, args, arg_count, t.line);
        }

        /* ── new ClassName(args) ────────────────────────────────────────── */
        case TOK_NEW: {
            /* new ElementType[length]  — array allocation */
            TokenType ct = p->current.type;
            bool is_type_kw = (ct==TOK_INT||ct==TOK_FLOAT||ct==TOK_BOOL||
                               ct==TOK_STRING||ct==TOK_SBYTE||ct==TOK_BYTE||
                               ct==TOK_SHORT||ct==TOK_USHORT||ct==TOK_UINT||
                               ct==TOK_LONG||ct==TOK_ULONG||ct==TOK_DOUBLE||
                               ct==TOK_CHAR||ct==TOK_IDENT);
            /* type keyword followed by '[' — array allocation */
            if (is_type_kw && ct != TOK_IDENT && p->next.type == TOK_LBRACKET) {
                Type elem = parse_base_type(p);
                consume(p, TOK_LBRACKET, "Expected '[' after element type");
                if (check(p, TOK_RBRACKET)) {
                    /* new Type[] { e0, e1, ... } — initializer list */
                    advance(p); /* consume ']' */
                    consume(p, TOK_LBRACE, "Expected '{' after 'new Type[]'");
                    ArgNode *head = NULL, *tail = NULL;
                    int count = 0;
                    if (!check(p, TOK_RBRACE)) {
                        do {
                            Expr    *elem_expr = parse_expr(p, BP_NONE);
                            ArgNode *node      = arg_node(&p->arena, elem_expr, NULL);
                            if (!head) { head = tail = node; }
                            else       { tail->next = node; tail = node; }
                            count++;
                        } while (match(p, TOK_COMMA));
                    }
                    consume(p, TOK_RBRACE, "Expected '}' to close array initializer");
                    return expr_array_lit(&p->arena, head, count, t.line);
                }
                /* new Type[n] — fixed-size allocation */
                Expr *len = parse_expr(p, BP_NONE);
                consume(p, TOK_RBRACKET, "Expected ']' after array length");
                return expr_new_array(&p->arena, elem, len, t.line);
            }
            /* Also handle: new ClassName[n] for object arrays */
            if (ct == TOK_IDENT && p->next.type == TOK_LBRACKET) {
                Type elem = parse_base_type(p);
                consume(p, TOK_LBRACKET, "Expected '[' after element type");
                if (check(p, TOK_RBRACKET)) {
                    /* new Type[] { e0, e1, ... } — initializer list */
                    advance(p); /* consume ']' */
                    consume(p, TOK_LBRACE, "Expected '{' after 'new Type[]'");
                    ArgNode *head = NULL, *tail = NULL;
                    int count = 0;
                    if (!check(p, TOK_RBRACE)) {
                        do {
                            Expr    *elem_expr = parse_expr(p, BP_NONE);
                            ArgNode *node      = arg_node(&p->arena, elem_expr, NULL);
                            if (!head) { head = tail = node; }
                            else       { tail->next = node; tail = node; }
                            count++;
                        } while (match(p, TOK_COMMA));
                    }
                    consume(p, TOK_RBRACE, "Expected '}' to close array initializer");
                    return expr_array_lit(&p->arena, head, count, t.line);
                }
                /* new Type[n] — fixed-size allocation */
                Expr *len = parse_expr(p, BP_NONE);
                consume(p, TOK_RBRACKET, "Expected ']' after array length");
                return expr_new_array(&p->arena, elem, len, t.line);
            }
            /* new ClassName(...) — object construction */
            /* new ClassName<T>(...) — generic object construction */
            Token class_tok = p->current;
            consume(p, TOK_IDENT, "Expected class name after 'new'");

            /* Optional type argument list: new Stack<int>() */
            TypeArgNode *type_args      = NULL;
            int          type_arg_count = 0;
            if (check(p, TOK_LT)) {
                type_args = parse_type_arg_list(p, &type_arg_count);
            }

            consume(p, TOK_LPAREN, "Expected '(' after class name");

            ArgNode *args      = NULL;
            ArgNode *args_tail = NULL;
            int      arg_count = 0;

            if (!check(p, TOK_RPAREN)) {
                do {
                    Expr    *arg  = parse_expr(p, BP_NONE);
                    ArgNode *node = arg_node(&p->arena, arg, NULL);
                    if (!args) { args = args_tail = node; }
                    else       { args_tail->next = node; args_tail = node; }
                    arg_count++;
                } while (match(p, TOK_COMMA));
            }
            consume(p, TOK_RPAREN, "Expected ')' after constructor arguments");

            Expr *new_e = expr_new(&p->arena,
                            class_tok.start, class_tok.length,
                            args, arg_count,
                            t.line);
            new_e->new_expr.type_args      = type_args;
            new_e->new_expr.type_arg_count = type_arg_count;
            return new_e;
        }

        /* Array literal: {e0, e1, e2} */
        case TOK_LBRACE: {
            ArgNode *head = NULL, *tail = NULL;
            int count = 0;
            if (!check(p, TOK_RBRACE)) {
                do {
                    Expr    *elem = parse_expr(p, BP_NONE);
                    ArgNode *node = arg_node(&p->arena, elem, NULL);
                    if (!head) { head = tail = node; }
                    else       { tail->next = node; tail = node; }
                    count++;
                } while (match(p, TOK_COMMA));
            }
            consume(p, TOK_RBRACE, "Expected '}' to close array literal");
            return expr_array_lit(&p->arena, head, count, t.line);
        }

        /* Array literal: [e0, e1, e2]  (bracket syntax — same semantics as braces) */
        case TOK_LBRACKET: {
            ArgNode *head = NULL, *tail = NULL;
            int count = 0;
            if (!check(p, TOK_RBRACKET)) {
                do {
                    Expr    *elem = parse_expr(p, BP_NONE);
                    ArgNode *node = arg_node(&p->arena, elem, NULL);
                    if (!head) { head = tail = node; }
                    else       { tail->next = node; tail = node; }
                    count++;
                } while (match(p, TOK_COMMA));
            }
            consume(p, TOK_RBRACKET, "Expected ']' to close array literal");
            return expr_array_lit(&p->arena, head, count, t.line);
        }

        default:
            if (!p->panic_mode && p->error_count < PARSER_MAX_ERRORS) {
                ParseError *e = &p->errors[p->error_count++];
                snprintf(e->message, sizeof(e->message),
                         "Expected an expression, got '%s'",
                         token_type_name(t.type));
                e->line       = t.line;
                p->had_error  = true;
                p->panic_mode = true;
            }
            /* Return a dummy node to let the parser continue */
            return expr_int_lit(&p->arena, 0, t.line);
        case TOK_TYPEOF: {
            /* typeof(expr) — parse the inner expression */
            consume(p, TOK_LPAREN, "Expected '(' after 'typeof'");
            Expr *operand = parse_expr(p, BP_NONE);
            consume(p, TOK_RPAREN, "Expected ')' after typeof expression");
            return expr_typeof(&p->arena, operand, t.line);
        }
    }
}

/*
 * left_binding_power — returns the binding power an operator presents
 * to its LEFT neighbor. Zero means "this token cannot be an infix operator."
 */
static int left_binding_power(TokenType type) {
    switch (type) {
        case TOK_ASSIGN: return BP_ASSIGN;
        case TOK_OR:     return BP_OR;
        case TOK_AND:    return BP_AND;
        case TOK_EQ:
        case TOK_NEQ:    return BP_EQUALITY;
        case TOK_LT: case TOK_LTE:
        case TOK_GT: case TOK_GTE:
        case TOK_IS: case TOK_AS:  return BP_COMPARISON;
        case TOK_PLUS:
        case TOK_MINUS:  return BP_TERM;
        case TOK_STAR:
        case TOK_SLASH:
        case TOK_PERCENT: return BP_FACTOR;
        case TOK_PLUS_PLUS:
        case TOK_MINUS_MINUS: return BP_UNARY; /* postfix binds tight */
        case TOK_DOT:         return BP_CALL + 1; /* member access binds tightest */
        case TOK_LBRACKET:    return BP_CALL + 1; /* index access same as member   */
        default:          return BP_NONE;  /* Not an infix operator */
    }
}

/*
 * parse_infix — given a left-hand expression and an operator token we've
 * already consumed, parse the right side and combine into a binary/assign node.
 */
static Expr *parse_infix(Parser *p, Expr *left, Token op) {
    int lbp = left_binding_power(op.type);

    /* Postfix ++ / -- — no right operand, left must be a variable or field */
    if (op.type == TOK_PLUS_PLUS || op.type == TOK_MINUS_MINUS) {
        if (left->kind == EXPR_IDENT) {
            return expr_postfix(&p->arena, op.type,
                                left->ident.name, left->ident.length, op.line);
        }
        if (left->kind == EXPR_FIELD_GET) {
            return expr_postfix_field(&p->arena, op.type, false,
                                      left->field_get.object,
                                      left->field_get.field_name,
                                      left->field_get.field_name_len,
                                      op.line);
        }
        if (!p->panic_mode && p->error_count < PARSER_MAX_ERRORS) {
            ParseError *e = &p->errors[p->error_count++];
            snprintf(e->message, sizeof(e->message),
                     "'%s' requires a variable or field as its operand",
                     op.type == TOK_PLUS_PLUS ? "++" : "--");
            e->line       = op.line;
            p->had_error  = true;
            p->panic_mode = true;
        }
        return left;
    }

    /* Array index: arr[i] read or arr[i] = v write */
    if (op.type == TOK_LBRACKET) {
        Expr *index = parse_expr(p, BP_NONE);
        consume(p, TOK_RBRACKET, "Expected ']' after index expression");

        /* Check for assignment: arr[i] = value */
        if (check(p, TOK_ASSIGN)) {
            advance(p); /* consume '=' */
            Expr *value = parse_expr(p, BP_NONE);
            return expr_index_assign(&p->arena, left, index, value, op.line);
        }
        return expr_index(&p->arena, left, index, op.line);
    }

    /* Member access: obj.field, obj.method(args), obj.field = value */
    if (op.type == TOK_DOT) {
        Token member = p->current;
        consume(p, TOK_IDENT, "Expected field or method name after '.'");

        if (check(p, TOK_LPAREN)) {
            /* Method call: obj.method(args) */
            advance(p); /* consume '(' */
            ArgNode *args      = NULL;
            ArgNode *args_tail = NULL;
            int      arg_count = 0;
            if (!check(p, TOK_RPAREN)) {
                do {
                    Expr    *arg  = parse_expr(p, BP_NONE);
                    ArgNode *node = arg_node(&p->arena, arg, NULL);
                    if (!args) { args = args_tail = node; }
                    else       { args_tail->next = node; args_tail = node; }
                    arg_count++;
                } while (match(p, TOK_COMMA));
            }
            consume(p, TOK_RPAREN, "Expected ')' after method arguments");
            return expr_method_call(&p->arena, left,
                                    member.start, member.length,
                                    args, arg_count, op.line);
        }

        if (check(p, TOK_ASSIGN)) {
            /* Field assignment: obj.field = value */
            advance(p); /* consume '=' */
            Expr *value = parse_expr(p, BP_ASSIGN);
            return expr_field_set(&p->arena, left,
                                  member.start, member.length,
                                  value, op.line);
        }

        /* Plain field access: obj.field */
        return expr_field_get(&p->arena, left,
                              member.start, member.length, op.line);
    }

    if (op.type == TOK_ASSIGN) {
        /* Assignment: right-associative.
         * The left side must be an identifier (lvalue).
         * We pass lbp (not lbp+1) so that  a = b = 5  parses as  a = (b = 5). */
        if (left->kind != EXPR_IDENT) {
            if (!p->panic_mode && p->error_count < PARSER_MAX_ERRORS) {
                ParseError *e = &p->errors[p->error_count++];
                snprintf(e->message, sizeof(e->message),
                         "Left side of '=' must be a variable name");
                e->line       = op.line;
                p->had_error  = true;
                p->panic_mode = true;
            }
        }
        Expr *right = parse_expr(p, lbp); /* right-associative: same bp */
        return expr_assign(&p->arena,
                           left->ident.name, left->ident.length,
                           right, op.line);
    }

    /* is / as — right side is a type name, not an expression */
    if (op.type == TOK_IS || op.type == TOK_AS) {
        Type target = parse_type(p);
        if (op.type == TOK_IS)
            return expr_is(&p->arena, left, target, op.line);
        else
            return expr_as(&p->arena, left, target, op.line);
    }

    /* All other binary operators: left-associative (pass lbp + 1) */
    Expr *right = parse_expr(p, lbp + 1);
    return expr_binary(&p->arena, op.type, left, right, op.line);
}

/*
 * parse_expr — the Pratt parsing loop.
 *
 * min_bp is the minimum binding power the NEXT infix operator must have
 * in order for us to keep consuming. When a caller passes a high min_bp,
 * we stop sooner (tighter precedence). When it passes BP_NONE (0), we
 * consume as much as possible.
 */
static Expr *parse_expr(Parser *p, int min_bp) {
    Expr *left = parse_prefix(p);

    for (;;) {
        TokenType op_type = p->current.type;
        int       lbp     = left_binding_power(op_type);

        /* If this token's binding power is too low, it belongs to the
         * enclosing expression — stop here and return what we have. */
        if (lbp <= min_bp) break;

        Token op = p->current;
        advance(p);  /* consume the infix operator */

        left = parse_infix(p, left, op);
    }

    return left;
}


/* ─────────────────────────────────────────────────────────────────────────────
 * STATEMENT PARSERS
 *
 * Forward declaration — parse_stmt is mutually recursive with parse_block.
 * ───────────────────────────────────────────────────────────────────────────*/
static Stmt *parse_stmt(Parser *p);

/*
 * parse_block — parses { stmt* }
 * Returns a STMT_BLOCK node.
 */
static Stmt *parse_block(Parser *p) {
    int line = p->current.line;
    consume(p, TOK_LBRACE, "Expected '{'");

    StmtNode *head = NULL;
    StmtNode *tail = NULL;

    /* Parse statements until we see '}' or EOF */
    while (!check(p, TOK_RBRACE) && !check(p, TOK_EOF)) {
        Stmt *s    = parse_stmt(p);
        StmtNode *n = stmt_node(&p->arena, s, NULL);
        if (!head) { head = tail = n; }
        else       { tail->next = n; tail = n; }
    }

    consume(p, TOK_RBRACE, "Expected '}' after block");
    return stmt_block(&p->arena, head, line);
}

/*
 * parse_var_decl — parses:  TYPE IDENT = expr ;
 *                  or:      TYPE IDENT ;        (uninitialized — allowed)
 *
 * Precondition: the type token has already been parsed and is in `type`.
 */
static Stmt *parse_var_decl(Parser *p, Type type, int line) {
    /* Identifier */
    Token name = p->current;
    consume(p, TOK_IDENT, "Expected variable name after type");

    /* Optional initializer */
    Expr *init = NULL;
    if (match(p, TOK_ASSIGN)) {
        init = parse_expr(p, BP_NONE);
    }

    consume(p, TOK_SEMICOLON, "Expected ';' after variable declaration");
    return stmt_var_decl(&p->arena, type, name.start, name.length, init, line);
}

/*
 * parse_type_param_list — parses a generic type parameter list:
 *   <T>
 *   <T, K>
 *   <T where T : IFoo>
 *   <K, V where V : IComparable>
 *
 * Precondition: current token is '<'.
 * Returns a linked list of TypeParamNode, sets *out_count.
 */
static TypeParamNode *parse_type_param_list(Parser *p, int *out_count) {
    advance(p); /* consume '<' */

    TypeParamNode *head = NULL, *tail = NULL;
    int count = 0;

    do {
        Token name_tok = p->current;
        consume(p, TOK_IDENT, "Expected type parameter name");

        TypeParamNode *tp = arena_alloc(&p->arena, sizeof(TypeParamNode));
        tp->name           = name_tok.start;
        tp->length         = name_tok.length;
        tp->constraint     = NULL;
        tp->constraint_len = 0;
        tp->next           = NULL;

        /* Optional constraint: where T : IFoo
         * We check for 'where' keyword after the parameter name. */
        if (p->current.type == TOK_WHERE) {
            advance(p); /* consume 'where' */
            /* Expect: T : IFoo — the param name again, then ':', then constraint */
            consume(p, TOK_IDENT, "Expected type parameter name after 'where'");
            consume(p, TOK_COLON, "Expected ':' after type parameter name in 'where' clause");
            Token constraint_tok = p->current;
            consume(p, TOK_IDENT, "Expected constraint type name after ':'");
            tp->constraint     = constraint_tok.start;
            tp->constraint_len = constraint_tok.length;
        }

        if (!head) { head = tail = tp; }
        else       { tail->next = tp; tail = tp; }
        count++;
    } while (match(p, TOK_COMMA));

    consume(p, TOK_GT, "Expected '>' to close type parameter list");
    *out_count = count;
    return head;
}

/*
 * parse_type_arg_list — parses a generic type argument list at an instantiation site:
 *   <int>
 *   <string, int>
 *   <MyClass>
 *
 * Precondition: current token is '<'.
 * Returns a linked list of TypeArgNode, sets *out_count.
 */
static TypeArgNode *parse_type_arg_list(Parser *p, int *out_count) {
    advance(p); /* consume '<' */

    TypeArgNode *head = NULL, *tail = NULL;
    int count = 0;

    do {
        Type arg_type = parse_type(p);
        TypeArgNode *ta = arena_alloc(&p->arena, sizeof(TypeArgNode));
        ta->type = arg_type;
        ta->next = NULL;
        if (!head) { head = tail = ta; }
        else       { tail->next = ta; tail = ta; }
        count++;
    } while (match(p, TOK_COMMA));

    consume(p, TOK_GT, "Expected '>' to close type argument list");
    *out_count = count;
    return head;
}

/*
 * parse_fn_decl — parses:  fn TYPE IDENT ( params ) block
 *
 * Examples:
 *   fn void main() { }
 *   fn int add(int a, int b) { return a + b; }
 *
 * Precondition: 'fn' has already been consumed.
 */
static Stmt *parse_fn_decl(Parser *p, int line) {
    /* Function name */
    Token name = p->current;
    consume(p, TOK_IDENT, "Expected function name");

    /* Optional generic type parameter list: function foo<T>(T x): T { } */
    TypeParamNode *type_params      = NULL;
    int            type_param_count = 0;
    if (check(p, TOK_LT)) {
        type_params = parse_type_param_list(p, &type_param_count);
    }

    /* Parameter list */
    consume(p, TOK_LPAREN, "Expected '(' after function name");

    ParamNode *params      = NULL;
    ParamNode *params_tail = NULL;
    int        param_count = 0;

    if (!check(p, TOK_RPAREN)) {
        bool saw_default = false;
        do {
            /* Each parameter is:  TYPE IDENT [= expr] */
            Type  param_type = parse_type(p);
            Token param_name = p->current;
            consume(p, TOK_IDENT, "Expected parameter name");

            /* Optional default value */
            Expr *default_val = NULL;
            if (match(p, TOK_ASSIGN)) {
                default_val = parse_expr(p, BP_NONE);
                saw_default = true;
            } else if (saw_default) {
                /* Required param after a defaulted one */
                if (!p->panic_mode && p->error_count < PARSER_MAX_ERRORS) {
                    ParseError *e = &p->errors[p->error_count++];
                    snprintf(e->message, sizeof(e->message),
                             "Required parameter '%.*s' cannot follow a parameter with a default value",
                             param_name.length, param_name.start);
                    e->line      = param_name.line;
                    p->had_error = true;
                }
            }

            ParamNode *pn = param_node(&p->arena,
                                       param_type,
                                       param_name.start, param_name.length,
                                       NULL);
            pn->default_value = default_val;
            if (!params) { params = params_tail = pn; }
            else         { params_tail->next = pn; params_tail = pn; }
            param_count++;
        } while (match(p, TOK_COMMA));
    }

    consume(p, TOK_RPAREN, "Expected ')' after parameters");
    consume(p, TOK_COLON, "Expected ':' after paren");

    /* Return type */
    Type ret_type = parse_type(p);

    /* Body */
    Stmt *body = parse_block(p);

    Stmt *fn = stmt_fn_decl(&p->arena,
                        ret_type,
                        name.start, name.length,
                        params, param_count,
                        body, line);
    fn->fn_decl.type_params      = type_params;
    fn->fn_decl.type_param_count = type_param_count;
    return fn;
}

/*
 * parse_class_decl — parses a full class declaration:
 *
 *   class Greeter {
 *       public:
 *           function greet(): void { ... }
 *       private:
 *           string message;
 *   }
 *
 * Access is controlled by section labels (public:, private:, protected:).
 * Members inherit the access level of the most recent label.
 * Members declared before any label default to ACCESS_PUBLIC.
 * The 'static' modifier may still appear per-member within any section.
 *
 * Precondition: 'class' has already been consumed.
 */
static Stmt *parse_class_decl(Parser *p, int line) {
    Token class_name = p->current;
    consume(p, TOK_IDENT, "Expected class name after 'class'");

    /* Optional generic type parameter list: class Stack<T> { } */
    TypeParamNode *type_params      = NULL;
    int            type_param_count = 0;
    if (check(p, TOK_LT)) {
        type_params = parse_type_param_list(p, &type_param_count);
    }

    /* Optional parent class and/or interfaces:
     *   class Dog : Animal
     *   class MyMod : IRunnable
     *   class MyMod : Animal, IRunnable, ITickable
     *
     * The checker distinguishes a parent class from interfaces by looking
     * up the name — SYM_CLASS → parent, SYM_INTERFACE → implements.
     * At parse time we just collect all names after the colon. */
    const char *parent_name = NULL;
    int         parent_len  = 0;

    /* Linked list of interface names — built forwards with a tail pointer */
    typedef struct IfaceNameNode IFNode;
    IFNode *ifaces      = NULL;
    IFNode *ifaces_tail = NULL;
    int     iface_count = 0;

    if (match(p, TOK_COLON)) {
        do {
            Token name_tok = p->current;
            consume(p, TOK_IDENT, "Expected class or interface name after ':'");
            /* We stash ALL names; the checker will sort parent vs interface */
            IFNode *inode = arena_alloc(&p->arena, sizeof(IFNode));
            inode->name   = name_tok.start;
            inode->length = name_tok.length;
            inode->next   = NULL;
            if (!ifaces) { ifaces = ifaces_tail = inode; }
            else         { ifaces_tail->next = inode; ifaces_tail = inode; }
            iface_count++;
        } while (match(p, TOK_COMMA));
    }

    Stmt *cls = stmt_class_decl(&p->arena,
                                class_name.start, class_name.length,
                                parent_name, parent_len,
                                line);
    /* Stash the raw name list; checker_check() will classify each as
     * parent class or interface and validate accordingly. */
    cls->class_decl.interfaces      = ifaces;
    cls->class_decl.interface_count = iface_count;
    cls->class_decl.type_params      = type_params;
    cls->class_decl.type_param_count = type_param_count;

    consume(p, TOK_LBRACE, "Expected '{' to begin class body");

    /* Current access level — defaults to public before any label appears. */
    AccessLevel current_access = ACCESS_PUBLIC;

    while (!check(p, TOK_RBRACE) && !check(p, TOK_EOF)) {
        int member_line = p->current.line;

        /* ── Section label: public: / private: / protected: ────────────
         * Detected by seeing one of these keywords followed by TOK_COLON.
         * We use the one-token lookahead (p->next) to distinguish a label
         * from a per-member modifier in front of a type name.            */
        if ((p->current.type == TOK_PUBLIC ||
             p->current.type == TOK_PRIVATE ||
             p->current.type == TOK_PROTECTED) &&
            p->next.type == TOK_COLON)
        {
            if      (p->current.type == TOK_PUBLIC)    current_access = ACCESS_PUBLIC;
            else if (p->current.type == TOK_PRIVATE)   current_access = ACCESS_PRIVATE;
            else                                        current_access = ACCESS_PROTECTED;
            advance(p);  /* consume the keyword */
            advance(p);  /* consume the ':' */
            continue;
        }

        /* ── Per-member 'static' modifier (allowed inside any section) ─ */
        bool is_static = match(p, TOK_STATIC);

        /* ── Constructor: ClassName(params) { body } ───────────────────
         * Recognised by: current token is an identifier matching the class
         * name, followed immediately by '('.                             */
        bool is_ctor = (p->current.type == TOK_IDENT &&
                        p->current.length == class_name.length &&
                        memcmp(p->current.start, class_name.start, class_name.length) == 0 &&
                        p->next.type == TOK_LPAREN);

        if (is_ctor) {
            advance(p);  /* consume class name token */
            consume(p, TOK_LPAREN, "Expected '(' after constructor name");

            ParamNode *params      = NULL;
            ParamNode *params_tail = NULL;
            int        param_count = 0;

            if (!check(p, TOK_RPAREN)) {
                bool saw_default_ctor = false;
                do {
                    Type  param_type = parse_type(p);
                    Token param_name = p->current;
                    consume(p, TOK_IDENT, "Expected parameter name");

                    Expr *default_val = NULL;
                    if (match(p, TOK_ASSIGN)) {
                        default_val = parse_expr(p, BP_NONE);
                        saw_default_ctor = true;
                    } else if (saw_default_ctor) {
                        if (!p->panic_mode && p->error_count < PARSER_MAX_ERRORS) {
                            ParseError *e = &p->errors[p->error_count++];
                            snprintf(e->message, sizeof(e->message),
                                     "Required parameter '%.*s' cannot follow a parameter with a default value",
                                     param_name.length, param_name.start);
                            e->line      = param_name.line;
                            p->had_error = true;
                        }
                    }
                    ParamNode *pn = param_node(&p->arena, param_type,
                                               param_name.start, param_name.length, NULL);
                    pn->default_value = default_val;
                    if (!params) { params = params_tail = pn; }
                    else         { params_tail->next = pn; params_tail = pn; }
                    param_count++;
                } while (match(p, TOK_COMMA));
            }
            consume(p, TOK_RPAREN, "Expected ')' after constructor parameters");

            Stmt *body = parse_block(p);
            Stmt *fn   = stmt_fn_decl(&p->arena, type_void(),
                                      class_name.start, class_name.length,
                                      params, param_count, body, member_line);

            typedef struct ClassMethodNode CMNode;
            CMNode *mn         = arena_alloc(&p->arena, sizeof(CMNode));
            mn->fn             = fn;
            mn->is_static      = false;
            mn->access         = current_access;
            mn->is_constructor = true;
            mn->next           = NULL;
            if (!cls->class_decl.methods) {
                cls->class_decl.methods = mn;
            } else {
                CMNode *tail = cls->class_decl.methods;
                while (tail->next) tail = tail->next;
                tail->next = mn;
            }
            cls->class_decl.method_count++;
            continue;
        }

        /* ── Method: function name(params): ReturnType { body } ────── */
        if (match(p, TOK_FN)) {
            Stmt *fn = parse_fn_decl(p, member_line);

            typedef struct ClassMethodNode CMNode;
            CMNode *mn         = arena_alloc(&p->arena, sizeof(CMNode));
            mn->fn             = fn;
            mn->is_static      = is_static;
            mn->access         = current_access;
            mn->is_constructor = false;
            mn->next           = NULL;
            if (!cls->class_decl.methods) {
                cls->class_decl.methods = mn;
            } else {
                CMNode *tail = cls->class_decl.methods;
                while (tail->next) tail = tail->next;
                tail->next = mn;
            }
            cls->class_decl.method_count++;
            continue;
        }

        /* ── Field declaration: Type name; ─────────────────────────── */
        {
            TokenType ct = p->current.type;
            if (ct == TOK_INT || ct == TOK_FLOAT || ct == TOK_BOOL ||
                ct == TOK_STRING || ct == TOK_IDENT ||
                ct == TOK_SBYTE || ct == TOK_BYTE || ct == TOK_SHORT ||
                ct == TOK_USHORT || ct == TOK_UINT || ct == TOK_LONG ||
                ct == TOK_ULONG || ct == TOK_DOUBLE || ct == TOK_CHAR)
            {
                Type  field_type = parse_type(p);
                Token field_name = p->current;
                consume(p, TOK_IDENT, "Expected field name");

                /* Optional field initializer: int x = 42; */
                Expr *initializer = NULL;
                if (match(p, TOK_ASSIGN)) {
                    initializer = parse_expr(p, BP_NONE);
                }
                consume(p, TOK_SEMICOLON, "Expected ';' after field declaration");

                typedef struct ClassFieldNode CFNode;
                CFNode *fn        = arena_alloc(&p->arena, sizeof(CFNode));
                fn->type          = field_type;
                fn->name          = field_name.start;
                fn->length        = field_name.length;
                fn->is_static     = is_static;
                fn->access        = current_access;
                fn->initializer   = initializer;
                fn->next          = NULL;
                if (!cls->class_decl.fields) {
                    cls->class_decl.fields = fn;
                } else {
                    CFNode *tail = cls->class_decl.fields;
                    while (tail->next) tail = tail->next;
                    tail->next = fn;
                }
                cls->class_decl.field_count++;
                continue;
            }
        }

        /* ── Unknown token in class body ────────────────────────────── */
        if (!p->panic_mode && p->error_count < PARSER_MAX_ERRORS) {
            ParseError *e = &p->errors[p->error_count++];
            snprintf(e->message, sizeof(e->message),
                     "Unexpected token '%s' in class body",
                     token_type_name(p->current.type));
            e->line       = member_line;
            p->had_error  = true;
            p->panic_mode = true;
        }
        advance(p);
    }

    consume(p, TOK_RBRACE, "Expected '}' to end class body");
    return cls;
}
/*
 * parse_interface_decl — parses:
 *
 *   interface IRunnable {
 *       function run(): void;
 *       function tick(int delta): void;
 *   }
 *
 * Only method signatures are allowed (no bodies, no fields).
 * Precondition: 'interface' has already been consumed.
 */
static Stmt *parse_interface_decl(Parser *p, int line) {
    Token iface_name = p->current;
    consume(p, TOK_IDENT, "Expected interface name after 'interface'");

    Stmt *iface = stmt_interface_decl(&p->arena,
                                      iface_name.start, iface_name.length,
                                      line);

    /* Optional generic type parameter list: interface IContainer<T> { } */
    if (check(p, TOK_LT)) {
        int type_param_count = 0;
        TypeParamNode *type_params = parse_type_param_list(p, &type_param_count);
        iface->interface_decl.type_params      = type_params;
        iface->interface_decl.type_param_count = type_param_count;
    }

    /* Optional parent interface: interface IChild : IBase */
    if (match(p, TOK_COLON)) {
        Token parent = p->current;
        consume(p, TOK_IDENT, "Expected parent interface name after ':'");
        iface->interface_decl.parent_name   = parent.start;
        iface->interface_decl.parent_length = parent.length;
    }

    consume(p, TOK_LBRACE, "Expected '{' to begin interface body");

    typedef struct IfaceMethodNode IMNode;
    IMNode *methods_tail = NULL;

    while (!check(p, TOK_RBRACE) && !check(p, TOK_EOF)) {
        int sig_line = p->current.line;
        consume(p, TOK_FN, "Expected 'function' in interface body");

        Token mname = p->current;
        consume(p, TOK_IDENT, "Expected method name");

        consume(p, TOK_LPAREN, "Expected '(' after method name");

        ParamNode *params      = NULL;
        ParamNode *params_tail = NULL;
        int        param_count = 0;

        if (!check(p, TOK_RPAREN)) {
            do {
                Type  ptype = parse_type(p);
                Token pname = p->current;
                consume(p, TOK_IDENT, "Expected parameter name");
                ParamNode *pn = param_node(&p->arena, ptype,
                                           pname.start, pname.length, NULL);
                if (!params) { params = params_tail = pn; }
                else         { params_tail->next = pn; params_tail = pn; }
                param_count++;
            } while (match(p, TOK_COMMA));
        }
        consume(p, TOK_RPAREN, "Expected ')' after parameters");
        consume(p, TOK_COLON,  "Expected ':' before return type");
        Type ret = parse_type(p);
        consume(p, TOK_SEMICOLON, "Expected ';' after interface method signature");

        IMNode *mn      = arena_alloc(&p->arena, sizeof(IMNode));
        mn->name        = mname.start;
        mn->length      = mname.length;
        mn->return_type = ret;
        mn->params      = params;
        mn->param_count = param_count;
        mn->next        = NULL;

        if (!iface->interface_decl.methods) {
            iface->interface_decl.methods = mn;
            methods_tail = mn;
        } else {
            methods_tail->next = mn;
            methods_tail = mn;
        }
        iface->interface_decl.method_count++;
        (void)sig_line;
    }

    consume(p, TOK_RBRACE, "Expected '}' to end interface body");
    return iface;
}

/*
 * parse_if_stmt — parses:  if ( expr ) block [ else block ]
 * Precondition: 'if' has already been consumed.
 */
static Stmt *parse_if_stmt(Parser *p, int line) {
    consume(p, TOK_LPAREN, "Expected '(' after 'if'");
    Expr *cond = parse_expr(p, BP_NONE);
    consume(p, TOK_RPAREN, "Expected ')' after if condition");

    Stmt *then_branch = parse_block(p);

    Stmt *else_branch = NULL;
    if (match(p, TOK_ELSE)) {
        /* else if — handle directly to avoid creating a nested structure */
        if (check(p, TOK_IF)) {
            int else_line = p->current.line;
            advance(p); /* consume 'if' */
            else_branch = parse_if_stmt(p, else_line);
        } else {
            else_branch = parse_block(p);
        }
    }

    return stmt_if(&p->arena, cond, then_branch, else_branch, line);
}

/*
 * parse_while_stmt — parses:  while ( expr ) block
 * Precondition: 'while' has already been consumed.
 */
static Stmt *parse_while_stmt(Parser *p, int line) {
    consume(p, TOK_LPAREN, "Expected '(' after 'while'");
    Expr *cond = parse_expr(p, BP_NONE);
    consume(p, TOK_RPAREN, "Expected ')' after while condition");
    Stmt *body = parse_block(p);
    return stmt_while(&p->arena, cond, body, line);
}

/*
 * parse_for_stmt — parses:  for ( init ; cond ; step ) block
 *
 * The init part can be:
 *   - A variable declaration: int i = 0
 *   - An expression:          i = 0
 *   - Empty:                  (nothing)
 *
 * Precondition: 'for' has already been consumed.
 */
static Stmt *parse_foreach_stmt(Parser *p, int line);

static Stmt *parse_for_stmt(Parser *p, int line) {
    consume(p, TOK_LPAREN, "Expected '(' after 'for'");

    /* Init — optional */
    Stmt *init = NULL;
    if (!check(p, TOK_SEMICOLON)) {
        /* Check if it's a type keyword (variable declaration) */
        TokenType ct = p->current.type;
        if (ct == TOK_INT || ct == TOK_FLOAT ||
            ct == TOK_BOOL || ct == TOK_STRING) {
            int   init_line = p->current.line;
            Type  init_type = parse_type(p);
            init = parse_var_decl(p, init_type, init_line);
            /* parse_var_decl already consumes the semicolon */
        } else {
            Expr *init_expr = parse_expr(p, BP_NONE);
            consume(p, TOK_SEMICOLON, "Expected ';' after for-init");
            init = stmt_expr(&p->arena, init_expr, line);
        }
    } else {
        advance(p); /* consume the ';' for empty init */
    }

    /* Condition — optional */
    Expr *cond = NULL;
    if (!check(p, TOK_SEMICOLON)) {
        cond = parse_expr(p, BP_NONE);
    }
    consume(p, TOK_SEMICOLON, "Expected ';' after for condition");

    /* Step — optional */
    Expr *step = NULL;
    if (!check(p, TOK_RPAREN)) {
        step = parse_expr(p, BP_NONE);
    }
    consume(p, TOK_RPAREN, "Expected ')' after for clauses");

    Stmt *body = parse_block(p);
    return stmt_for(&p->arena, init, cond, step, body, line);
}

/*
 * parse_match_stmt — parses:
 *   match ( expr ) {
 *       case Pattern: stmt | block
 *       case Pattern: stmt | block
 *       default:      stmt | block
 *   }
 *
 * Pattern is either an enum member access (Direction.North) or an int literal.
 * Each arm body is either a '{' block or one-or-more statements ending before
 * the next 'case', 'default', or '}'.
 * Fall-through is explicit: execution continues into the next arm unless 'break'.
 *
 * Precondition: 'match' has already been consumed.
 */
static Stmt *parse_match_stmt(Parser *p, int line) {
    typedef struct MatchArmNode MANode;

    consume(p, TOK_LPAREN, "Expected '(' after 'match'");
    Expr *subject = parse_expr(p, BP_NONE);
    consume(p, TOK_RPAREN, "Expected ')' after match subject");
    consume(p, TOK_LBRACE, "Expected '{' to open match body");

    Stmt *node = stmt_match(&p->arena, subject, line);
    MANode *tail = NULL;

    while (!check(p, TOK_RBRACE) && !check(p, TOK_EOF)) {
        MANode *arm = arena_alloc(&p->arena, sizeof(MANode));
        arm->is_default = false;
        arm->pattern    = NULL;
        arm->body       = NULL;
        arm->next       = NULL;

        if (match(p, TOK_CASE)) {
            /* case <pattern>: */
            arm->pattern = parse_expr(p, BP_NONE);
            consume(p, TOK_COLON, "Expected ':' after case pattern");
        } else if (match(p, TOK_DEFAULT)) {
            arm->is_default = true;
            node->match_stmt.has_default = true;
            consume(p, TOK_COLON, "Expected ':' after 'default'");
        } else {
            /* Unexpected token — record error and skip */
            if (!p->panic_mode && p->error_count < PARSER_MAX_ERRORS) {
                ParseError *e = &p->errors[p->error_count++];
                snprintf(e->message, sizeof(e->message),
                    "Expected 'case' or 'default' inside match (got '%s')",
                    token_type_name(p->current.type));
                e->line      = p->current.line;
                p->had_error  = true;
                p->panic_mode = true;
            }
            advance(p);
            continue;
        }

        /* Arm body: either a { block } or statements until next case/default/} */
        if (check(p, TOK_LBRACE)) {
            arm->body = parse_block(p);
        } else {
            /* Collect statements into a synthetic block */
            StmtNode *stmts = NULL, *stail = NULL;
            while (!check(p, TOK_CASE) && !check(p, TOK_DEFAULT) &&
                   !check(p, TOK_RBRACE) && !check(p, TOK_EOF)) {
                Stmt *s = parse_stmt(p);
                if (!s) break;
                StmtNode *sn = arena_alloc(&p->arena, sizeof(StmtNode));
                sn->stmt = s; sn->next = NULL;
                if (!stmts) stmts = stail = sn;
                else { stail->next = sn; stail = sn; }
            }
            Stmt *blk = arena_alloc(&p->arena, sizeof(Stmt));
            blk->kind         = STMT_BLOCK;
            blk->line         = line;
            blk->block.stmts  = stmts;
            arm->body = blk;
        }

        /* Append arm */
        if (!node->match_stmt.arms) node->match_stmt.arms = tail = arm;
        else { tail->next = arm; tail = arm; }
        node->match_stmt.arm_count++;
    }

    consume(p, TOK_RBRACE, "Expected '}' to close match");
    return node;
}

/*
 * parse_stmt — dispatches to the correct statement parser based on current token.
 * This is the central statement dispatch function.
 */
static Stmt *parse_stmt(Parser *p) {
    int line = p->current.line;

    /* Import declaration — must appear before any other top-level statements.
     * import <name>;       — system import (angle brackets)
     * import "path.xeno";  — local import (quoted string)
     * The compiler resolves these before the type-check pass. */
    if (match(p, TOK_IMPORT)) {
        Stmt *s = arena_alloc(&p->arena, sizeof(Stmt));
        s->kind = STMT_IMPORT;
        s->line = line;

        if (check(p, TOK_LT)) {
            /* System import: import <name> */
            advance(p); /* consume '<' */
            s->import_decl.is_system = true;

            /* Collect identifier(s) — e.g. <collections> or <math> */
            if (!check(p, TOK_IDENT)) {
                if (!p->panic_mode && p->error_count < PARSER_MAX_ERRORS) {
                    ParseError *e = &p->errors[p->error_count++];
                    snprintf(e->message, sizeof(e->message), "Expected system module name after '<'");
                    e->line = p->current.line;
                    p->had_error = true; p->panic_mode = true;
                }
                return s;
            }
            const char *start = p->current.start;
            int total_len = 0;
            while (check(p, TOK_IDENT) || (p->current.type == TOK_DOT)) {
                total_len += p->current.length;
                advance(p);
                if (check(p, TOK_DOT)) { total_len += 1; advance(p); }
                else break;
            }
            /* Re-measure: just use start..current.start */
            total_len = (int)(p->current.start - start);
            char *name = arena_alloc(&p->arena, total_len + 1);
            memcpy(name, start, total_len);
            name[total_len] = '\0';
            s->import_decl.path     = name;
            s->import_decl.path_len = total_len;

            consume(p, TOK_GT, "Expected '>' after system module name");
        } else if (check(p, TOK_STRING_LIT)) {
            /* Local import: import "path/to/file.xeno" */
            Token str_tok = p->current;
            advance(p);
            s->import_decl.is_system = false;
            /* Strip surrounding quotes */
            const char *raw   = str_tok.start + 1;
            int         rlen  = str_tok.length - 2;
            if (rlen < 0) rlen = 0;
            char *path = arena_alloc(&p->arena, rlen + 1);
            memcpy(path, raw, rlen);
            path[rlen] = '\0';
            s->import_decl.path     = path;
            s->import_decl.path_len = rlen;
        } else {
            if (!p->panic_mode && p->error_count < PARSER_MAX_ERRORS) {
                ParseError *e = &p->errors[p->error_count++];
                snprintf(e->message, sizeof(e->message),
                         "Expected '<n>' or \"path\" after 'import'");
                e->line = p->current.line;
                p->had_error = true; p->panic_mode = true;
            }
            return s;
        }

        match(p, TOK_SEMICOLON);  /* semicolon after import is optional */
        return s;
    }

    /* Function declaration */
    if (match(p, TOK_FN)) {
        return parse_fn_decl(p, line);
    }

    /* Class declaration — optionally preceded by @Annotation(...) */
    {
        /* Collect any leading annotations */
        AnnotationNode *annotations     = NULL;
        AnnotationNode *annotations_tail = NULL;

        while (check(p, TOK_AT)) {
            advance(p);  /* consume '@' */
            Token ann_name = p->current;
            consume(p, TOK_IDENT, "Expected annotation name after '@'");

            AnnotationNode *ann = arena_alloc(&p->arena, sizeof(AnnotationNode));
            ann->name      = ann_name.start;
            ann->name_len  = ann_name.length;
            ann->args      = NULL;
            ann->next      = NULL;

            /* Parse optional (arg, ...) argument list.
             * Supports:
             *   positional:  @Mod("name", "1.0")
             *   named:       @Mod(name="name", version="1.0")
             *   expressions: @Hook(Target.Class)
             *   mixed:       @Attr("val", count=3)
             */
            if (match(p, TOK_LPAREN)) {
                AnnotationKVNode *kv_tail = NULL;
                while (!check(p, TOK_RPAREN) && !check(p, TOK_EOF)) {
                    AnnotationKVNode *kv = arena_alloc(&p->arena, sizeof(AnnotationKVNode));
                    kv->key     = NULL;
                    kv->key_len = 0;
                    kv->value   = NULL;
                    kv->next    = NULL;

                    /* Check for named arg: ident '=' expr */
                    if (check(p, TOK_IDENT)) {
                        Token peek_next = p->next;  /* lookahead */
                        if (peek_next.type == TOK_ASSIGN) {
                            /* named arg */
                            Token key_tok = p->current;
                            advance(p);  /* consume ident */
                            advance(p);  /* consume '=' */
                            kv->key     = key_tok.start;
                            kv->key_len = key_tok.length;
                        }
                    }

                    /* Parse the value as a full expression */
                    kv->value = parse_expr(p, BP_NONE);

                    if (!ann->args) { ann->args = kv_tail = kv; }
                    else            { kv_tail->next = kv; kv_tail = kv; }

                    if (!match(p, TOK_COMMA)) break;
                }
                consume(p, TOK_RPAREN, "Expected ')' after annotation arguments");
            }

            if (!annotations) { annotations = annotations_tail = ann; }
            else              { annotations_tail->next = ann; annotations_tail = ann; }
        }

        if (match(p, TOK_CLASS)) {
            Stmt *cls = parse_class_decl(p, line);
            if (cls) cls->class_decl.annotations = annotations;
            return cls;
        }

        /* If we consumed annotations but no class follows, that's an error */
        if (annotations) {
            consume(p, TOK_CLASS, "Expected 'class' after annotation");
            return NULL;
        }
    }

    if (match(p, TOK_CLASS)) {
        return parse_class_decl(p, line);
    }

    /* Interface declaration */
    if (match(p, TOK_INTERFACE)) {
        return parse_interface_decl(p, line);
    }

    /* Enum declaration */
    if (match(p, TOK_ENUM)) {
        Token enum_name = p->current;
        consume(p, TOK_IDENT, "Expected enum name after 'enum'");
        consume(p, TOK_LBRACE, "Expected '{' after enum name");

        Stmt *s = stmt_enum_decl(&p->arena, enum_name.start, enum_name.length, line);

        typedef struct EnumMemberNode EMNode;
        int next_value = 0;
        EMNode *tail   = NULL;

        while (!check(p, TOK_RBRACE) && !check(p, TOK_EOF)) {
            Token member_name = p->current;
            consume(p, TOK_IDENT, "Expected enum member name");

            int  value        = next_value;
            bool has_explicit = false;
            if (match(p, TOK_ASSIGN)) {
                /* Explicit value — parse an integer literal */
                bool neg = match(p, TOK_MINUS);
                Token val_tok = p->current;
                consume(p, TOK_INT_LIT, "Expected integer value after '='");
                /* Parse the raw token text as a base-10 integer */
                int v = 0;
                for (int i = 0; i < val_tok.length; i++)
                    v = v * 10 + (val_tok.start[i] - '0');
                value        = neg ? -v : v;
                has_explicit = true;
            }
            next_value = value + 1;

            EMNode *m          = arena_alloc(&p->arena, sizeof(EMNode));
            m->name            = member_name.start;
            m->length          = member_name.length;
            m->value           = value;
            m->has_explicit_value = has_explicit;
            m->next            = NULL;

            if (!s->enum_decl.members) { s->enum_decl.members = tail = m; }
            else                       { tail->next = m; tail = m; }
            s->enum_decl.member_count++;

            /* Members separated by commas; trailing comma is allowed */
            if (!match(p, TOK_COMMA)) break;
        }

        consume(p, TOK_RBRACE, "Expected '}' after enum members");
        return s;
    }

    /* Variable declaration — starts with a type keyword.
     * For class types: `Greeter g = ...` — current=IDENT, next=IDENT.
     * We use the 2-token lookahead to distinguish from expression statements. */
    {
        TokenType ct = p->current.type;
        if (ct == TOK_INT || ct == TOK_FLOAT ||
            ct == TOK_BOOL || ct == TOK_STRING ||
        ct == TOK_SBYTE || ct == TOK_BYTE || ct == TOK_SHORT ||
        ct == TOK_USHORT || ct == TOK_UINT || ct == TOK_LONG ||
        ct == TOK_ULONG || ct == TOK_DOUBLE || ct == TOK_CHAR) {
            Type type = parse_type(p);
            return parse_var_decl(p, type, line);
        }
        /* Class-typed variable: ClassName varName = ...
         * Also handles array types: ClassName[] varName = ...
         * Also handles generic types: ClassName<T> varName = ... */
        if (ct == TOK_IDENT && p->next.type == TOK_IDENT) {
            Type type = parse_type(p);   /* consumes the class name */
            return parse_var_decl(p, type, line);
        }
        /* Generic type variable: ClassName<TypeArg> varName = ...
         * OR generic function call: foo<TypeArg>(args);
         * We consume the type/call, then decide based on what follows. */
        if (ct == TOK_IDENT && p->next.type == TOK_LT) {
            /* Save the function/type name before consuming */
            Token saved_name = p->current;

            /* Try parsing as a type: consumes IDENT and <TypeArgs> */
            Type type = parse_type(p);

            /* After parsing, if current is IDENT → variable declaration */
            if (check(p, TOK_IDENT)) {
                return parse_var_decl(p, type, line);
            }

            /* If current is '(' → this was a generic function CALL, not a type.
             * Build the call expression from the saved name and type args. */
            if (check(p, TOK_LPAREN)) {
                advance(p); /* consume '(' */

                ArgNode *args      = NULL;
                ArgNode *args_tail = NULL;
                int      arg_count = 0;
                if (!check(p, TOK_RPAREN)) {
                    do {
                        Expr    *arg  = parse_expr(p, BP_NONE);
                        ArgNode *node = arg_node(&p->arena, arg, NULL);
                        if (!args) { args = args_tail = node; }
                        else       { args_tail->next = node; args_tail = node; }
                        arg_count++;
                    } while (match(p, TOK_COMMA));
                }
                consume(p, TOK_RPAREN, "Expected ')' after arguments");

                /* Reconstruct the type args from the canonical type name */
                TypeArgNode *targs      = NULL;
                TypeArgNode *targs_tail = NULL;
                int          targ_count = 0;
                if (type.kind == TYPE_OBJECT && type.class_name &&
                    is_generic_type_name(type.class_name))
                {
                    /* The "type" was actually parsed as "foo<TypeArg>" — we need
                     * to extract the base name (fn name) and the type args. */
                    char base[64];
                    if (generic_base_name(type.class_name, base, sizeof(base))) {
                        /* Re-parse type args from canonical name */
                        Type targ_types[MAX_TYPE_ARGS];
                        int  tc = parse_canonical_type_args(type.class_name,
                                                           targ_types, MAX_TYPE_ARGS);
                        for (int i = 0; i < tc; i++) {
                            TypeArgNode *ta = arena_alloc(&p->arena, sizeof(TypeArgNode));
                            ta->type = targ_types[i];
                            ta->next = NULL;
                            if (!targs) { targs = targs_tail = ta; }
                            else        { targs_tail->next = ta; targs_tail = ta; }
                            targ_count++;
                        }
                        /* Build the call expression using the base name */
                        char *fn_name = arena_alloc(&p->arena, strlen(base) + 1);
                        memcpy(fn_name, base, strlen(base) + 1);
                        Expr *call_e = expr_call(&p->arena,
                                                  fn_name, (int)strlen(fn_name),
                                                  args, arg_count, line);
                        call_e->call.type_args      = targs;
                        call_e->call.type_arg_count = targ_count;
                        consume(p, TOK_SEMICOLON, "Expected ';' after expression");
                        return stmt_expr(&p->arena, call_e, line);
                    }
                }

                /* Fallback: treat as non-generic call with the type's class name */
                Expr *call_e = expr_call(&p->arena,
                                          saved_name.start, saved_name.length,
                                          args, arg_count, line);
                consume(p, TOK_SEMICOLON, "Expected ';' after expression");
                return stmt_expr(&p->arena, call_e, line);
            }

            /* Neither IDENT nor '(' after type — parse error */
            if (!p->panic_mode && p->error_count < PARSER_MAX_ERRORS) {
                ParseError *e = &p->errors[p->error_count++];
                snprintf(e->message, sizeof(e->message),
                         "Expected variable name or '(' after generic type");
                e->line      = p->current.line;
                p->had_error = true;
                p->panic_mode = true;
            }
            return NULL;
        }
        /* Class/interface array: ClassName[] varName = ...
         * Distinguish from array access: data[i] = ...
         * Use 3-token lookahead: Type[] requires next=[ and peek=] */
        if (ct == TOK_IDENT && p->next.type == TOK_LBRACKET &&
            p->peek.type == TOK_RBRACKET) {
            Type type = parse_type(p);   /* consumes ClassName[] */
            return parse_var_decl(p, type, line);
        }
    }

    /* Control flow */
    if (match(p, TOK_IF))       return parse_if_stmt(p, line);
    if (match(p, TOK_WHILE))    return parse_while_stmt(p, line);
    if (match(p, TOK_FOR))      return parse_for_stmt(p, line);
    if (match(p, TOK_FOREACH))  return parse_foreach_stmt(p, line);
    if (match(p, TOK_MATCH))    return parse_match_stmt(p, line);

    if (match(p, TOK_RETURN)) {
        Expr *val = NULL;
        if (!check(p, TOK_SEMICOLON)) {
            val = parse_expr(p, BP_NONE);
        }
        consume(p, TOK_SEMICOLON, "Expected ';' after return value");
        return stmt_return(&p->arena, val, line);
    }

    if (match(p, TOK_BREAK)) {
        consume(p, TOK_SEMICOLON, "Expected ';' after 'break'");
        return stmt_break(&p->arena, line);
    }

    if (match(p, TOK_CONTINUE)) {
        consume(p, TOK_SEMICOLON, "Expected ';' after 'continue'");
        return stmt_continue(&p->arena, line);
    }

    /* Block */
    if (check(p, TOK_LBRACE)) {
        return parse_block(p);
    }

    /* Expression statement — an expression followed by a semicolon.
     * The most common form is a function call used as a statement. */
    {
        Expr *e = parse_expr(p, BP_NONE);
        consume(p, TOK_SEMICOLON, "Expected ';' after expression");
        Stmt *s = stmt_expr(&p->arena, e, line);

        /* If we're in panic mode after the expression, synchronize */
        if (p->panic_mode) synchronize(p);

        return s;
    }
}


static Stmt *
parse_foreach_stmt(Parser *p, int line) {
    consume(p, TOK_LPAREN, "Expected '(' after 'foreach'");
    Type   elem_type = parse_type(p);
    Token  var_tok   = p->current;
    consume(p, TOK_IDENT, "Expected variable name after type in foreach");
    consume(p, TOK_IN,    "Expected 'in' after foreach variable name");
    Expr  *array     = parse_expr(p, BP_NONE);
    consume(p, TOK_RPAREN, "Expected ')' after foreach array expression");
    Stmt  *body      = parse_block(p);
    char  *name_copy = arena_alloc(&p->arena, var_tok.length + 1);
    memcpy(name_copy, var_tok.start, var_tok.length);
    name_copy[var_tok.length] = '\0';
    return stmt_foreach(&p->arena, elem_type, name_copy, var_tok.length, array, body, line);
}

/* ─────────────────────────────────────────────────────────────────────────────
 * PUBLIC INTERFACE
 * ───────────────────────────────────────────────────────────────────────────*/

void parser_init(Parser *p, Lexer *lexer) {
    p->lexer       = lexer;
    p->error_count = 0;
    p->had_error   = false;
    p->panic_mode  = false;

    arena_init(&p->arena, ARENA_DEFAULT_SIZE);

    /* Prime the 3-token lookahead */
    p->previous.type = TOK_EOF;
    p->current.type  = TOK_EOF;
    p->next.type     = TOK_EOF;
    p->peek.type     = TOK_EOF;
    /* Call advance three times: fills current, next, and peek */
    advance(p);
    advance(p);
    advance(p);
}

Program parser_parse(Parser *p) {
    Program program;
    program.stmts = NULL;
    program.count = 0;

    StmtNode *head = NULL;
    StmtNode *tail = NULL;

    while (!check(p, TOK_EOF)) {
        Stmt *s = parse_stmt(p);
        if (p->panic_mode) synchronize(p);

        StmtNode *n = stmt_node(&p->arena, s, NULL);
        if (!head) { head = tail = n; }
        else       { tail->next = n; tail = n; }
        program.count++;
    }

    program.stmts = head;
    return program;
}

void parser_free(Parser *p) {
    arena_free(&p->arena);
}

void parser_print_errors(const Parser *p) {
    for (int i = 0; i < p->error_count; i++) {
        fprintf(stdout, "[line %d] Parse error: %s\n",
                p->errors[i].line,
                p->errors[i].message);
    }
}