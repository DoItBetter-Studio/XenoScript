/*
 * ast.c — AST node constructors and debug printer
 *
 * The constructors are intentionally thin — they allocate from the arena,
 * set the tag and fields, and return. No logic here. Logic lives in the
 * parser, type checker, and compiler.
 *
 * The debug printer lets you dump the entire AST as indented text after
 * parsing. This is one of the most useful tools you'll have — when something
 * goes wrong in the compiler, printing the AST first tells you immediately
 * whether the problem is in the parser or later.
 */

#include "ast.h"
#include <stdio.h>
#include <string.h>

/* ─────────────────────────────────────────────────────────────────────────────
 * TYPE HELPERS
 * ───────────────────────────────────────────────────────────────────────────*/

const char *type_kind_name(TypeKind kind) {
    switch (kind) {
        case TYPE_UNKNOWN:   return "<unknown>";
        case TYPE_VOID:      return "void";
        case TYPE_BOOL:      return "bool";
        case TYPE_INT:       return "int";
        case TYPE_FLOAT:     return "float";
        case TYPE_STRING:    return "string";
        case TYPE_OBJECT:    return "<object>";
        case TYPE_ENUM:      return "<enum>";
        case TYPE_CLASS_REF: return "<class>";
        case TYPE_SBYTE:     return "sbyte";
        case TYPE_BYTE:      return "byte";
        case TYPE_SHORT:     return "short";
        case TYPE_USHORT:    return "ushort";
        case TYPE_UINT:      return "uint";
        case TYPE_LONG:      return "long";
        case TYPE_ULONG:     return "ulong";
        case TYPE_DOUBLE:    return "double";
        case TYPE_CHAR:      return "char";
        case TYPE_ANY:       return "any";
        case TYPE_ARRAY:     return "array";
        default:             return "<invalid>";
    }
}

Type type_void(void)   { Type t; t.kind = TYPE_VOID;   t.class_name = NULL; t.enum_name = NULL; t.element_type = NULL; return t; }
Type type_bool(void)   { Type t; t.kind = TYPE_BOOL;   t.class_name = NULL; t.enum_name = NULL; t.element_type = NULL; return t; }
Type type_int(void)    { Type t; t.kind = TYPE_INT;    t.class_name = NULL; t.enum_name = NULL; t.element_type = NULL; return t; }
Type type_float(void)  { Type t; t.kind = TYPE_FLOAT;  t.class_name = NULL; t.enum_name = NULL; t.element_type = NULL; return t; }
Type type_string(void) { Type t; t.kind = TYPE_STRING; t.class_name = NULL; t.enum_name = NULL; t.element_type = NULL; return t; }
Type type_object(const char *class_name) {
    Type t;
    t.kind       = TYPE_OBJECT;
    t.class_name = class_name;
    t.enum_name  = NULL;
    t.element_type = NULL;
    return t;
}
Type type_enum(const char *enum_name) {
    Type t;
    t.kind       = TYPE_ENUM;
    t.class_name = NULL;
    t.enum_name  = enum_name;
    t.element_type = NULL;
    return t;
}
Type type_sbyte(void)  { Type t; t.kind = TYPE_SBYTE;  t.class_name = NULL; t.enum_name = NULL; t.element_type = NULL; return t; }
Type type_byte(void)   { Type t; t.kind = TYPE_BYTE;   t.class_name = NULL; t.enum_name = NULL; t.element_type = NULL; return t; }
Type type_short(void)  { Type t; t.kind = TYPE_SHORT;  t.class_name = NULL; t.enum_name = NULL; t.element_type = NULL; return t; }
Type type_ushort(void) { Type t; t.kind = TYPE_USHORT; t.class_name = NULL; t.enum_name = NULL; t.element_type = NULL; return t; }
Type type_uint(void)   { Type t; t.kind = TYPE_UINT;   t.class_name = NULL; t.enum_name = NULL; t.element_type = NULL; return t; }
Type type_long(void)   { Type t; t.kind = TYPE_LONG;   t.class_name = NULL; t.enum_name = NULL; t.element_type = NULL; return t; }
Type type_ulong(void)  { Type t; t.kind = TYPE_ULONG;  t.class_name = NULL; t.enum_name = NULL; t.element_type = NULL; return t; }
Type type_double(void) { Type t; t.kind = TYPE_DOUBLE; t.class_name = NULL; t.enum_name = NULL; t.element_type = NULL; return t; }
Type type_char(void)   { Type t; t.kind = TYPE_CHAR;   t.class_name = NULL; t.enum_name = NULL; t.element_type = NULL; return t; }
Type type_any(void)    { Type t; t.kind = TYPE_ANY;    t.class_name = NULL; t.enum_name = NULL; t.element_type = NULL; return t; }
Type type_array(Type *element_type) {
    Type t;
    t.kind         = TYPE_ARRAY;
    t.class_name   = NULL;
    t.enum_name    = NULL;
    t.element_type = element_type;
    return t;
}

Type type_class_ref(const char *class_name) {
    Type t;
    t.kind       = TYPE_CLASS_REF;
    t.class_name = class_name;
    t.enum_name  = NULL;
    t.element_type = NULL;
    return t;
}

/* Two types are equal if they have the same kind.
 * For objects, they must also name the same class.
 * For enums, they must name the same enum type. */
bool type_equals(Type a, Type b) {
    if (a.kind != b.kind) return false;
    if (a.kind == TYPE_ARRAY) {
        /* Both array — element types must match */
        if (!a.element_type || !b.element_type) return false;
        return type_equals(*a.element_type, *b.element_type);
    }
    if (a.kind == TYPE_OBJECT) {
        if (!a.class_name || !b.class_name) return false;
        /* Simple pointer or string comparison on interned names */
        if (a.class_name == b.class_name) return true;
        /* Fallback: strcmp for safety */
        int i = 0;
        while (a.class_name[i] && b.class_name[i] &&
               a.class_name[i] == b.class_name[i]) i++;
        return a.class_name[i] == b.class_name[i];
    }
    if (a.kind == TYPE_ENUM) {
        if (!a.enum_name || !b.enum_name) return false;
        if (a.enum_name == b.enum_name) return true;
        int i = 0;
        while (a.enum_name[i] && b.enum_name[i] &&
               a.enum_name[i] == b.enum_name[i]) i++;
        return a.enum_name[i] == b.enum_name[i];
    }
    return true;
}
bool type_is_numeric(Type t) {
    switch (t.kind) {
        case TYPE_INT: case TYPE_FLOAT: case TYPE_DOUBLE:
        case TYPE_SBYTE: case TYPE_BYTE: case TYPE_SHORT: case TYPE_USHORT:
        case TYPE_UINT: case TYPE_LONG: case TYPE_ULONG:
            return true;
        default: return false;
    }
}

/* Returns true if the type is an integer family (not float/double) */
bool type_is_int_family(Type t) {
    switch (t.kind) {
        case TYPE_INT: case TYPE_SBYTE: case TYPE_BYTE:
        case TYPE_SHORT: case TYPE_USHORT: case TYPE_UINT:
        case TYPE_LONG: case TYPE_ULONG: case TYPE_CHAR:
            return true;
        default: return false;
    }
}

/* Returns true if the type is unsigned */
bool type_is_unsigned(Type t) {
    switch (t.kind) {
        case TYPE_BYTE: case TYPE_USHORT: case TYPE_UINT: case TYPE_ULONG:
            return true;
        default: return false;
    }
}


/* ─────────────────────────────────────────────────────────────────────────────
 * EXPRESSION CONSTRUCTORS
 *
 * Pattern: allocate from arena, set kind and line, fill payload, return.
 * resolved_type is left at TYPE_UNKNOWN (zeroed by arena_alloc) for the
 * type checker to fill in.
 * ───────────────────────────────────────────────────────────────────────────*/

Expr *expr_int_lit(Arena *a, int64_t value, int line) {
    Expr *e         = arena_alloc(a, sizeof(Expr));
    e->kind         = EXPR_INT_LIT;
    e->line         = line;
    e->int_lit.value = value;
    return e;
}

Expr *expr_char_lit(Arena *a, uint32_t value, int line) {
    Expr *e           = arena_alloc(a, sizeof(Expr));
    e->kind           = EXPR_CHAR_LIT;
    e->line           = line;
    e->char_lit.value = value;
    e->resolved_type  = type_char();
    return e;
}

Expr *expr_array_lit(Arena *a, ArgNode *elements, int count, int line) {
    Expr *e = arena_alloc(a, sizeof(Expr));
    e->kind              = EXPR_ARRAY_LIT;
    e->line              = line;
    e->array_lit.elements = elements;
    e->array_lit.count    = count;
    e->resolved_type.kind = TYPE_UNKNOWN;
    return e;
}

Expr *expr_index(Arena *a, Expr *array, Expr *index, int line) {
    Expr *e = arena_alloc(a, sizeof(Expr));
    e->kind             = EXPR_INDEX;
    e->line             = line;
    e->index_expr.array = array;
    e->index_expr.index = index;
    e->resolved_type.kind = TYPE_UNKNOWN;
    return e;
}

Expr *expr_index_assign(Arena *a, Expr *array, Expr *index, Expr *value, int line) {
    Expr *e = arena_alloc(a, sizeof(Expr));
    e->kind                  = EXPR_INDEX_ASSIGN;
    e->line                  = line;
    e->index_assign.array    = array;
    e->index_assign.index    = index;
    e->index_assign.value    = value;
    e->resolved_type.kind    = TYPE_UNKNOWN;
    return e;
}

Expr *expr_new_array(Arena *a, Type element_type, Expr *length, int line) {
    Expr *e = arena_alloc(a, sizeof(Expr));
    e->kind                   = EXPR_NEW_ARRAY;
    e->line                   = line;
    e->new_array.element_type = element_type;
    e->new_array.length       = length;
    e->resolved_type.kind     = TYPE_UNKNOWN;
    return e;
}

Expr *expr_is(Arena *a, Expr *operand, Type check_type, int line) {
    Expr *e = arena_alloc(a, sizeof(Expr));
    e->kind            = EXPR_IS;
    e->line            = line;
    e->type_op.operand     = operand;
    e->type_op.check_type  = check_type;
    return e;
}

Expr *expr_as(Arena *a, Expr *operand, Type check_type, int line) {
    Expr *e = arena_alloc(a, sizeof(Expr));
    e->kind            = EXPR_AS;
    e->line            = line;
    e->type_op.operand     = operand;
    e->type_op.check_type  = check_type;
    return e;
}

Expr *expr_typeof(Arena *a, Expr *operand, int line) {
    Expr *e = arena_alloc(a, sizeof(Expr));
    e->kind           = EXPR_TYPEOF;
    e->line           = line;
    e->type_of.operand = operand;
    return e;
}


Expr *expr_float_lit(Arena *a, double value, int line) {
    Expr *e           = arena_alloc(a, sizeof(Expr));
    e->kind           = EXPR_FLOAT_LIT;
    e->line           = line;
    e->float_lit.value = value;
    return e;
}

Expr *expr_bool_lit(Arena *a, bool value, int line) {
    Expr *e          = arena_alloc(a, sizeof(Expr));
    e->kind          = EXPR_BOOL_LIT;
    e->line          = line;
    e->bool_lit.value = value;
    return e;
}

Expr *expr_string_lit(Arena *a, const char *chars, int len, int line) {
    Expr *e               = arena_alloc(a, sizeof(Expr));
    e->kind               = EXPR_STRING_LIT;
    e->line               = line;
    e->string_lit.chars   = chars;
    e->string_lit.length  = len;
    return e;
}

Expr *expr_interp_string(Arena *a, int line) {
    Expr *e                          = arena_alloc(a, sizeof(Expr));
    e->kind                          = EXPR_INTERP_STRING;
    e->line                          = line;
    e->interp_string.segments        = NULL;
    e->interp_string.segment_count   = 0;
    return e;
}



Expr *expr_ident(Arena *a, const char *name, int len, int line) {
    Expr *e        = arena_alloc(a, sizeof(Expr));
    e->kind        = EXPR_IDENT;
    e->line        = line;
    e->ident.name   = name;
    e->ident.length = len;
    return e;
}

Expr *expr_unary(Arena *a, TokenType op, Expr *operand, int line) {
    Expr *e          = arena_alloc(a, sizeof(Expr));
    e->kind          = EXPR_UNARY;
    e->line          = line;
    e->unary.op      = op;
    e->unary.operand = operand;
    return e;
}

Expr *expr_postfix(Arena *a, TokenType op, const char *name, int length, int line) {
    Expr *e                   = arena_alloc(a, sizeof(Expr));
    e->kind                   = EXPR_POSTFIX;
    e->line                   = line;
    e->postfix.op             = op;
    e->postfix.is_prefix      = false;
    e->postfix.is_field       = false;
    e->postfix.name           = name;
    e->postfix.length         = length;
    e->postfix.object         = NULL;
    e->postfix.field_name     = NULL;
    e->postfix.field_name_len = 0;
    return e;
}

Expr *expr_prefix(Arena *a, TokenType op, const char *name, int length, int line) {
    Expr *e                   = arena_alloc(a, sizeof(Expr));
    e->kind                   = EXPR_POSTFIX;
    e->line                   = line;
    e->postfix.op             = op;
    e->postfix.is_prefix      = true;
    e->postfix.is_field       = false;
    e->postfix.name           = name;
    e->postfix.length         = length;
    e->postfix.object         = NULL;
    e->postfix.field_name     = NULL;
    e->postfix.field_name_len = 0;
    return e;
}

Expr *expr_postfix_field(Arena *a, TokenType op, bool is_prefix,
                         Expr *object, const char *field_name, int field_name_len, int line) {
    Expr *e                   = arena_alloc(a, sizeof(Expr));
    e->kind                   = EXPR_POSTFIX;
    e->line                   = line;
    e->postfix.op             = op;
    e->postfix.is_prefix      = is_prefix;
    e->postfix.is_field       = true;
    e->postfix.name           = NULL;
    e->postfix.length         = 0;
    e->postfix.object         = object;
    e->postfix.field_name     = field_name;
    e->postfix.field_name_len = field_name_len;
    return e;
}


Expr *expr_binary(Arena *a, TokenType op, Expr *left, Expr *right, int line) {
    Expr *e         = arena_alloc(a, sizeof(Expr));
    e->kind         = EXPR_BINARY;
    e->line         = line;
    e->binary.op    = op;
    e->binary.left  = left;
    e->binary.right = right;
    return e;
}

Expr *expr_assign(Arena *a, const char *name, int len, Expr *value, int line) {
    Expr *e          = arena_alloc(a, sizeof(Expr));
    e->kind          = EXPR_ASSIGN;
    e->line          = line;
    e->assign.name   = name;
    e->assign.length = len;
    e->assign.value  = value;
    return e;
}

Expr *expr_call(Arena *a, const char *name, int len,
                ArgNode *args, int count, int line) {
    Expr *e            = arena_alloc(a, sizeof(Expr));
    e->kind            = EXPR_CALL;
    e->line            = line;
    e->call.name       = name;
    e->call.length     = len;
    e->call.args       = args;
    e->call.arg_count  = count;
    return e;
}

Expr *expr_new(Arena *a, const char *class_name, int class_len,
               ArgNode *args, int count, int line) {
    Expr *e                       = arena_alloc(a, sizeof(Expr));
    e->kind                       = EXPR_NEW;
    e->line                       = line;
    e->new_expr.class_name        = class_name;
    e->new_expr.class_name_len    = class_len;
    e->new_expr.args              = args;
    e->new_expr.arg_count         = count;
    return e;
}

Expr *expr_field_get(Arena *a, Expr *object, const char *field, int field_len, int line) {
    Expr *e                       = arena_alloc(a, sizeof(Expr));
    e->kind                       = EXPR_FIELD_GET;
    e->line                       = line;
    e->field_get.object           = object;
    e->field_get.field_name       = field;
    e->field_get.field_name_len   = field_len;
    return e;
}

Expr *expr_field_set(Arena *a, Expr *object, const char *field, int field_len,
                     Expr *value, int line) {
    Expr *e                       = arena_alloc(a, sizeof(Expr));
    e->kind                       = EXPR_FIELD_SET;
    e->line                       = line;
    e->field_set.object           = object;
    e->field_set.field_name       = field;
    e->field_set.field_name_len   = field_len;
    e->field_set.value            = value;
    return e;
}

Expr *expr_method_call(Arena *a, Expr *object, const char *method, int method_len,
                       ArgNode *args, int count, int line) {
    Expr *e                         = arena_alloc(a, sizeof(Expr));
    e->kind                         = EXPR_METHOD_CALL;
    e->line                         = line;
    e->method_call.object           = object;
    e->method_call.method_name      = method;
    e->method_call.method_name_len  = method_len;
    e->method_call.args             = args;
    e->method_call.arg_count        = count;
    return e;
}

Expr *expr_this(Arena *a, int line) {
    Expr *e  = arena_alloc(a, sizeof(Expr));
    e->kind  = EXPR_THIS;
    e->line  = line;
    return e;
}

Expr *expr_super_call(Arena *a, ArgNode *args, int count, int line) {
    Expr *e                   = arena_alloc(a, sizeof(Expr));
    e->kind                   = EXPR_SUPER_CALL;
    e->line                   = line;
    e->super_call.args        = args;
    e->super_call.arg_count   = count;
    return e;
}


/* ─────────────────────────────────────────────────────────────────────────────
 * STATEMENT CONSTRUCTORS
 * ───────────────────────────────────────────────────────────────────────────*/

Stmt *stmt_var_decl(Arena *a, Type type, const char *name, int len,
                    Expr *init, int line) {
    Stmt *s             = arena_alloc(a, sizeof(Stmt));
    s->kind             = STMT_VAR_DECL;
    s->line             = line;
    s->var_decl.type    = type;
    s->var_decl.name    = name;
    s->var_decl.length  = len;
    s->var_decl.init    = init;
    return s;
}

Stmt *stmt_expr(Arena *a, Expr *expr, int line) {
    Stmt *s     = arena_alloc(a, sizeof(Stmt));
    s->kind     = STMT_EXPR;
    s->line     = line;
    s->expr.expr = expr;
    return s;
}

Stmt *stmt_if(Arena *a, Expr *cond, Stmt *then_b, Stmt *else_b, int line) {
    Stmt *s              = arena_alloc(a, sizeof(Stmt));
    s->kind              = STMT_IF;
    s->line              = line;
    s->if_stmt.condition   = cond;
    s->if_stmt.then_branch = then_b;
    s->if_stmt.else_branch = else_b;
    return s;
}

Stmt *stmt_while(Arena *a, Expr *cond, Stmt *body, int line) {
    Stmt *s                = arena_alloc(a, sizeof(Stmt));
    s->kind                = STMT_WHILE;
    s->line                = line;
    s->while_stmt.condition = cond;
    s->while_stmt.body      = body;
    return s;
}

Stmt *stmt_for(Arena *a, Stmt *init, Expr *cond, Expr *step,
               Stmt *body, int line) {
    Stmt *s              = arena_alloc(a, sizeof(Stmt));
    s->kind              = STMT_FOR;
    s->line              = line;
    s->for_stmt.init      = init;
    s->for_stmt.condition = cond;
    s->for_stmt.step      = step;
    s->for_stmt.body      = body;
    return s;
}

Stmt *stmt_foreach(Arena *a, Type elem_type, const char *var_name, int var_len,
                   Expr *array, Stmt *body, int line) {
    Stmt *s = arena_alloc(a, sizeof(Stmt));
    s->kind = STMT_FOREACH; s->line = line;
    s->foreach_stmt.elem_type = elem_type; s->foreach_stmt.var_name = var_name;
    s->foreach_stmt.var_len = var_len; s->foreach_stmt.array = array;
    s->foreach_stmt.body = body;
    return s;
}

Stmt *stmt_match(Arena *a, Expr *subject, int line) {
    Stmt *s                    = arena_alloc(a, sizeof(Stmt));
    s->kind                    = STMT_MATCH;
    s->line                    = line;
    s->match_stmt.subject      = subject;
    s->match_stmt.arms         = NULL;
    s->match_stmt.arm_count    = 0;
    s->match_stmt.has_default  = false;
    return s;
}

Stmt *stmt_return(Arena *a, Expr *value, int line) {
    Stmt *s              = arena_alloc(a, sizeof(Stmt));
    s->kind              = STMT_RETURN;
    s->line              = line;
    s->return_stmt.value = value;
    return s;
}

Stmt *stmt_break(Arena *a, int line) {
    Stmt *s = arena_alloc(a, sizeof(Stmt));
    s->kind = STMT_BREAK;
    s->line = line;
    return s;
}

Stmt *stmt_continue(Arena *a, int line) {
    Stmt *s = arena_alloc(a, sizeof(Stmt));
    s->kind = STMT_CONTINUE;
    s->line = line;
    return s;
}

Stmt *stmt_block(Arena *a, StmtNode *stmts, int line) {
    Stmt *s           = arena_alloc(a, sizeof(Stmt));
    s->kind           = STMT_BLOCK;
    s->line           = line;
    s->block.stmts    = stmts;
    return s;
}

Stmt *stmt_fn_decl(Arena *a, Type ret, const char *name, int len,
                   ParamNode *params, int param_count, Stmt *body, int line) {
    Stmt *s                  = arena_alloc(a, sizeof(Stmt));
    s->kind                  = STMT_FN_DECL;
    s->line                  = line;
    s->fn_decl.return_type   = ret;
    s->fn_decl.name          = name;
    s->fn_decl.length        = len;
    s->fn_decl.params        = params;
    s->fn_decl.param_count   = param_count;
    s->fn_decl.body          = body;
    return s;
}

Stmt *stmt_class_decl(Arena *a, const char *name, int len,
                      const char *parent_name, int parent_len, int line) {
    Stmt *s                        = arena_alloc(a, sizeof(Stmt));
    s->kind                        = STMT_CLASS_DECL;
    s->line                        = line;
    s->class_decl.name             = name;
    s->class_decl.length           = len;
    s->class_decl.parent_name      = parent_name;
    s->class_decl.parent_length    = parent_len;
    s->class_decl.fields           = NULL;
    s->class_decl.field_count      = 0;
    s->class_decl.methods          = NULL;
    s->class_decl.method_count     = 0;
    return s;
}

Stmt *stmt_enum_decl(Arena *a, const char *name, int len, int line) {
    Stmt *s              = arena_alloc(a, sizeof(Stmt));
    s->kind              = STMT_ENUM_DECL;
    s->line              = line;
    s->enum_decl.name    = name;
    s->enum_decl.length  = len;
    s->enum_decl.members = NULL;
    s->enum_decl.member_count = 0;
    return s;
}

Stmt *stmt_interface_decl(Arena *a, const char *name, int len, int line) {
    Stmt *s                          = arena_alloc(a, sizeof(Stmt));
    s->kind                          = STMT_INTERFACE_DECL;
    s->line                          = line;
    s->interface_decl.name           = name;
    s->interface_decl.length         = len;
    s->interface_decl.parent_name    = NULL;
    s->interface_decl.parent_length  = 0;
    s->interface_decl.methods        = NULL;
    s->interface_decl.method_count   = 0;
    return s;
}


/* ─────────────────────────────────────────────────────────────────────────────
 * LIST NODE HELPERS
 * ───────────────────────────────────────────────────────────────────────────*/

StmtNode *stmt_node(Arena *a, Stmt *stmt, StmtNode *next) {
    StmtNode *n = arena_alloc(a, sizeof(StmtNode));
    n->stmt = stmt;
    n->next = next;
    return n;
}

ArgNode *arg_node(Arena *a, Expr *expr, ArgNode *next) {
    ArgNode *n = arena_alloc(a, sizeof(ArgNode));
    n->expr = expr;
    n->next = next;
    return n;
}

ParamNode *param_node(Arena *a, Type type, const char *name, int len,
                      ParamNode *next) {
    ParamNode *n = arena_alloc(a, sizeof(ParamNode));
    n->type   = type;
    n->name   = name;
    n->length = len;
    n->next   = next;
    return n;
}


/* ─────────────────────────────────────────────────────────────────────────────
 * DEBUG PRINTER
 *
 * Prints the AST as indented text. Extremely useful when debugging the parser.
 * Not used in the final pipeline — #ifdef it out of release builds if you want.
 *
 * Example output for `int x = 2 + 3;` :
 *
 *   STMT_VAR_DECL int 'x'
 *     EXPR_BINARY '+'  [int]
 *       EXPR_INT_LIT 2  [int]
 *       EXPR_INT_LIT 3  [int]
 * ───────────────────────────────────────────────────────────────────────────*/

static void print_indent(int indent) {
    for (int i = 0; i < indent; i++) printf("  ");
}

void ast_print_expr(const Expr *expr, int indent) {
    if (!expr) { print_indent(indent); printf("<null expr>\n"); return; }

    print_indent(indent);

    /* Print the resolved type in brackets if it's been resolved */
    const char *type_str = type_kind_name(expr->resolved_type.kind);

    switch (expr->kind) {
        case EXPR_INT_LIT:
            printf("EXPR_INT_LIT %lld  [%s]\n",
                   (long long)expr->int_lit.value, type_str);
            break;

        case EXPR_CHAR_LIT:
            printf("EXPR_CHAR_LIT U+%04X  [%s]\n",
                   (unsigned)expr->char_lit.value, type_str);
            break;

        case EXPR_ARRAY_LIT:
            printf("EXPR_ARRAY_LIT (count=%d)  [%s]\n",
                   expr->array_lit.count, type_str);
            break;

        case EXPR_INDEX:
            printf("EXPR_INDEX  [%s]\n", type_str);
            break;

        case EXPR_INDEX_ASSIGN:
            printf("EXPR_INDEX_ASSIGN  [%s]\n", type_str);
            break;

        case EXPR_NEW_ARRAY:
        case EXPR_IS:     printf("EXPR_IS\n");     break;
        case EXPR_AS:     printf("EXPR_AS\n");     break;
        case EXPR_TYPEOF: printf("EXPR_TYPEOF\n"); break;
            printf("EXPR_NEW_ARRAY  [%s]\n", type_str);
            break;

        case EXPR_FLOAT_LIT:
            printf("EXPR_FLOAT_LIT %g  [%s]\n",
                   expr->float_lit.value, type_str);
            break;

        case EXPR_BOOL_LIT:
            printf("EXPR_BOOL_LIT %s  [%s]\n",
                   expr->bool_lit.value ? "true" : "false", type_str);
            break;

        case EXPR_STRING_LIT:
            printf("EXPR_STRING_LIT \"%.*s\"  [%s]\n",
                   expr->string_lit.length, expr->string_lit.chars, type_str);
            break;

        case EXPR_INTERP_STRING: {
            typedef struct InterpSegment ISeg;
            printf("EXPR_INTERP_STRING (%d segments)  [%s]\n",
                   expr->interp_string.segment_count, type_str);
            for (ISeg *s = expr->interp_string.segments; s; s = s->next) {
                print_indent(indent + 1);
                if (!s->is_expr)
                    printf("TEXT \"%.*s\"\n", s->text_len, s->text);
                else {
                    printf("EXPR\n");
                    ast_print_expr(s->expr, indent + 2);
                }
            }
            break;
        }

        case EXPR_IDENT:
            printf("EXPR_IDENT '%.*s'  [%s]\n",
                   expr->ident.length, expr->ident.name, type_str);
            break;

        case EXPR_UNARY:
            printf("EXPR_UNARY '%s'  [%s]\n",
                   token_type_name(expr->unary.op), type_str);
            ast_print_expr(expr->unary.operand, indent + 1);
            break;

        case EXPR_POSTFIX:
            if (expr->postfix.is_field) {
                printf("%s '%s' on <obj>.%.*s  [%s]\n",
                       expr->postfix.is_prefix ? "EXPR_PREFIX_FIELD" : "EXPR_POSTFIX_FIELD",
                       token_type_name(expr->postfix.op),
                       expr->postfix.field_name_len, expr->postfix.field_name, type_str);
                ast_print_expr(expr->postfix.object, indent + 2);
            } else {
                printf("%s '%s' on '%.*s'  [%s]\n",
                       expr->postfix.is_prefix ? "EXPR_PREFIX" : "EXPR_POSTFIX",
                       token_type_name(expr->postfix.op),
                       expr->postfix.length, expr->postfix.name, type_str);
            }
            break;

        case EXPR_BINARY:
            printf("EXPR_BINARY '%s'  [%s]\n",
                   token_type_name(expr->binary.op), type_str);
            ast_print_expr(expr->binary.left,  indent + 1);
            ast_print_expr(expr->binary.right, indent + 1);
            break;

        case EXPR_ASSIGN:
            printf("EXPR_ASSIGN '%.*s'  [%s]\n",
                   expr->assign.length, expr->assign.name, type_str);
            ast_print_expr(expr->assign.value, indent + 1);
            break;

        case EXPR_CALL:
            printf("EXPR_CALL '%.*s' (%d args)  [%s]\n",
                   expr->call.length, expr->call.name,
                   expr->call.arg_count, type_str);
            for (ArgNode *arg = expr->call.args; arg; arg = arg->next)
                ast_print_expr(arg->expr, indent + 1);
            break;

        case EXPR_NEW:
            printf("EXPR_NEW '%.*s' (%d args)  [%s]\n",
                   expr->new_expr.class_name_len, expr->new_expr.class_name,
                   expr->new_expr.arg_count, type_str);
            for (ArgNode *arg = expr->new_expr.args; arg; arg = arg->next)
                ast_print_expr(arg->expr, indent + 1);
            break;

        case EXPR_THIS:
            printf("EXPR_THIS  [%s]\n", type_str);
            break;
        case EXPR_ENUM_ACCESS:
            printf("EXPR_ENUM_ACCESS %.*s.%.*s = %d  [%s]\n",
                   expr->enum_access.enum_name_len, expr->enum_access.enum_name,
                   expr->enum_access.member_name_len, expr->enum_access.member_name,
                   expr->enum_access.value, type_str);
            break;

        case EXPR_STATIC_GET:
            printf("EXPR_STATIC_GET %.*s.%.*s  [%s]\n",
                   expr->static_get.class_name_len, expr->static_get.class_name,
                   expr->static_get.field_name_len, expr->static_get.field_name,
                   type_str);
            break;

        case EXPR_STATIC_SET:
            printf("EXPR_STATIC_SET %.*s.%.*s  [%s]\n",
                   expr->static_set.class_name_len, expr->static_set.class_name,
                   expr->static_set.field_name_len, expr->static_set.field_name,
                   type_str);
            break;

        case EXPR_STATIC_CALL:
            printf("EXPR_STATIC_CALL %.*s.%.*s(...)  [%s]\n",
                   expr->static_call.class_name_len, expr->static_call.class_name,
                   expr->static_call.method_name_len, expr->static_call.method_name,
                   type_str);
            break;

        case EXPR_SUPER_CALL:
            printf("EXPR_SUPER_CALL (%d args)  [%s]\n",
                   expr->super_call.arg_count, type_str);
            for (ArgNode *a = expr->super_call.args; a; a = a->next)
                ast_print_expr(a->expr, indent + 1);
            break;

        case EXPR_FIELD_GET:
            printf("EXPR_FIELD_GET '%.*s'  [%s]\n",
                   expr->field_get.field_name_len, expr->field_get.field_name,
                   type_str);
            ast_print_expr(expr->field_get.object, indent + 1);
            break;

        case EXPR_FIELD_SET:
            printf("EXPR_FIELD_SET '%.*s'  [%s]\n",
                   expr->field_set.field_name_len, expr->field_set.field_name,
                   type_str);
            ast_print_expr(expr->field_set.object, indent + 1);
            ast_print_expr(expr->field_set.value,  indent + 1);
            break;

        case EXPR_METHOD_CALL:
            printf("EXPR_METHOD_CALL '%.*s' (%d args)  [%s]\n",
                   expr->method_call.method_name_len, expr->method_call.method_name,
                   expr->method_call.arg_count, type_str);
            ast_print_expr(expr->method_call.object, indent + 1);
            for (ArgNode *arg = expr->method_call.args; arg; arg = arg->next)
                ast_print_expr(arg->expr, indent + 1);
            break;
    }
}

void ast_print_stmt(const Stmt *stmt, int indent) {
    if (!stmt) return;

    print_indent(indent);

    switch (stmt->kind) {
        case STMT_VAR_DECL:
            printf("STMT_VAR_DECL %s '%.*s'\n",
                   type_kind_name(stmt->var_decl.type.kind),
                   stmt->var_decl.length, stmt->var_decl.name);
            if (stmt->var_decl.init)
                ast_print_expr(stmt->var_decl.init, indent + 1);
            break;

        case STMT_EXPR:
            printf("STMT_EXPR\n");
            ast_print_expr(stmt->expr.expr, indent + 1);
            break;

        case STMT_IF:
            printf("STMT_IF\n");
            print_indent(indent + 1); printf("condition:\n");
            ast_print_expr(stmt->if_stmt.condition,   indent + 2);
            print_indent(indent + 1); printf("then:\n");
            ast_print_stmt(stmt->if_stmt.then_branch, indent + 2);
            if (stmt->if_stmt.else_branch) {
                print_indent(indent + 1); printf("else:\n");
                ast_print_stmt(stmt->if_stmt.else_branch, indent + 2);
            }
            break;

        case STMT_WHILE:
            printf("STMT_WHILE\n");
            print_indent(indent + 1); printf("condition:\n");
            ast_print_expr(stmt->while_stmt.condition, indent + 2);
            print_indent(indent + 1); printf("body:\n");
            ast_print_stmt(stmt->while_stmt.body,      indent + 2);
            break;

        case STMT_FOR:
            printf("STMT_FOR\n");
            if (stmt->for_stmt.init) {
                print_indent(indent + 1); printf("init:\n");
                ast_print_stmt(stmt->for_stmt.init, indent + 2);
            }
            if (stmt->for_stmt.condition) {
                print_indent(indent + 1); printf("condition:\n");
                ast_print_expr(stmt->for_stmt.condition, indent + 2);
            }
            if (stmt->for_stmt.step) {
                print_indent(indent + 1); printf("step:\n");
                ast_print_expr(stmt->for_stmt.step, indent + 2);
            }
            print_indent(indent + 1); printf("body:\n");
            ast_print_stmt(stmt->for_stmt.body, indent + 2);
            break;

        case STMT_FOREACH:
            printf("STMT_FOREACH\n");
            break;
        case STMT_MATCH: {
            typedef struct MatchArmNode MANode;
            printf("STMT_MATCH\n");
            print_indent(indent + 1); printf("subject:\n");
            ast_print_expr(stmt->match_stmt.subject, indent + 2);
            for (MANode *arm = stmt->match_stmt.arms; arm; arm = arm->next) {
                print_indent(indent + 1);
                if (arm->is_default) printf("default:\n");
                else { printf("case:\n"); ast_print_expr(arm->pattern, indent + 2); }
                ast_print_stmt(arm->body, indent + 2);
            }
            break;
        }

        case STMT_RETURN:
            printf("STMT_RETURN\n");
            if (stmt->return_stmt.value)
                ast_print_expr(stmt->return_stmt.value, indent + 1);
            break;

        case STMT_BREAK:
            printf("STMT_BREAK\n");
            break;

        case STMT_CONTINUE:
            printf("STMT_CONTINUE\n");
            break;

        case STMT_BLOCK:
            printf("STMT_BLOCK\n");
            for (StmtNode *n = stmt->block.stmts; n; n = n->next)
                ast_print_stmt(n->stmt, indent + 1);
            break;

        case STMT_FN_DECL:
            printf("STMT_FN_DECL %s '%.*s' (",
                   type_kind_name(stmt->fn_decl.return_type.kind),
                   stmt->fn_decl.length, stmt->fn_decl.name);
            for (ParamNode *p = stmt->fn_decl.params; p; p = p->next) {
                printf("%s %.*s", type_kind_name(p->type.kind), p->length, p->name);
                if (p->next) printf(", ");
            }
            printf(")\n");
            ast_print_stmt(stmt->fn_decl.body, indent + 1);
            break;

        case STMT_CLASS_DECL: {
            printf("STMT_CLASS_DECL '%.*s'\n",
                   stmt->class_decl.length, stmt->class_decl.name);
            typedef struct ClassFieldNode  CFNode;
            typedef struct ClassMethodNode CMNode;
            for (CFNode *f = stmt->class_decl.fields; f; f = f->next) {
                print_indent(indent + 1);
                const char *acc = f->access == ACCESS_PRIVATE   ? " private"
                                : f->access == ACCESS_PROTECTED ? " protected"
                                :                                 " public";
                printf("FIELD %s '%.*s'%s%s\n",
                       type_kind_name(f->type.kind), f->length, f->name,
                       f->is_static ? " static" : "",
                       acc);
            }
            for (CMNode *m = stmt->class_decl.methods; m; m = m->next) {
                print_indent(indent + 1);
                printf("%s '%.*s'\n",
                       m->is_constructor ? "CONSTRUCTOR" : "METHOD",
                       m->fn->fn_decl.length, m->fn->fn_decl.name);
                ast_print_stmt(m->fn->fn_decl.body, indent + 2);
            }
            break;
        }

        case STMT_ENUM_DECL: {
            printf("STMT_ENUM_DECL '%.*s'\n",
                   stmt->enum_decl.length, stmt->enum_decl.name);
            typedef struct EnumMemberNode EMNode;
            for (EMNode *m = stmt->enum_decl.members; m; m = m->next) {
                print_indent(indent + 1);
                printf("MEMBER '%.*s' = %d\n", m->length, m->name, m->value);
            }
            break;
        }

        case STMT_INTERFACE_DECL: {
            printf("STMT_INTERFACE_DECL '%.*s'\n",
                   stmt->interface_decl.length, stmt->interface_decl.name);
            typedef struct IfaceMethodNode IMNode;
            for (IMNode *m = stmt->interface_decl.methods; m; m = m->next) {
                print_indent(indent + 1);
                printf("METHOD '%.*s' -> %s (%d params)\n",
                       m->length, m->name,
                       type_kind_name(m->return_type.kind),
                       m->param_count);
            }
            break;
        }
    }
}

void ast_print_program(const Program *program) {
    printf("=== AST ===\n");
    for (StmtNode *n = program->stmts; n; n = n->next)
        ast_print_stmt(n->stmt, 0);
    printf("===========\n");
}