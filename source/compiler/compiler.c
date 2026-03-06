/*
 * compiler.c — AST to bytecode compiler
 *
 * Walks the type-checked AST and emits bytecode into Chunks.
 * The type checker has already verified everything, so the compiler
 * can trust the types and focus purely on code generation.
 *
 * Structure:
 *   Module management    (init, free, find)
 *   Emit helpers         (write bytes, constants, jumps)
 *   Local variable mgmt  (declare, resolve, scope push/pop)
 *   Expression compiler  (compile_expr)
 *   Statement compiler   (compile_stmt)
 *   Top-level            (compiler_compile)
 */

#include "compiler.h"
#include <stdio.h>

/* Forward declarations */
static int host_table_find(const CompilerHostTable *t, const char *name, int len);
static void compile_expr(Compiler *c, const Expr *expr);

/* After emitting `provided` explicit args, emit default expressions for the
 * remaining params and return the padded total arg count. */
static int emit_default_args(Compiler *c, ParamNode *params,
                              int provided, int line) {
    (void)line;
    int idx = 0, total = provided;
    for (ParamNode *p = params; p; p = p->next, idx++) {
        if (idx < provided) continue;
        if (p->default_value) { compile_expr(c, p->default_value); total++; }
    }
    return total;
}
#include <string.h>
#include <stdlib.h>
#include <stdarg.h>

/* ─────────────────────────────────────────────────────────────────────────────
 * MODULE
 * ───────────────────────────────────────────────────────────────────────────*/

void module_init(Module *m) {
    m->chunks      = NULL;
    m->count       = 0;
    m->capacity    = 0;
    m->class_count = 0;
    m->sinit_index = -1;
    memset(m->names,    0, sizeof(m->names));
    memset(m->classes,  0, sizeof(m->classes));
    memset(&m->metadata, 0, sizeof(m->metadata));
}

void module_free(Module *m) {
    for (int i = 0; i < m->count; i++)
        chunk_free(&m->chunks[i]);
    free(m->chunks);
    m->chunks      = NULL;
    m->count       = 0;
    m->capacity    = 0;
    m->class_count = 0;
}

/* Grow the module's chunk array if needed, returns index of new slot */
int module_add_chunk(Module *m) {
    if (m->count >= m->capacity) {
        int new_cap = m->capacity < 8 ? 8 : m->capacity * 2;
        m->chunks   = realloc(m->chunks, new_cap * sizeof(Chunk));
        m->capacity = new_cap;
    }
    int idx = m->count++;
    chunk_init(&m->chunks[idx]);
    return idx;
}

int module_find(const Module *m, const char *name, int len) {
    for (int i = 0; i < m->count; i++) {
        if ((int)strlen(m->names[i]) == len &&
            memcmp(m->names[i], name, len) == 0)
            return i;
    }
    return -1;
}

int module_find_class(const Module *m, const char *name) {
    for (int i = 0; i < m->class_count; i++) {
        if (strcmp(m->classes[i].name, name) == 0)
            return i;
    }
    return -1;
}

/* Deep-copy a Chunk from src into dst. */
static bool chunk_deep_copy(Chunk *dst, const Chunk *src) {
    chunk_init(dst);
    dst->param_count       = src->param_count;
    dst->local_count       = src->local_count;
    dst->is_constructor    = src->is_constructor;
    dst->return_type_kind  = src->return_type_kind;
    for (int i = 0; i < src->param_count && i < 16; i++)
        dst->param_type_kinds[i] = src->param_type_kinds[i];

    if (src->count > 0) {
        dst->code  = malloc(src->count);
        dst->lines = malloc(src->count * sizeof(int));
        if (!dst->code || !dst->lines) { chunk_free(dst); return false; }
        memcpy(dst->code,  src->code,  src->count);
        memcpy(dst->lines, src->lines, src->count * sizeof(int));
        dst->count    = src->count;
        dst->capacity = src->count;
    }

    if (src->constants.count > 0) {
        dst->constants.values = malloc(src->constants.count * sizeof(Value));
        if (!dst->constants.values) { chunk_free(dst); return false; }
        memcpy(dst->constants.values, src->constants.values,
               src->constants.count * sizeof(Value));
        dst->constants.count    = src->constants.count;
        dst->constants.capacity = src->constants.count;
    }
    return true;
}

bool module_merge(Module *dst, const Module *src) {
    /* Merge functions — skip if name already exists in dst */
    for (int i = 0; i < src->count; i++) {
        const char *name = src->names[i];
        /* Skip internal sinit chunks — each module runs its own */
        if (strcmp(name, "__sinit__") == 0) continue;
        /* Skip if already present */
        if (module_find(dst, name, (int)strlen(name)) >= 0) continue;

        int idx = module_add_chunk(dst);
        if (!chunk_deep_copy(&dst->chunks[idx], &src->chunks[i])) return false;
        strncpy(dst->names[idx], name, 63);
    }

    /* Merge class definitions */
    for (int i = 0; i < src->class_count; i++) {
        const ClassDef *sc = &src->classes[i];
        if (module_find_class(dst, sc->name) >= 0) continue;
        if (dst->class_count >= MODULE_MAX_CLASSES) continue;
        dst->classes[dst->class_count++] = *sc;
    }
    return true;
}

void module_disassemble(const Module *m) {
    printf("\n+==========================================+\n");
    printf("|           MODULE DISASSEMBLY             |\n");
    printf("+==========================================+\n");
    if (m->metadata.has_mod) {
        printf("\n@Mod metadata:\n");
        printf("  name:        %s\n", m->metadata.name);
        if (m->metadata.version[0])     printf("  version:     %s\n", m->metadata.version);
        if (m->metadata.author[0])      printf("  author:      %s\n", m->metadata.author);
        if (m->metadata.description[0]) printf("  description: %s\n", m->metadata.description);
        printf("  entry class: %s\n", m->metadata.entry_class);
    }
    for (int i = 0; i < m->count; i++)
        chunk_disassemble(&m->chunks[i], m->names[i]);
}


/* ─────────────────────────────────────────────────────────────────────────────
 * ERROR REPORTING
 * ───────────────────────────────────────────────────────────────────────────*/

static void compile_error(Compiler *c, int line, const char *fmt, ...) {
    if (c->error_count >= COMPILER_MAX_ERRORS) return;
    CompileError *e = &c->errors[c->error_count++];
    e->line         = line;
    c->had_error    = true;
    va_list args;
    va_start(args, fmt);
    vsnprintf(e->message, sizeof(e->message), fmt, args);
    va_end(args);
}


/* ─────────────────────────────────────────────────────────────────────────────
 * EMIT HELPERS
 *
 * These are the only functions that write to the current chunk.
 * All other code goes through these.
 * ───────────────────────────────────────────────────────────────────────────*/

static void emit_byte(Compiler *c, uint8_t byte, int line) {
    chunk_write(c->current_chunk, byte, line);
}

static void emit_op(Compiler *c, OpCode op, int line) {
    emit_byte(c, (uint8_t)op, line);
}

/* Emit a uint16_t operand. Returns the offset of the first byte. */
static int emit_u16(Compiler *c, uint16_t value, int line) {
    return chunk_write_u16(c->current_chunk, value, line);
}

/* Emit a jump instruction with a placeholder operand.
 * Returns the offset of the placeholder so it can be patched later. */
static int emit_jump(Compiler *c, OpCode jump_op, int line) {
    emit_op(c, jump_op, line);
    return emit_u16(c, 0, line);  /* placeholder */
}

/* Patch a previously emitted jump to point to the current position. */
static void patch_jump(Compiler *c, int placeholder_offset) {
    /* Current position is where execution continues after the jump.
     * The jump offset is relative to the byte AFTER the 2-byte operand. */
    int after_operand = placeholder_offset + 2;
    int jump_dist     = c->current_chunk->count - after_operand;

    if (jump_dist > INT16_MAX) {
        compile_error(c, 0, "Jump distance too large (%d)", jump_dist);
        return;
    }
    chunk_patch_u16(c->current_chunk, placeholder_offset, (uint16_t)(int16_t)jump_dist);
}

/* Emit a backward jump to a known position (for while/for loops). */
static void emit_loop(Compiler *c, int loop_start, int line) {
    emit_op(c, OP_JUMP, line);
    /* Distance from the byte after this instruction back to loop_start.
     * Negative because we're jumping backward. */
    int after_operand = c->current_chunk->count + 2;
    int dist          = loop_start - after_operand;
    emit_u16(c, (uint16_t)(int16_t)dist, line);
}

/* Add a constant to the current chunk's pool and emit a load instruction. */
static void emit_const_int(Compiler *c, int64_t value, int line) {
    int idx = chunk_add_constant(c->current_chunk, val_int(value));
    emit_op(c, OP_LOAD_CONST_INT, line);
    emit_u16(c, (uint16_t)idx, line);
}

static void emit_const_float(Compiler *c, double value, int line) {
    int idx = chunk_add_constant(c->current_chunk, val_float(value));
    emit_op(c, OP_LOAD_CONST_FLOAT, line);
    emit_u16(c, (uint16_t)idx, line);
}

static void emit_const_str(Compiler *c, const char *chars, int len, int line) {
    /* Copy the string into a heap allocation for the constant pool.
     * The VM will own these strings for the lifetime of the module. */
    char *copy = malloc(len + 1);
    memcpy(copy, chars, len);
    copy[len] = '\0';
    int idx = chunk_add_constant(c->current_chunk, val_str(copy));
    emit_op(c, OP_LOAD_CONST_STR, line);
    emit_u16(c, (uint16_t)idx, line);
}

/* Add a string constant and return its index (no emit). Used by typeof. */
static int add_const_str(Compiler *c, const char *s) {
    int len = s ? (int)strlen(s) : 0;
    char *copy = malloc(len + 1);
    if (!copy) return -1;
    memcpy(copy, s ? s : "", len);
    copy[len] = '\0';
    return chunk_add_constant(c->current_chunk, val_str(copy));
}

/* Return the C string name for a TypeKind (used for typeof array names). */
static const char *type_kind_cname(TypeKind k) {
    switch (k) {
        case TYPE_BOOL:   return "bool";
        case TYPE_INT:    return "int";
        case TYPE_FLOAT:  return "float";
        case TYPE_STRING: return "string";
        case TYPE_SBYTE:  return "sbyte";
        case TYPE_BYTE:   return "byte";
        case TYPE_SHORT:  return "short";
        case TYPE_USHORT: return "ushort";
        case TYPE_UINT:   return "uint";
        case TYPE_LONG:   return "long";
        case TYPE_ULONG:  return "ulong";
        case TYPE_DOUBLE: return "double";
        case TYPE_CHAR:   return "char";
        case TYPE_ENUM:   return "enum";
        case TYPE_VOID:   return "void";
        default:          return "unknown";
    }
}


/* ─────────────────────────────────────────────────────────────────────────────
 * LOCAL VARIABLE MANAGEMENT
 * ───────────────────────────────────────────────────────────────────────────*/

/* Open a new lexical scope (entering a block). */
static void scope_push(Compiler *c) {
    c->scope_depth++;
}

/* Close a scope, discarding all locals declared in it.
 * Emits OP_POP for each discarded local to clean the stack. */
static void scope_pop(Compiler *c, int line) {
    c->scope_depth--;
    /* Remove all locals that were declared at the now-closed depth.
     * We walk backward so we discard in reverse declaration order. */
    while (c->local_count > 0 &&
           c->locals[c->local_count - 1].depth > c->scope_depth)
    {
        c->local_count--;
        /* Note: we do NOT emit OP_POP here because locals live in frame
         * slots, not on the value stack. The frame is discarded on RETURN.
         * We only need to pop if we put the value ON the stack separately,
         * which we don't for stored locals. */
        (void)line;
    }
}

/* Declare a new local variable in the current scope.
 * Returns the slot index assigned to it. */
static int declare_local(Compiler *c, const char *name, int length) {
    if (c->local_count >= MAX_LOCALS) {
        compile_error(c, 0, "Too many local variables in function");
        return 0;
    }
    LocalVar *local = &c->locals[c->local_count++];
    local->name   = name;
    local->length = length;
    local->slot   = c->next_slot++;
    local->depth  = c->scope_depth;
    return local->slot;
}

/* Look up a local variable by name. Returns its slot index, or -1. */
static int resolve_local(Compiler *c, const char *name, int length) {
    /* Search backward so inner scopes shadow outer ones */
    for (int i = c->local_count - 1; i >= 0; i--) {
        LocalVar *l = &c->locals[i];
        if (l->length == length && memcmp(l->name, name, length) == 0)
            return l->slot;
    }
    return -1;
}


/* ─────────────────────────────────────────────────────────────────────────────
 * CLASS HELPERS
 *
 * Look up a field or method index by name on the current class.
 * Returns -1 if not found. The compiler uses these to emit field/method
 * index operands rather than doing name lookups at runtime.
 * ───────────────────────────────────────────────────────────────────────────*/

/* Strip generic type arguments from a class name for module lookup.
 * "Stack<string>" -> "Stack", "List<int>" -> "List", "Foo" -> "Foo"
 * Writes into buf (caller provides buf/buf_size) and returns buf. */
static const char *strip_generic(const char *class_name, char *buf, size_t buf_size) {
    if (!class_name) { buf[0] = '\0'; return buf; }
    const char *lt = strchr(class_name, '<');
    if (!lt) {
        strncpy(buf, class_name, buf_size - 1);
        buf[buf_size - 1] = '\0';
        return buf;
    }
    size_t len = (size_t)(lt - class_name);
    if (len >= buf_size) len = buf_size - 1;
    memcpy(buf, class_name, len);
    buf[len] = '\0';
    return buf;
}

/* Look up a field index from a class by name, using the module's ClassDef.
 * Returns the fields[] array index (used by LOAD_STATIC/STORE_STATIC). */
static int find_field_index_by_class(Compiler *c, const char *class_name,
                                      const char *field_name, int field_len) {
    int ci = module_find_class(c->module, class_name);
    if (ci < 0) return -1;
    ClassDef *cls = &c->module->classes[ci];
    for (int i = 0; i < cls->field_count; i++) {
        if ((int)strlen(cls->fields[i].name) == field_len &&
            memcmp(cls->fields[i].name, field_name, field_len) == 0)
            return i;
    }
    return -1;
}

/* Look up the instance slot index for an instance field.
 * Returns the instance_slot (used by GET_FIELD/SET_FIELD opcodes).
 * Returns -1 if not found or if the field is static. */
static int find_instance_slot_by_class(Compiler *c, const char *class_name,
                                        const char *field_name, int field_len) {
    int ci = module_find_class(c->module, class_name);
    if (ci < 0) return -1;
    ClassDef *cls = &c->module->classes[ci];
    for (int i = 0; i < cls->field_count; i++) {
        if ((int)strlen(cls->fields[i].name) == field_len &&
            memcmp(cls->fields[i].name, field_name, field_len) == 0)
            return cls->fields[i].instance_slot;
    }
    return -1;
}

/* Look up a method's chunk index by name on a class. */
static int find_method_fn_index(Compiler *c, const char *class_name,
                                 const char *method_name, int method_len) {
    int ci = module_find_class(c->module, class_name);
    if (ci < 0) return -1;
    ClassDef *cls = &c->module->classes[ci];
    for (int i = 0; i < cls->method_count; i++) {
        if ((int)strlen(cls->methods[i].name) == method_len &&
            memcmp(cls->methods[i].name, method_name, method_len) == 0)
            return cls->methods[i].fn_index;
    }
    return -1;
}


/* ─────────────────────────────────────────────────────────────────────────────
 * EXPRESSION COMPILER
 *
 * compile_expr walks an expression node and emits bytecode that leaves
 * exactly ONE value on the stack — the result of the expression.
 *
 * The type information on each node (filled by the type checker) tells
 * us which typed instruction to emit. No runtime type checking needed.
 * ───────────────────────────────────────────────────────────────────────────*/

static void compile_expr(Compiler *c, const Expr *expr) {
    int line = expr->line;

    switch (expr->kind) {

        /* ── Literals ──────────────────────────────────────────────────── */
        case EXPR_INT_LIT:
            emit_const_int(c, expr->int_lit.value, line);
            break;

        case EXPR_CHAR_LIT:
            emit_const_int(c, (int64_t)expr->char_lit.value, line);
            break;

        case EXPR_NEW_ARRAY:
            /* new ElementType[length] — push length then emit OP_NEW_ARRAY */
            compile_expr(c, expr->new_array.length);
            emit_op(c, OP_NEW_ARRAY, line);
            break;

        case EXPR_IS: {
            /* expr is TypeName — statically typed, so evaluate at compile time.
             * Emit the operand (for side effects), pop it, push constant bool. */
            compile_expr(c, expr->type_op.operand);
            emit_op(c, OP_POP, line);  /* discard value, keep side effects */
            TypeKind src = expr->type_op.operand->resolved_type.kind;
            TypeKind tgt = expr->type_op.check_type.kind;
            /* Types match if kinds are equal, or both are int-family */
            bool int_kinds[] = {0,0,0,1,0,0,0,0,0,1,1,1,1,1,1,1,0,0,0,0}; /* TYPE_* indices */
            bool result;
            if (src == tgt) {
                result = true;
            } else if (src < 20 && tgt < 20 && int_kinds[src] && int_kinds[tgt]) {
                /* int-family cross-check: int is ushort etc — true at runtime */
                result = true;
            } else if (src == TYPE_ARRAY && tgt == TYPE_ARRAY) {
                /* array is array[] — check element type */
                Type *se = expr->type_op.operand->resolved_type.element_type;
                Type *te = expr->type_op.check_type.element_type;
                result = (se && te && se->kind == te->kind);
            } else {
                result = false;
            }
            emit_op(c, OP_LOAD_CONST_BOOL, line);
            emit_byte(c, result ? 1 : 0, line);
            break;
        }

        case EXPR_AS: {
            /* expr as TypeName -> val : emit operand then OP_AS_TYPE [tag] */
            compile_expr(c, expr->type_op.operand);

            /* Special case: casting to string is a VALUE CONVERSION, not a
             * type check. Use OP_TO_STR with the source type kind byte so the
             * VM actually converts int/float/bool to their string representation. */
            if (expr->type_op.check_type.kind == TYPE_STRING) {
                TypeKind src = expr->type_op.operand->resolved_type.kind;
                uint8_t to_str_kind;
                if (src == TYPE_INT || src == TYPE_LONG || src == TYPE_SBYTE ||
                    src == TYPE_BYTE || src == TYPE_SHORT || src == TYPE_USHORT ||
                    src == TYPE_UINT || src == TYPE_ULONG)
                    to_str_kind = 0; /* int kind in OP_TO_STR */
                else if (src == TYPE_FLOAT || src == TYPE_DOUBLE)
                    to_str_kind = 1; /* float kind */
                else if (src == TYPE_BOOL)
                    to_str_kind = 2; /* bool kind */
                else
                    to_str_kind = 255; /* string/already string → default no-op */
                emit_op(c, OP_TO_STR, line);
                emit_byte(c, to_str_kind, line);
            } else {
                emit_op(c, OP_AS_TYPE, line);
                emit_byte(c, (uint8_t)expr->type_op.check_type.kind, line);
            }
            break;
        }

        case EXPR_TYPEOF: {
            /* typeof(expr) -> Type object
             * Emit the operand, then OP_TYPEOF [tag] [name_hi] [name_lo].
             * For named types (objects, enums) we store the name as a string
             * constant and pass its index; otherwise 0xFFFF means use built-in. */
            compile_expr(c, expr->type_of.operand);
            uint8_t tag = (uint8_t)expr->type_of.operand->resolved_type.kind;
            uint16_t name_idx = 0xFFFF;
            /* For object/class/enum types, embed the name as a string const */
            TypeKind tk = expr->type_of.operand->resolved_type.kind;
            if (tk == TYPE_OBJECT || tk == TYPE_CLASS_REF) {
                const char *cname = expr->type_of.operand->resolved_type.class_name;
                if (!cname) cname = "object";
                /* store as string constant */
                int ci = add_const_str(c, cname);
                if (ci >= 0 && ci < 0xFFFF) name_idx = (uint16_t)ci;
            }
            /* For enums, embed the enum name */
            if (tk == TYPE_ENUM) {
                const char *ename = expr->type_of.operand->resolved_type.enum_name;
                if (!ename) ename = "enum";
                int ci = add_const_str(c, ename);
                if (ci >= 0 && ci < 0xFFFF) name_idx = (uint16_t)ci;
            }
            /* For arrays, embed element type name + "[]" */
            if (tk == TYPE_ARRAY) {
                const char *ename = "unknown";
                if (expr->type_of.operand->resolved_type.element_type)
                    ename = type_kind_cname(expr->type_of.operand->resolved_type.element_type->kind);
                char arr_name[64];
                snprintf(arr_name, sizeof(arr_name), "%s[]", ename);
                int ci = add_const_str(c, arr_name);
                if (ci >= 0 && ci < 0xFFFF) name_idx = (uint16_t)ci;
            }
            emit_op(c, OP_TYPEOF, line);
            emit_byte(c, tag, line);
            /* Emit name inline: [len][bytes...] len=0 means use built-in */
            if (name_idx != 0xFFFF) {
                /* name was added as const; retrieve the string we just stored */
                const char *stored = c->current_chunk->constants.values[
                    c->current_chunk->constants.count - 1].s;
                uint8_t nlen = stored ? (uint8_t)strlen(stored) : 0;
                emit_byte(c, nlen, line);
                for (uint8_t ni = 0; ni < nlen; ni++)
                    emit_byte(c, (uint8_t)stored[ni], line);
            } else {
                emit_byte(c, 0, line); /* len=0: use built-in name */
            }
            break;
        }

        case EXPR_ARRAY_LIT: {
            /* {e0, e1, ..., eN} — push all elements then OP_ARRAY_LIT count */
            int count = 0;
            for (ArgNode *elem = expr->array_lit.elements; elem; elem = elem->next) {
                compile_expr(c, elem->expr);
                count++;
            }
            emit_op(c, OP_ARRAY_LIT, line);
            emit_byte(c, (uint8_t)count, line);
            break;
        }

        case EXPR_INDEX:
            /* arr[i] — push array, push index, GET */
            compile_expr(c, expr->index_expr.array);
            compile_expr(c, expr->index_expr.index);
            emit_op(c, OP_ARRAY_GET, line);
            break;

        case EXPR_INDEX_ASSIGN:
            /* arr[i] = v — push array, push index, push value, SET */
            compile_expr(c, expr->index_assign.array);
            compile_expr(c, expr->index_assign.index);
            compile_expr(c, expr->index_assign.value);
            emit_op(c, OP_ARRAY_SET, line);
            /* ARRAY_SET doesn't leave a value — push the stored value back
             * so the assignment expression has a result (consistent with
             * how STORE_LOCAL works). */
            compile_expr(c, expr->index_assign.array);
            compile_expr(c, expr->index_assign.index);
            emit_op(c, OP_ARRAY_GET, line);
            break;

        case EXPR_FLOAT_LIT:
            emit_const_float(c, expr->float_lit.value, line);
            break;

        case EXPR_BOOL_LIT:
            /* Bools are inlined as a single byte operand — no pool entry */
            emit_op(c, OP_LOAD_CONST_BOOL, line);
            emit_byte(c, expr->bool_lit.value ? 1 : 0, line);
            break;

        case EXPR_STRING_LIT:
            emit_const_str(c, expr->string_lit.chars,
                              expr->string_lit.length, line);
            break;

        /* ── Interpolated string — $"Hello, {name}!" ────────────────────
         * Strategy: push each segment as a string, then CONCAT_STR N-1 times.
         * Empty string: push "" and done.
         * Single segment: push it and done (no concat needed).
         * N segments: push all, concat N-1 times (left-to-right). */
        case EXPR_INTERP_STRING: {
            typedef struct InterpSegment ISeg;
            int seg_count = expr->interp_string.segment_count;

            if (seg_count == 0) {
                /* Empty interpolated string — just push "" */
                emit_const_str(c, "", 0, line);
                break;
            }

            int pushed = 0;
            for (ISeg *seg = expr->interp_string.segments; seg; seg = seg->next) {
                if (!seg->is_expr) {
                    /* Text segment — push literal string */
                    emit_const_str(c, seg->text, seg->text_len, line);
                } else {
                    /* Expression segment — compile and convert to string */
                    compile_expr(c, seg->expr);
                    TypeKind tk = seg->expr->resolved_type.kind;
                    if (tk != TYPE_STRING) {
                        emit_op(c, OP_TO_STR, line);
                        if (tk == TYPE_ENUM) {
                            /* kind=4: enum name lookup, extra byte = class index */
                            emit_byte(c, 4, line);
                            int _eci = -1;
                            if (seg->expr->resolved_type.enum_name)
                                _eci = module_find_class(c->module,
                                    seg->expr->resolved_type.enum_name);
                            emit_byte(c, (uint8_t)(_eci >= 0 ? _eci : 0xFF), line);
                        } else {
                            uint8_t kind_byte =
                                (tk == TYPE_INT ||
                                 tk == TYPE_SBYTE || tk == TYPE_BYTE ||
                                 tk == TYPE_SHORT || tk == TYPE_USHORT ||
                                 tk == TYPE_UINT  || tk == TYPE_LONG ||
                                 tk == TYPE_ULONG) ? 0 :
                                (tk == TYPE_FLOAT || tk == TYPE_DOUBLE) ? 1 :
                                (tk == TYPE_CHAR) ? 3 :  /* char: print as character */
                                2; /* bool=2 */
                            emit_byte(c, kind_byte, line);
                        }
                    }
                }
                pushed++;
                /* After every second segment, concat the top two */
                if (pushed >= 2) {
                    emit_op(c, OP_CONCAT_STR, line);
                    pushed = 1; /* result is now one string on stack */
                }
            }
            break;
        }

        /* ── Variable reference ────────────────────────────────────────── */
        case EXPR_IDENT: {
            int slot = resolve_local(c, expr->ident.name, expr->ident.length);
            if (slot < 0) {
                compile_error(c, line, "Undefined variable '%.*s' at compile time",
                              expr->ident.length, expr->ident.name);
                break;
            }
            emit_op(c, OP_LOAD_LOCAL, line);
            emit_byte(c, (uint8_t)slot, line);
            break;
        }

        /* ── Unary operators ───────────────────────────────────────────── */
        case EXPR_UNARY:
            compile_expr(c, expr->unary.operand);
            if (expr->unary.op == TOK_MINUS) {
                if (expr->resolved_type.kind == TYPE_INT)
                    emit_op(c, OP_NEGATE_INT, line);
                else
                    emit_op(c, OP_NEGATE_FLOAT, line);
            } else { /* TOK_BANG */
                emit_op(c, OP_NOT_BOOL, line);
            }
            break;

        /* ── Binary operators ──────────────────────────────────────────── */
        case EXPR_BINARY: {
            TypeKind lk = expr->binary.left->resolved_type.kind;
            TypeKind rk = expr->binary.right->resolved_type.kind;
            TokenType op = expr->binary.op;

            /* String concatenation with auto-conversion.
             * OP_TO_STR takes a kind byte (0=int,1=float,2=bool,3=str) and
             * converts the top-of-stack value to its string representation. */
            if (op == TOK_PLUS &&
                (lk == TYPE_STRING || rk == TYPE_STRING))
            {
                compile_expr(c, expr->binary.left);
                if (lk != TYPE_STRING) {
                    emit_op(c, OP_TO_STR, line);
                    if (lk == TYPE_ENUM) {
                        emit_byte(c, 4, line);
                        int _eci = -1;
                        const char *_en = expr->binary.left->resolved_type.enum_name;
                        if (_en) _eci = module_find_class(c->module, _en);
                        fprintf(stderr, "DEBUG interp lk enum_name=%s eci=%d class_count=%d\n",
                                _en ? _en : "NULL", _eci, c->module->class_count);
                        emit_byte(c, (uint8_t)(_eci >= 0 ? _eci : 0xFF), line);
                    } else {
                        bool lk_int   = (lk==TYPE_INT||lk==TYPE_SBYTE||lk==TYPE_BYTE||lk==TYPE_SHORT||lk==TYPE_USHORT||lk==TYPE_UINT||lk==TYPE_LONG||lk==TYPE_ULONG);
                        bool lk_float = (lk==TYPE_FLOAT||lk==TYPE_DOUBLE);
                        emit_byte(c, (uint8_t)(lk_int ? 0 : lk_float ? 1 : lk==TYPE_CHAR ? 3 : 2), line);
                    }
                }
                compile_expr(c, expr->binary.right);
                if (rk != TYPE_STRING) {
                    emit_op(c, OP_TO_STR, line);
                    if (rk == TYPE_ENUM) {
                        emit_byte(c, 4, line);
                        int _eci = -1;
                        if (expr->binary.right->resolved_type.enum_name)
                            _eci = module_find_class(c->module, expr->binary.right->resolved_type.enum_name);
                        emit_byte(c, (uint8_t)(_eci >= 0 ? _eci : 0xFF), line);
                    } else {
                        bool rk_int   = (rk==TYPE_INT||rk==TYPE_SBYTE||rk==TYPE_BYTE||rk==TYPE_SHORT||rk==TYPE_USHORT||rk==TYPE_UINT||rk==TYPE_LONG||rk==TYPE_ULONG);
                        bool rk_float = (rk==TYPE_FLOAT||rk==TYPE_DOUBLE);
                        emit_byte(c, (uint8_t)(rk_int ? 0 : rk_float ? 1 : rk==TYPE_CHAR ? 3 : 2), line);
                    }
                }
                emit_op(c, OP_CONCAT_STR, line);
                break;
            }

            /* Compile both sides -- they'll be on the stack for the op */
            compile_expr(c, expr->binary.left);
            compile_expr(c, expr->binary.right);

            /* Use the wider of the two types for opcode selection.
             * int-family types all use INT opcodes; float/double use FLOAT.
             * This mirrors the checker's type_wider_int() rule.
             * For comparisons (which resolve to TYPE_BOOL), use the left
             * operand's type — resolved_type is always TYPE_BOOL for them. */
            TypeKind tk = expr->resolved_type.kind;
            if (tk == TYPE_BOOL) {
                TypeKind lk2 = expr->binary.left->resolved_type.kind;
                /* For unknown/generic T or object refs, use raw int comparison */
                if (lk2 != TYPE_UNKNOWN && lk2 != TYPE_OBJECT)
                    tk = lk2;
            }
            /* Map all int-family and char types to TYPE_INT for opcode selection */
            bool tk_is_int   = (tk == TYPE_INT || tk == TYPE_LONG ||
                                 tk == TYPE_SBYTE || tk == TYPE_BYTE ||
                                 tk == TYPE_SHORT || tk == TYPE_USHORT ||
                                 tk == TYPE_UINT || tk == TYPE_ULONG ||
                                 tk == TYPE_CHAR);
            bool tk_is_float = (tk == TYPE_FLOAT || tk == TYPE_DOUBLE);

            switch (op) {
                /* Arithmetic */
                case TOK_PLUS:
                    if (tk_is_int)        emit_op(c, OP_ADD_INT,   line);
                    else if (tk_is_float) emit_op(c, OP_ADD_FLOAT, line);
                    else                  emit_op(c, OP_CONCAT_STR,line);
                    break;
                case TOK_MINUS:
                    emit_op(c, tk_is_int ? OP_SUB_INT : OP_SUB_FLOAT, line);
                    break;
                case TOK_STAR:
                    emit_op(c, tk_is_int ? OP_MUL_INT : OP_MUL_FLOAT, line);
                    break;
                case TOK_SLASH:
                    emit_op(c, tk_is_int ? OP_DIV_INT : OP_DIV_FLOAT, line);
                    break;
                case TOK_PERCENT:
                    emit_op(c, tk_is_int ? OP_MOD_INT : OP_MOD_FLOAT, line);
                    break;

                /* Comparison */
                case TOK_LT:
                    emit_op(c, tk == TYPE_INT ? OP_CMP_LT_INT  : OP_CMP_LT_FLOAT,  line);
                    break;
                case TOK_LTE:
                    emit_op(c, tk == TYPE_INT ? OP_CMP_LTE_INT : OP_CMP_LTE_FLOAT, line);
                    break;
                case TOK_GT:
                    emit_op(c, tk == TYPE_INT ? OP_CMP_GT_INT  : OP_CMP_GT_FLOAT,  line);
                    break;
                case TOK_GTE:
                    emit_op(c, tk == TYPE_INT ? OP_CMP_GTE_INT : OP_CMP_GTE_FLOAT, line);
                    break;
                case TOK_EQ:
                    if      (tk == TYPE_INT || tk == TYPE_ENUM || tk == TYPE_UNKNOWN) emit_op(c, OP_CMP_EQ_INT,   line);
                    else if (tk == TYPE_FLOAT)  emit_op(c, OP_CMP_EQ_FLOAT, line);
                    else if (tk == TYPE_BOOL)   emit_op(c, OP_CMP_EQ_BOOL,  line);
                    else                        emit_op(c, OP_CMP_EQ_STR,   line);
                    break;
                case TOK_NEQ:
                    if      (tk == TYPE_INT || tk == TYPE_ENUM || tk == TYPE_UNKNOWN) emit_op(c, OP_CMP_NEQ_INT,   line);
                    else if (tk == TYPE_FLOAT)  emit_op(c, OP_CMP_NEQ_FLOAT, line);
                    else if (tk == TYPE_BOOL)   emit_op(c, OP_CMP_NEQ_BOOL,  line);
                    else                        emit_op(c, OP_CMP_NEQ_STR,   line);
                    break;

                /* Logical */
                case TOK_AND:
                    emit_op(c, OP_AND_BOOL, line);
                    break;
                case TOK_OR:
                    emit_op(c, OP_OR_BOOL, line);
                    break;

                default:
                    compile_error(c, line, "Unknown binary operator");
                    break;
            }
            break;
        }

        /* ── Postfix increment/decrement ───────────────────────────────── */
        case EXPR_POSTFIX: {
            OpCode add_or_sub = expr->postfix.op == TOK_PLUS_PLUS ? OP_ADD_INT : OP_SUB_INT;

            if (!expr->postfix.is_field) {
                /* Simple variable lvalue */
                int slot = resolve_local(c, expr->postfix.name, expr->postfix.length);
                if (slot < 0) {
                    compile_error(c, line, "Undefined variable '%.*s'",
                                  expr->postfix.length, expr->postfix.name);
                    break;
                }
                /* Load current value, add/sub 1, store back, load again (new value).
                 * For postfix: the "new value" is returned (acceptable simplification per design). */
                emit_op(c, OP_LOAD_LOCAL, line);
                emit_byte(c, (uint8_t)slot, line);
                emit_const_int(c, 1, line);
                emit_op(c, add_or_sub, line);
                emit_op(c, OP_STORE_LOCAL, line);
                emit_byte(c, (uint8_t)slot, line);
                /* Prefix returns new value; postfix also returns new value (acceptable simplification). */
                emit_op(c, OP_LOAD_LOCAL, line);
                emit_byte(c, (uint8_t)slot, line);
            } else if (expr->postfix.is_static_field) {
                /* Static field lvalue — ClassName.field++ / ClassName.field--
                 * Strategy: LOAD_STATIC, add/sub 1, STORE_STATIC, LOAD_STATIC */
                char _cname_buf[128];
                const char *cname = strip_generic(expr->postfix.object->resolved_type.class_name, _cname_buf, sizeof(_cname_buf));
                int ci = module_find_class(c->module, cname);
                if (ci < 0) {
                    compile_error(c, line, "Unknown class '%s'", cname);
                    break;
                }
                ClassDef *cls = &c->module->classes[ci];
                int fi = -1;
                for (int k = 0; k < cls->field_count; k++) {
                    if ((int)strlen(cls->fields[k].name) == expr->postfix.field_name_len &&
                        memcmp(cls->fields[k].name, expr->postfix.field_name,
                               expr->postfix.field_name_len) == 0) {
                        fi = k; break;
                    }
                }
                if (fi < 0) {
                    compile_error(c, line, "Unknown static field '%.*s' on class '%s'",
                                  expr->postfix.field_name_len,
                                  expr->postfix.field_name, cname);
                    break;
                }
                emit_op(c, OP_LOAD_STATIC, line);
                emit_byte(c, (uint8_t)ci, line);
                emit_byte(c, (uint8_t)fi, line);
                emit_const_int(c, 1, line);
                emit_op(c, add_or_sub, line);
                emit_op(c, OP_STORE_STATIC, line);
                emit_byte(c, (uint8_t)ci, line);
                emit_byte(c, (uint8_t)fi, line);
                emit_op(c, OP_LOAD_STATIC, line);
                emit_byte(c, (uint8_t)ci, line);
                emit_byte(c, (uint8_t)fi, line);
            } else {
                /* Field lvalue — obj.field++ / obj.field-- / ++obj.field / --obj.field
                 * Strategy:
                 *   compile(object)   → push obj
                 *   GET_FIELD idx     → push old value
                 *   CONST_INT 1
                 *   ADD/SUB_INT       → push new value
                 *   compile(object)   → push obj again
                 *   SET_FIELD idx     → store new value (pops obj and value)
                 *   compile(object)   → push obj for the result read
                 *   GET_FIELD idx     → push new value as expression result
                 */
                char _pfx_buf[128];
                const char *class_name = strip_generic(expr->postfix.object->resolved_type.class_name, _pfx_buf, sizeof(_pfx_buf));
                int field_idx = find_instance_slot_by_class(c, class_name,
                                    expr->postfix.field_name,
                                    expr->postfix.field_name_len);
                if (field_idx < 0) {
                    compile_error(c, line, "Unknown field '%.*s' on class '%s'",
                                  expr->postfix.field_name_len,
                                  expr->postfix.field_name, class_name);
                    break;
                }

                /* Load current field value */
                compile_expr(c, expr->postfix.object);
                emit_op(c, OP_GET_FIELD, line);
                emit_byte(c, (uint8_t)field_idx, line);

                /* Compute new value */
                emit_const_int(c, 1, line);
                emit_op(c, add_or_sub, line);

                /* Store new value back — SET_FIELD pops (obj, value) */
                /* We need to push obj first, then swap with the new value.
                 * But our VM likely expects: LOAD obj, LOAD value, SET_FIELD.
                 * The new value is currently on top. We need to interleave:
                 *   compile(object) → pushes obj on top of new_val — wrong order.
                 * Solution: store new value in a temp local, then do the SET_FIELD. */

                /* Allocate a temporary local slot — intentionally NOT released
                 * (decrementing next_slot after emitting STORE_LOCAL idx would
                 * leave the chunk's local_count below the highest slot used,
                 * causing the VM to allocate too few frame slots). The slot
                 * persists for the function's lifetime, wasting one entry per
                 * field-postfix expression — acceptable until we add OP_SWAP. */
                int tmp_slot = c->next_slot++;
                emit_op(c, OP_STORE_LOCAL, line);
                emit_byte(c, (uint8_t)tmp_slot, line);

                /* Now emit SET_FIELD: push object, push new value, SET_FIELD */
                compile_expr(c, expr->postfix.object);
                emit_op(c, OP_LOAD_LOCAL, line);
                emit_byte(c, (uint8_t)tmp_slot, line);
                emit_op(c, OP_SET_FIELD, line);
                emit_byte(c, (uint8_t)field_idx, line);

                /* Expression result: push the new value */
                emit_op(c, OP_LOAD_LOCAL, line);
                emit_byte(c, (uint8_t)tmp_slot, line);
            }
            break;
        }

        /* ── Assignment ────────────────────────────────────────────────── */
        case EXPR_ASSIGN: {
            compile_expr(c, expr->assign.value);

            /* Emit truncation if assigning into a narrow integer type */
            switch (expr->resolved_type.kind) {
                case TYPE_SBYTE:  emit_op(c, OP_TRUNC_I8,   line); break;
                case TYPE_BYTE:   emit_op(c, OP_TRUNC_U8,   line); break;
                case TYPE_SHORT:  emit_op(c, OP_TRUNC_I16,  line); break;
                case TYPE_USHORT: emit_op(c, OP_TRUNC_U16,  line); break;
                case TYPE_UINT:   emit_op(c, OP_TRUNC_U32,  line); break;
                case TYPE_ULONG:  emit_op(c, OP_TRUNC_U64,  line); break;
                case TYPE_CHAR:   emit_op(c, OP_TRUNC_CHAR, line); break;
                default: break;
            }

            /* Resolve the target slot */
            int slot = resolve_local(c, expr->assign.name, expr->assign.length);
            if (slot < 0) {
                compile_error(c, line, "Undefined variable '%.*s'",
                              expr->assign.length, expr->assign.name);
                break;
            }

            /* STORE_LOCAL pops the value and writes it to the slot.
             * But assignment is an expression that produces the assigned
             * value, so we need the value to remain on the stack.
             * Solution: load it back immediately after storing.
             * The optimizer can eliminate this later if needed. */
            emit_op(c, OP_STORE_LOCAL, line);
            emit_byte(c, (uint8_t)slot, line);
            emit_op(c, OP_LOAD_LOCAL, line);
            emit_byte(c, (uint8_t)slot, line);
            break;
        }

        /* ── Function call ─────────────────────────────────────────────── */
        case EXPR_CALL: {
            /* Check host table first so we know if args need auto-conversion */
            int host_idx = host_table_find(c->host_table,
                                           expr->call.name,
                                           expr->call.length);
            bool any_param = (host_idx >= 0 &&
                              c->host_table->decls[host_idx].has_any_param);

            /* Push all arguments onto the stack in order.
             * For TYPE_ANY host functions, auto-convert each arg to string. */
            for (ArgNode *arg = expr->call.args; arg; arg = arg->next) {
                compile_expr(c, arg->expr);
                if (any_param) {
                    TypeKind ak = arg->expr->resolved_type.kind;
                    if (ak != TYPE_STRING) {
                        emit_op(c, OP_TO_STR, line);
                        if (ak == TYPE_ENUM) {
                            /* kind=4: enum name lookup.
                             * Extra byte: class index of this enum type.
                             * Find the ClassDef by enum_name. */
                            emit_byte(c, 4, line);
                            const char *ename = arg->expr->resolved_type.enum_name;
                            int eci = ename ? module_find_class(c->module, ename) : -1;
                            emit_byte(c, (uint8_t)(eci >= 0 ? eci : 0xFF), line);
                        } else {
                            bool ak_int   = (ak==TYPE_INT||ak==TYPE_SBYTE||ak==TYPE_BYTE||
                                            ak==TYPE_SHORT||ak==TYPE_USHORT||ak==TYPE_UINT||
                                            ak==TYPE_LONG||ak==TYPE_ULONG);
                            bool ak_float = (ak==TYPE_FLOAT||ak==TYPE_DOUBLE);
                            emit_byte(c, (uint8_t)(ak_int ? 0 : ak_float ? 1 :
                                                ak==TYPE_CHAR ? 3 : 2), line);
                        }
                    }
                }
            }

            if (host_idx >= 0) {
                emit_op(c, OP_CALL_HOST, line);
                emit_u16(c, (uint16_t)c->host_table->decls[host_idx].index, line);
                emit_byte(c, (uint8_t)expr->call.arg_count, line);
                break;
            }

            /* Resolve script function index in the module */
            int fn_idx = module_find(c->module,
                                     expr->call.name,
                                     expr->call.length);
            if (fn_idx < 0) {
                compile_error(c, line, "Undefined function '%.*s'",
                              expr->call.length, expr->call.name);
                break;
            }

            /* Emit any missing default args BEFORE the opcode */
            int total_argc = emit_default_args(c, expr->call.resolved_params,
                                               expr->call.arg_count, line);
            emit_op(c, OP_CALL, line);
            emit_u16(c, (uint16_t)fn_idx, line);
            emit_byte(c, (uint8_t)total_argc, line);
            break;
        }

        /* ── Object instantiation ──────────────────────────────────────── */
        case EXPR_NEW: {
            /* Push constructor arguments */
            for (ArgNode *arg = expr->new_expr.args; arg; arg = arg->next)
                compile_expr(c, arg->expr);

            /* expr->new_expr.class_name is a raw source pointer — make a
             * null-terminated copy for module_find_class (which uses strcmp) */
            char class_name_buf[64];
            int clen = expr->new_expr.class_name_len < 63
                     ? expr->new_expr.class_name_len : 63;
            memcpy(class_name_buf, expr->new_expr.class_name, clen);
            class_name_buf[clen] = '\0';

            int ci = module_find_class(c->module, class_name_buf);
            if (ci < 0) {
                compile_error(c, line, "Unknown class '%s'", class_name_buf);
                break;
            }
            /* Emit any missing default constructor args BEFORE the opcode */
            int ctor_argc = emit_default_args(c, expr->new_expr.resolved_params,
                                              expr->new_expr.arg_count, line);
            emit_op(c, OP_NEW, line);
            emit_u16(c, (uint16_t)ci, line);
            emit_byte(c, (uint8_t)ctor_argc, line);
            break;
        }

        /* ── this ──────────────────────────────────────────────────────── */
        case EXPR_THIS:
            emit_op(c, OP_LOAD_THIS, line);
            break;

        /* ── EnumName.Member ────────────────────────────────────────────── */
        case EXPR_ENUM_ACCESS:
            /* Enums are erased to ints at compile time.
             * Emit the member's integer value as a constant. */
            emit_const_int(c, (int64_t)expr->enum_access.value, line);
            break;

        /* ── ClassName.staticField (read) ──────────────────────────────── */
        case EXPR_STATIC_GET: {
            const char *cname  = expr->static_get.class_name;
            const char *fname  = expr->static_get.field_name;
            int         flen   = expr->static_get.field_name_len;
            int ci = module_find_class(c->module, cname);
            if (ci < 0) {
                compile_error(c, line, "Unknown class '%s'", cname);
                break;
            }
            int fi = find_field_index_by_class(c, cname, fname, flen);
            if (fi < 0) {
                compile_error(c, line, "Class '%s' has no static field '%.*s'",
                              cname, flen, fname);
                break;
            }
            emit_op(c, OP_LOAD_STATIC, line);
            emit_byte(c, (uint8_t)ci, line);
            emit_byte(c, (uint8_t)fi, line);
            break;
        }

        /* ── ClassName.staticField = value (write) ─────────────────────── */
        case EXPR_STATIC_SET: {
            const char *cname  = expr->static_set.class_name;
            const char *fname  = expr->static_set.field_name;
            int         flen   = expr->static_set.field_name_len;
            compile_expr(c, expr->static_set.value);
            int ci = module_find_class(c->module, cname);
            if (ci < 0) { compile_error(c, line, "Unknown class '%s'", cname); break; }
            int fi = find_field_index_by_class(c, cname, fname, flen);
            if (fi < 0) {
                compile_error(c, line, "Class '%s' has no static field '%.*s'",
                              cname, flen, fname);
                break;
            }
            emit_op(c, OP_STORE_STATIC, line);
            emit_byte(c, (uint8_t)ci, line);
            emit_byte(c, (uint8_t)fi, line);
            /* STORE_STATIC consumes the value — push it back so the
             * expression has a value (needed when used in an assignment stmt) */
            emit_op(c, OP_LOAD_STATIC, line);
            emit_byte(c, (uint8_t)ci, line);
            emit_byte(c, (uint8_t)fi, line);
            break;
        }

        /* ── ClassName.staticMethod(args) ──────────────────────────────── */
        case EXPR_STATIC_CALL: {
            const char *cname  = expr->static_call.class_name;
            const char *mname  = expr->static_call.method_name;
            int         mlen   = expr->static_call.method_name_len;
            /* Push arguments */
            for (ArgNode *arg = expr->static_call.args; arg; arg = arg->next)
                compile_expr(c, arg->expr);
            /* Find the fn_index */
            int fn_idx = find_method_fn_index(c, cname, mname, mlen);
            if (fn_idx < 0) {
                compile_error(c, line, "Class '%s' has no static method '%.*s'",
                              cname, mlen, mname);
                break;
            }
            emit_op(c, OP_CALL, line);
            emit_u16(c, (uint16_t)fn_idx, line);
            emit_byte(c, (uint8_t)expr->static_call.arg_count, line);
            break;
        }

        /* ── super(args) ─────────────────────────────────────────────────── */
        case EXPR_SUPER_CALL: {
            /* Find the parent class's constructor fn_index */
            if (c->current_class_idx < 0) {
                compile_error(c, line, "'super' outside class context");
                break;
            }
            ClassDef *cur_cls = &c->module->classes[c->current_class_idx];
            int par_ci = cur_cls->parent_index;
            if (par_ci < 0) {
                compile_error(c, line, "'super' called in class with no parent");
                break;
            }
            ClassDef *par_cls = &c->module->classes[par_ci];
            if (par_cls->constructor_index < 0) {
                /* Parent has no constructor — super() is a no-op */
                break;
            }
            /* Push args only — 'this' is already slot 0 of the current frame.
             * OP_CALL_SUPER reuses the current 'this' rather than popping a
             * new object off the stack, avoiding the stack imbalance that
             * OP_CALL_METHOD would cause when the parent constructor returns. */
            for (ArgNode *arg = expr->super_call.args; arg; arg = arg->next)
                compile_expr(c, arg->expr);
            emit_op(c, OP_CALL_SUPER, line);
            emit_u16(c, (uint16_t)par_cls->constructor_index, line);
            emit_byte(c, (uint8_t)expr->super_call.arg_count, line);
            break;
        }

        /* ── obj.field (read) ───────────────────────────────────────────── */
        case EXPR_FIELD_GET: {
            /* Type object field access: typeof(x).name / .isArray / etc */
            if (expr->field_get.object->resolved_type.kind == TYPE_CLASS_REF &&
                strncmp(expr->field_get.object->resolved_type.class_name, "Type", 4) == 0 &&
                expr->field_get.object->resolved_type.class_name[4] == '\0') {
                compile_expr(c, expr->field_get.object);
                /* Map field name to field_id byte */
                const char *fn = expr->field_get.field_name;
                int flen = expr->field_get.field_name_len;
                uint8_t fid = 0xFF;
                if      (flen==4 && strncmp(fn,"name",4)==0)        fid=0;
                else if (flen==7 && strncmp(fn,"isArray",7)==0)     fid=1;
                else if (flen==11 && strncmp(fn,"isPrimitive",11)==0) fid=2;
                else if (flen==6 && strncmp(fn,"isEnum",6)==0)      fid=3;
                else if (flen==7 && strncmp(fn,"isClass",7)==0)     fid=4;
                else {
                    compile_error(c, line, "Type has no field '%.*s'", flen, fn);
                    break;
                }
                emit_op(c, OP_TYPE_FIELD, line);
                emit_byte(c, fid, line);
                break;
            }

            /* Array .length special case */
            if (expr->field_get.object->resolved_type.kind == TYPE_ARRAY) {
                compile_expr(c, expr->field_get.object);
                emit_op(c, OP_ARRAY_LEN, line);
                break;
            }

            compile_expr(c, expr->field_get.object);

            /* Determine which class this is */
            const char *class_name = expr->field_get.object->resolved_type.class_name;
            int field_idx = find_instance_slot_by_class(c, class_name,
                                expr->field_get.field_name,
                                expr->field_get.field_name_len);
            if (field_idx < 0) {
                compile_error(c, line, "Unknown field '%.*s' on class '%s'",
                              expr->field_get.field_name_len,
                              expr->field_get.field_name, class_name);
                break;
            }
            emit_op(c, OP_GET_FIELD, line);
            emit_byte(c, (uint8_t)field_idx, line);
            break;
        }

        /* ── obj.field = value (write) ─────────────────────────────────── */
        case EXPR_FIELD_SET: {
            compile_expr(c, expr->field_set.object);
            compile_expr(c, expr->field_set.value);

            char _fs_buf[128];
            const char *class_name = strip_generic(expr->field_set.object->resolved_type.class_name, _fs_buf, sizeof(_fs_buf));
            int field_idx = find_instance_slot_by_class(c, class_name,
                                expr->field_set.field_name,
                                expr->field_set.field_name_len);
            if (field_idx < 0) {
                compile_error(c, line, "Unknown field '%.*s' on class '%s'",
                              expr->field_set.field_name_len,
                              expr->field_set.field_name, class_name);
                break;
            }
            emit_op(c, OP_SET_FIELD, line);
            emit_byte(c, (uint8_t)field_idx, line);
            /* FIELD_SET leaves nothing on stack — but as an expression
             * it should produce the value. Reload the object and field. */
            compile_expr(c, expr->field_set.object);
            emit_op(c, OP_GET_FIELD, line);
            emit_byte(c, (uint8_t)field_idx, line);
            break;
        }

        /* ── obj.method(args) ───────────────────────────────────────────── */
        case EXPR_METHOD_CALL: {
            /* Push receiver object first, then arguments */
            compile_expr(c, expr->method_call.object);
            for (ArgNode *arg = expr->method_call.args; arg; arg = arg->next)
                compile_expr(c, arg->expr);

            const char *class_name_raw = expr->method_call.object->resolved_type.class_name;
            char class_name_buf[128];
            const char *class_name = strip_generic(class_name_raw, class_name_buf, sizeof(class_name_buf));
            int ci = module_find_class(c->module, class_name);

            if (ci < 0) {
                /* class_name is an interface — emit a virtual dispatch.
                 * Store the method name as a string constant; the VM
                 * looks up the concrete fn_index on the actual object. */
                char mname_buf[256];
                int mlen = expr->method_call.method_name_len < 255
                         ? expr->method_call.method_name_len : 255;
                memcpy(mname_buf, expr->method_call.method_name, mlen);
                mname_buf[mlen] = '\0';
                int name_idx = add_const_str(c, mname_buf);
                int iface_argc = emit_default_args(c, expr->method_call.resolved_params,
                                                   expr->method_call.arg_count, line);
                emit_op(c, OP_CALL_IFACE, line);
                emit_u16(c, (uint16_t)name_idx, line);
                emit_byte(c, (uint8_t)iface_argc, line);
                break;
            }

            int fn_idx = find_method_fn_index(c, class_name,
                             expr->method_call.method_name,
                             expr->method_call.method_name_len);
            if (fn_idx < 0) {
                compile_error(c, line, "Unknown method '%.*s' on class '%s'",
                              expr->method_call.method_name_len,
                              expr->method_call.method_name, class_name);
                break;
            }
            int method_argc = emit_default_args(c, expr->method_call.resolved_params,
                                                expr->method_call.arg_count, line);
            emit_op(c, OP_CALL_METHOD, line);
            emit_u16(c, (uint16_t)fn_idx, line);
            emit_byte(c, (uint8_t)method_argc, line);
            break;
        }
    }
}


/* ─────────────────────────────────────────────────────────────────────────────
 * STATEMENT COMPILER
 * ───────────────────────────────────────────────────────────────────────────*/

static void compile_stmt(Compiler *c, const Stmt *stmt);

static void compile_block(Compiler *c, const Stmt *block) {
    scope_push(c);
    for (StmtNode *n = block->block.stmts; n; n = n->next)
        compile_stmt(c, n->stmt);
    scope_pop(c, block->line);
}

static void compile_stmt(Compiler *c, const Stmt *stmt) {
    int line = stmt->line;

    switch (stmt->kind) {

        /* ── Variable declaration ──────────────────────────────────────── */
        case STMT_VAR_DECL: {
            TypeKind vtype = stmt->var_decl.type.kind;
            /* Compile the initializer if present, else push a zero value */
            if (stmt->var_decl.init) {
                compile_expr(c, stmt->var_decl.init);
            } else {
                /* Default zero-initialize based on declared type */
                switch (vtype) {
                    case TYPE_INT:
                    case TYPE_SBYTE: case TYPE_BYTE:
                    case TYPE_SHORT: case TYPE_USHORT:
                    case TYPE_UINT:  case TYPE_LONG:
                    case TYPE_ULONG: case TYPE_CHAR:
                        emit_const_int(c, 0, line); break;
                    case TYPE_ARRAY:
                        /* Default: empty zero-length array */
                        emit_const_int(c, 0, line);
                        emit_op(c, OP_NEW_ARRAY, line);
                        break;
                    case TYPE_FLOAT:
                    case TYPE_DOUBLE:
                        emit_const_float(c, 0.0, line); break;
                    case TYPE_BOOL:
                        emit_op(c, OP_LOAD_CONST_BOOL, line);
                        emit_byte(c, 0, line);
                        break;
                    case TYPE_STRING: emit_const_str(c, "", 0, line); break;
                    default: break;
                }
            }

            /* Emit truncation for narrow integer types so overflow wraps correctly */
            switch (vtype) {
                case TYPE_SBYTE:  emit_op(c, OP_TRUNC_I8,   line); break;
                case TYPE_BYTE:   emit_op(c, OP_TRUNC_U8,   line); break;
                case TYPE_SHORT:  emit_op(c, OP_TRUNC_I16,  line); break;
                case TYPE_USHORT: emit_op(c, OP_TRUNC_U16,  line); break;
                case TYPE_UINT:   emit_op(c, OP_TRUNC_U32,  line); break;
                case TYPE_ULONG:  emit_op(c, OP_TRUNC_U64,  line); break;
                case TYPE_CHAR:   emit_op(c, OP_TRUNC_CHAR, line); break;
                default: break;
            }

            /* Declare the local and store the value into its slot */
            int slot = declare_local(c, stmt->var_decl.name,
                                        stmt->var_decl.length);
            emit_op(c, OP_STORE_LOCAL, line);
            emit_byte(c, (uint8_t)slot, line);
            break;
        }

        /* ── Expression statement ──────────────────────────────────────── */
        case STMT_EXPR: {
            compile_expr(c, stmt->expr.expr);
            /* Expression statements discard the result value.
             * EXPR_SUPER_CALL produces no stack value (constructor return
             * suppresses the dummy push), so skip the POP for it. */
            ExprKind k = stmt->expr.expr->kind;
            if (k != EXPR_SUPER_CALL) {
                emit_op(c, OP_POP, line);
            }
            break;
        }

        /* ── Block ─────────────────────────────────────────────────────── */
        case STMT_BLOCK:
            compile_block(c, stmt);
            break;

        /* ── If statement ──────────────────────────────────────────────── */
        case STMT_IF: {
            /* Compile condition — leaves a bool on the stack */
            compile_expr(c, stmt->if_stmt.condition);

            /* Jump past then-branch if false */
            int then_jump = emit_jump(c, OP_JUMP_IF_FALSE, line);

            /* Compile then-branch */
            compile_block(c, stmt->if_stmt.then_branch);

            if (stmt->if_stmt.else_branch) {
                /* Jump past else-branch at end of then-branch */
                int else_jump = emit_jump(c, OP_JUMP, line);

                /* Patch then-jump to land here (start of else) */
                patch_jump(c, then_jump);

                /* Compile else-branch */
                if (stmt->if_stmt.else_branch->kind == STMT_BLOCK)
                    compile_block(c, stmt->if_stmt.else_branch);
                else
                    compile_stmt(c, stmt->if_stmt.else_branch);

                /* Patch else-jump to land here (after else) */
                patch_jump(c, else_jump);
            } else {
                /* No else — patch then-jump to land right here */
                patch_jump(c, then_jump);
            }
            break;
        }

        /* ── While loop ────────────────────────────────────────────────── */
        case STMT_WHILE: {
            int depth = c->loop_depth;
            c->break_count[depth]    = 0;
            c->continue_count[depth] = 0;

            /* Record where the loop condition starts */
            int loop_start = c->current_chunk->count;
            c->loop_start  = loop_start;
            c->loop_depth++;

            /* Compile condition */
            compile_expr(c, stmt->while_stmt.condition);

            /* Jump past body if false */
            int exit_jump = emit_jump(c, OP_JUMP_IF_FALSE, line);

            /* Compile body */
            compile_block(c, stmt->while_stmt.body);

            /* Jump back to condition */
            emit_loop(c, loop_start, line);

            /* Patch exit jump to land after the loop */
            patch_jump(c, exit_jump);

            c->loop_depth--;

            /* Patch all break jumps to land here (after loop) */
            for (int i = 0; i < c->break_count[depth]; i++)
                patch_jump(c, c->break_patches[depth][i]);

            /* Patch all continue jumps to land at loop_start */
            for (int i = 0; i < c->continue_count[depth]; i++) {
                int pos    = c->continue_patches[depth][i];
                int offset = loop_start - (pos + 2);
                c->current_chunk->code[pos]     = (offset >> 8) & 0xFF;
                c->current_chunk->code[pos + 1] =  offset       & 0xFF;
            }
            break;
        }

        /* ── For loop ──────────────────────────────────────────────────── */
        case STMT_FOR: {
            int depth = c->loop_depth;
            c->break_count[depth]    = 0;
            c->continue_count[depth] = 0;

            /* For loop introduces its own scope for the init variable */
            scope_push(c);

            /* Init */
            if (stmt->for_stmt.init)
                compile_stmt(c, stmt->for_stmt.init);

            /* Condition */
            int loop_start = c->current_chunk->count;
            int exit_jump  = -1;

            if (stmt->for_stmt.condition) {
                compile_expr(c, stmt->for_stmt.condition);
                exit_jump = emit_jump(c, OP_JUMP_IF_FALSE, line);
            }

            /* Body */
            c->loop_depth++;
            compile_block(c, stmt->for_stmt.body);
            c->loop_depth--;

            /* Step — continue jumps land HERE (before the step runs) */
            int continue_target = c->current_chunk->count;
            if (stmt->for_stmt.step) {
                compile_expr(c, stmt->for_stmt.step);
                emit_op(c, OP_POP, line);  /* step result is discarded */
            }

            /* Jump back to condition */
            emit_loop(c, loop_start, line);

            /* Patch exit jump */
            if (exit_jump >= 0) patch_jump(c, exit_jump);

            scope_pop(c, line);

            /* Patch all break jumps to land here (after loop) */
            for (int i = 0; i < c->break_count[depth]; i++)
                patch_jump(c, c->break_patches[depth][i]);

            /* Patch all continue jumps to land at continue_target (before step) */
            for (int i = 0; i < c->continue_count[depth]; i++) {
                int pos    = c->continue_patches[depth][i];
                int offset = continue_target - (pos + 2);
                c->current_chunk->code[pos]     = (offset >> 8) & 0xFF;
                c->current_chunk->code[pos + 1] =  offset       & 0xFF;
            }
            break;
        }

        case STMT_FOREACH: {
            int depth = c->loop_depth;
            c->break_count[depth] = 0; c->continue_count[depth] = 0;
            scope_push(c);
            compile_expr(c, stmt->foreach_stmt.array);
            int arr_slot = declare_local(c, "__arr", 5);
            emit_op(c, OP_STORE_LOCAL, line); emit_byte(c, (uint8_t)arr_slot, line);
            emit_const_int(c, 0, line);
            int idx_slot = declare_local(c, "__i", 2);
            emit_op(c, OP_STORE_LOCAL, line); emit_byte(c, (uint8_t)idx_slot, line);
            int loop_start = c->current_chunk->count;
            emit_op(c, OP_LOAD_LOCAL, line); emit_byte(c, (uint8_t)idx_slot, line);
            emit_op(c, OP_LOAD_LOCAL, line); emit_byte(c, (uint8_t)arr_slot, line);
            emit_op(c, OP_ARRAY_LEN, line); emit_op(c, OP_CMP_LT_INT, line);
            int exit_jump = emit_jump(c, OP_JUMP_IF_FALSE, line);
            emit_op(c, OP_LOAD_LOCAL, line); emit_byte(c, (uint8_t)arr_slot, line);
            emit_op(c, OP_LOAD_LOCAL, line); emit_byte(c, (uint8_t)idx_slot, line);
            emit_op(c, OP_ARRAY_GET, line);
            int var_slot = declare_local(c, stmt->foreach_stmt.var_name, stmt->foreach_stmt.var_len);
            emit_op(c, OP_STORE_LOCAL, line); emit_byte(c, (uint8_t)var_slot, line);
            c->loop_depth++; compile_block(c, stmt->foreach_stmt.body); c->loop_depth--;
            int continue_target = c->current_chunk->count;
            emit_op(c, OP_LOAD_LOCAL, line); emit_byte(c, (uint8_t)idx_slot, line);
            emit_const_int(c, 1, line); emit_op(c, OP_ADD_INT, line);
            emit_op(c, OP_STORE_LOCAL, line); emit_byte(c, (uint8_t)idx_slot, line);
            emit_loop(c, loop_start, line); patch_jump(c, exit_jump); scope_pop(c, line);
            for (int i = 0; i < c->break_count[depth]; i++) patch_jump(c, c->break_patches[depth][i]);
            for (int i = 0; i < c->continue_count[depth]; i++) {
                int pos = c->continue_patches[depth][i], off = continue_target-(pos+2);
                c->current_chunk->code[pos] = (off>>8)&0xFF; c->current_chunk->code[pos+1] = off&0xFF;
            }
            break;
        }

        /* ── Return ────────────────────────────────────────────────────── */
        case STMT_RETURN:
            if (stmt->return_stmt.value) {
                compile_expr(c, stmt->return_stmt.value);
                emit_op(c, OP_RETURN, line);
            } else {
                emit_op(c, OP_RETURN_VOID, line);
            }
            break;

        /* ── Break / Continue ───────────────────────────────────────────── */
        case STMT_BREAK:
            /* break is valid inside both loops and match arms —
             * both use loop_depth to track the break-patch level */
            if (c->loop_depth == 0) {
                compile_error(c, line, "'break' outside of loop or match");
                break;
            } else {
                int depth = c->loop_depth - 1;
                if (c->break_count[depth] >= MAX_LOOP_PATCHES) {
                    compile_error(c, line, "Too many break statements");
                    break;
                }
                int patch = emit_jump(c, OP_JUMP, line);
                c->break_patches[depth][c->break_count[depth]++] = patch;
            }
            break;

        /* ── Match statement ─────────────────────────────────────────────
         * Compiles to a linear chain of comparisons:
         *
         *   LOAD subject
         *   STORE_LOCAL temp
         *
         *   ; arm 0
         *   LOAD temp; LOAD pattern0; CMP_EQ; JUMP_IF_FALSE → cmp1
         *   arm0_body
         *   (break → JUMP match_end)
         *   cmp1:
         *
         *   ; arm 1
         *   ...
         *
         *   ; default (if present)
         *   default_body
         *   (break → JUMP match_end)
         *
         *   ; no-match fallthrough (only if no default)
         *   MATCH_FAIL
         *
         *   match_end:
         * ──────────────────────────────────────────────────────────────── */
        case STMT_MATCH: {
            typedef struct MatchArmNode MANode;

            int depth = c->loop_depth;
            c->break_count[depth] = 0;
            c->loop_depth++;

            /* Compile subject and stash it in a temp local inside its own scope */
            scope_push(c);
            compile_expr(c, stmt->match_stmt.subject);
            int temp_slot = declare_local(c, "$match_subj", 11);
            emit_op(c, OP_STORE_LOCAL, line);
            emit_byte(c, (uint8_t)temp_slot, line);

            /* Fall-through correctness requires two separate jump targets per arm:
             *   comp_N  — start of arm N's comparison (JUMP_IF_FALSE from arm N-1 lands here)
             *   body_N  — start of arm N's body      (fall-through from arm N-1 lands here)
             *
             * We track two sets of pending patches:
             *   pending_cmp_skip   — JUMP_IF_FALSE from the current arm's failed
             *                        comparison; patched to comp_{N+1} at the TOP
             *                        of the next iteration.
             *   pending_fallthrough — unconditional JUMP emitted when an arm body
             *                        ends WITHOUT a break; patched to body_{N+1}
             *                        AFTER the next arm's comparison is emitted
             *                        (i.e. just before body_{N+1} starts).
             *
             * Layout per arm:
             *   comp_N:   LOAD temp; LOAD pattern; CMP_EQ; JUMP_IF_FALSE -> comp_{N+1}
             *   body_N:   <statements>
             *             [if fall-through: JUMP -> body_{N+1}]
             *             [if break:        JUMP -> match_end  (via break_patches)]
             */
            int pending_cmp_skip    = -1;   /* JUMP_IF_FALSE awaiting comp_{N+1} */
            int pending_fallthrough = -1;   /* JUMP awaiting body_{N+1}          */
            TypeKind subject_kind = stmt->match_stmt.subject->resolved_type.kind;

            for (MANode *arm = stmt->match_stmt.arms; arm; arm = arm->next) {
                /* ── Start of this arm's comparison ──────────────────────────
                 * Patch the previous arm's failed comparison jump to HERE. */
                if (pending_cmp_skip >= 0) {
                    patch_jump(c, pending_cmp_skip);
                    pending_cmp_skip = -1;
                }

                if (!arm->is_default) {
                    /* Emit comparison */
                    emit_op(c, OP_LOAD_LOCAL, line);
                    emit_byte(c, (uint8_t)temp_slot, line);
                    compile_expr(c, arm->pattern);
                    if (subject_kind == TYPE_INT || subject_kind == TYPE_ENUM)
                        emit_op(c, OP_CMP_EQ_INT, line);
                    else
                        emit_op(c, OP_CMP_EQ_INT, line);
                    pending_cmp_skip = emit_jump(c, OP_JUMP_IF_FALSE, line);
                }

                /* ── Start of this arm's body ─────────────────────────────────
                 * Patch any pending fall-through jump from the PREVIOUS arm to HERE
                 * (i.e. past this arm's comparison, directly into its body). */
                if (pending_fallthrough >= 0) {
                    patch_jump(c, pending_fallthrough);
                    pending_fallthrough = -1;
                }

                /* Compile arm body */
                int break_count_before = c->break_count[depth];
                compile_block(c, arm->body);

                /* Detect fall-through: if no new break was emitted by this body,
                 * the arm has no break — emit a forward JUMP to next body (fall-through).
                 * If a break WAS emitted, execution leaves via that jump; the
                 * fall-through JUMP is still emitted but will be dead code after
                 * the break (since break jumps past the whole match, the
                 * fall-through JUMP is unreachable and harmless).
                 *
                 * Actually: we always emit the fall-through JUMP. If there's a
                 * break, execution never reaches it. If there's no break, it jumps
                 * to body_{N+1}. This avoids needing to inspect the body for breaks.
                 * Exception: the last arm needs no fall-through (there is no next body). */
                if (arm->next) {
                    pending_fallthrough = emit_jump(c, OP_JUMP, line);
                }
                (void)break_count_before;
            }

            /* Patch the last failed-comparison jump to land here (no-match zone) */
            if (pending_cmp_skip >= 0) {
                patch_jump(c, pending_cmp_skip);
            }
            /* Any remaining pending fall-through also lands here
             * (last arm has no break and no next arm — falls into no-match zone) */
            if (pending_fallthrough >= 0) {
                patch_jump(c, pending_fallthrough);
            }

            /* If no default, a no-match is a runtime error */
            if (!stmt->match_stmt.has_default) {
                emit_op(c, OP_MATCH_FAIL, line);
            }

            /* match_end: patch all break jumps to land here */
            c->loop_depth--;
            for (int i = 0; i < c->break_count[depth]; i++)
                patch_jump(c, c->break_patches[depth][i]);

            /* Release the temp local's scope */
            scope_pop(c, line);
            break;
        }

        case STMT_CONTINUE:
            if (c->loop_depth == 0) {
                compile_error(c, line, "'continue' outside of loop");
                break;
            } else {
                int depth = c->loop_depth - 1;
                if (c->continue_count[depth] >= MAX_LOOP_PATCHES) {
                    compile_error(c, line, "Too many continue statements in one loop");
                    break;
                }
                int patch = emit_jump(c, OP_JUMP, line);
                c->continue_patches[depth][c->continue_count[depth]++] = patch;
            }
            break;

        /* ── Function declaration inside block — forbidden ─────────────── */
        case STMT_FN_DECL:
            compile_error(c, line,
                "Function declarations must be at the top level");
            break;

        /* ── Class declaration inside block — forbidden ──────────────────── */
        case STMT_CLASS_DECL:
            compile_error(c, line,
                "Class declarations must be at the top level");
            break;

        /* ── Enum declaration inside block — forbidden ───────────────────── */
        case STMT_ENUM_DECL:
            compile_error(c, line,
                "Enum declarations must be at the top level");
            break;

        case STMT_INTERFACE_DECL:
            /* Interfaces are compile-time only — nothing to emit */
            break;

        case STMT_IMPORT:
            /* Import declarations are fully resolved before compilation —
             * the imported source was merged into the program. Nothing to emit. */
            break;
    }
}


/* ─────────────────────────────────────────────────────────────────────────────
 * FUNCTION COMPILER
 *
 * Compiles a single function declaration into a new Chunk in the module.
 * ───────────────────────────────────────────────────────────────────────────*/



/* ─────────────────────────────────────────────────────────────────────────────
 * PUBLIC INTERFACE
 * ───────────────────────────────────────────────────────────────────────────*/

void compiler_host_table_init(CompilerHostTable *t) {
    t->count = 0;
}

void compiler_host_table_add(CompilerHostTable *t,
                             const char *name, int index, int param_count) {
    if (t->count >= COMPILER_MAX_HOST_DECLS) return;
    CompilerHostDecl *d = &t->decls[t->count++];
    d->name          = name;
    d->index         = index;
    d->param_count   = param_count;
    d->has_any_param = false;
}

void compiler_host_table_add_any(CompilerHostTable *t,
                                 const char *name, int index, int param_count) {
    if (t->count >= COMPILER_MAX_HOST_DECLS) return;
    CompilerHostDecl *d = &t->decls[t->count++];
    d->name          = name;
    d->index         = index;
    d->param_count   = param_count;
    d->has_any_param = true;
}

static int host_table_find(const CompilerHostTable *t,
                           const char *name, int len) {
    if (!t) return -1;
    for (int i = 0; i < t->count; i++) {
        if ((int)strlen(t->decls[i].name) == len &&
            memcmp(t->decls[i].name, name, len) == 0)
            return i;
    }
    return -1;
}

bool compiler_compile(Compiler *c, const Program *program, Module *module,
                      const CompilerHostTable *host_table) {
    /* Initialize compiler state */
    memset(c, 0, sizeof(Compiler));
    c->module             = module;
    c->host_table         = host_table;
    c->current_chunk      = NULL;
    c->had_error          = false;
    c->error_count        = 0;
    c->current_class_idx  = -1;
    c->current_class_ast  = NULL;

    /* ── PASS 1: Register all names ─────────────────────────────────────────
     *
     * Walk top-level declarations and pre-allocate:
     *   - One chunk per top-level function
     *   - One ClassDef per class, plus one chunk per method/constructor
     *
     * This makes forward references work in both directions.
     * ────────────────────────────────────────────────────────────────────*/
    for (StmtNode *n = program->stmts; n; n = n->next) {
        Stmt *s = n->stmt;

        if (s->kind == STMT_IMPORT) continue;

        if (s->kind == STMT_FN_DECL) {
            if (c->module->count >= MODULE_MAX_FUNCTIONS) {
                compile_error(c, s->line, "Too many functions");
                break;
            }
            int fn_idx = module_add_chunk(c->module);
            int namelen = s->fn_decl.length < 63 ? s->fn_decl.length : 63;
            memcpy(c->module->names[fn_idx], s->fn_decl.name, namelen);
            c->module->names[fn_idx][namelen] = '\0';
        }
        else if (s->kind == STMT_CLASS_DECL) {
            if (c->module->class_count >= MODULE_MAX_CLASSES) {
                compile_error(c, s->line, "Too many classes");
                break;
            }

            int ci = c->module->class_count++;
            ClassDef *cls = &c->module->classes[ci];
            memset(cls, 0, sizeof(ClassDef));

            int namelen = s->class_decl.length < CLASS_NAME_MAX - 1
                        ? s->class_decl.length : CLASS_NAME_MAX - 1;
            memcpy(cls->name, s->class_decl.name, namelen);
            cls->name[namelen] = '\0';
            cls->constructor_index = -1;
            cls->parent_index      = -1;

            /* ── Populate ModMetadata from @Mod annotation ──────────────── */
            for (AnnotationNode *ann = s->class_decl.annotations; ann; ann = ann->next) {
                if (ann->name_len != 3 || memcmp(ann->name, "Mod", 3) != 0) continue;

                c->module->metadata.has_mod = true;

                /* entry_class = this class's name */
                strncpy(c->module->metadata.entry_class, cls->name, MOD_STRING_MAX - 1);
                c->module->metadata.entry_class[MOD_STRING_MAX - 1] = '\0';

                /* Extract ModMetadata from @Mod annotation args.
                 * Supports both positional and named args.
                 * Positional order: name, version, author, description.
                 * Named: name=, version=, author=, description= */
                int pos_idx = 0;
                for (AnnotationKVNode *kv = ann->args; kv; kv = kv->next) {
                    /* Determine which field this arg maps to */
                    char *dest = NULL;
                    if (kv->key == NULL) {
                        /* positional */
                        if      (pos_idx == 0) dest = c->module->metadata.name;
                        else if (pos_idx == 1) dest = c->module->metadata.version;
                        else if (pos_idx == 2) dest = c->module->metadata.author;
                        else if (pos_idx == 3) dest = c->module->metadata.description;
                        pos_idx++;
                    } else {
                        /* named */
                        if      (kv->key_len == 4  && memcmp(kv->key, "name",        4)  == 0) dest = c->module->metadata.name;
                        else if (kv->key_len == 7  && memcmp(kv->key, "version",     7)  == 0) dest = c->module->metadata.version;
                        else if (kv->key_len == 6  && memcmp(kv->key, "author",      6)  == 0) dest = c->module->metadata.author;
                        else if (kv->key_len == 11 && memcmp(kv->key, "description", 11) == 0) dest = c->module->metadata.description;
                    }
                    if (!dest) continue;

                    /* Extract string value from the expression */
                    if (kv->value && kv->value->kind == EXPR_STRING_LIT) {
                        const char *vstart = kv->value->string_lit.chars;
                        int         vlen   = kv->value->string_lit.length;
                        if (vlen > MOD_STRING_MAX - 1) vlen = MOD_STRING_MAX - 1;
                        memcpy(dest, vstart, vlen);
                        dest[vlen] = '\0';
                    }
                }
            }

            /* ── Flatten parent fields and methods into this ClassDef ──────
             * Copy ancestor members first so their field indices come before
             * the child's own fields — giving a stable, predictable layout.
             * Parent ClassDef must already be registered (Pass 1 is ordered
             * so that forward-declared parents work because the module is
             * built in source order; parents should be declared first).   */
            if (s->class_decl.parent_name && s->class_decl.parent_length > 0) {
                char par_name[CLASS_NAME_MAX];
                int  par_len = s->class_decl.parent_length < CLASS_NAME_MAX - 1
                             ? s->class_decl.parent_length : CLASS_NAME_MAX - 1;
                memcpy(par_name, s->class_decl.parent_name, par_len);
                par_name[par_len] = '\0';

                int par_ci = module_find_class(c->module, par_name);
                if (par_ci >= 0) {
                    cls->parent_index = par_ci;
                    ClassDef *par_cls = &c->module->classes[par_ci];

                    /* Copy parent fields first */
                    for (int fi = 0; fi < par_cls->field_count; fi++) {
                        if (cls->field_count >= CLASS_MAX_FIELDS) {
                            compile_error(c, s->line,
                                "Class '%s': too many fields after inheriting from '%s'",
                                cls->name, par_name);
                            break;
                        }
                        cls->fields[cls->field_count++] = par_cls->fields[fi];
                    }

                    /* Copy parent methods — child can override by same name */
                    for (int mi = 0; mi < par_cls->method_count; mi++) {
                        if (cls->method_count >= CLASS_MAX_METHODS) {
                            compile_error(c, s->line,
                                "Class '%s': too many methods after inheriting from '%s'",
                                cls->name, par_name);
                            break;
                        }
                        cls->methods[cls->method_count++] = par_cls->methods[mi];
                    }

                    /* Inherit constructor as fallback (overridden below if child declares one) */
                    cls->constructor_index = par_cls->constructor_index;
                }
            }

            /* Register fields (both instance and static).
             * instance_slot tracks the consecutive slot index for instance
             * fields only — this is what GET_FIELD/SET_FIELD use at runtime.
             * Static fields get instance_slot = -1. */
            typedef struct ClassFieldNode CFNode;
            int instance_slot = 0;
            for (CFNode *f = s->class_decl.fields; f; f = f->next) {
                if (cls->field_count >= CLASS_MAX_FIELDS) {
                    compile_error(c, s->line, "Class '%s': too many fields", cls->name);
                    break;
                }
                FieldDef *fd = &cls->fields[cls->field_count++];
                int flen = f->length < FIELD_NAME_MAX - 1 ? f->length : FIELD_NAME_MAX - 1;
                memcpy(fd->name, f->name, flen);
                fd->name[flen]  = '\0';
                fd->type_kind   = f->type.kind;
                fd->is_static   = f->is_static;
                fd->instance_slot = f->is_static ? -1 : instance_slot++;
                if (f->type.kind == TYPE_OBJECT && f->type.class_name) {
                    int cnlen = (int)strlen(f->type.class_name);
                    cnlen = cnlen < CLASS_NAME_MAX - 1 ? cnlen : CLASS_NAME_MAX - 1;
                    memcpy(fd->class_name, f->type.class_name, cnlen);
                    fd->class_name[cnlen] = '\0';
                }
            }

            /* Register methods — allocate a chunk for each */
            typedef struct ClassMethodNode CMNode;
            for (CMNode *m = s->class_decl.methods; m; m = m->next) {
                if (c->module->count >= MODULE_MAX_FUNCTIONS) {
                    compile_error(c, s->line, "Too many functions (in class methods)");
                    break;
                }

                int fn_idx = module_add_chunk(c->module);

                /* Build a unique mangled name: "ClassName.methodName" */
                char mangled[128];
                snprintf(mangled, sizeof(mangled), "%s.%.*s",
                         cls->name,
                         m->fn->fn_decl.length, m->fn->fn_decl.name);
                int mnlen = (int)strlen(mangled);
                mnlen = mnlen < 63 ? mnlen : 63;
                memcpy(c->module->names[fn_idx], mangled, mnlen);
                c->module->names[fn_idx][mnlen] = '\0';

                if (m->is_constructor) {
                    cls->constructor_index = fn_idx;
                } else {
                    /* Check if this method overrides an inherited one */
                    int mlen = m->fn->fn_decl.length < FIELD_NAME_MAX - 1
                             ? m->fn->fn_decl.length : FIELD_NAME_MAX - 1;
                    int override_slot = -1;
                    for (int mi = 0; mi < cls->method_count; mi++) {
                        if ((int)strlen(cls->methods[mi].name) == mlen &&
                            memcmp(cls->methods[mi].name, m->fn->fn_decl.name, mlen) == 0) {
                            override_slot = mi;
                            break;
                        }
                    }
                    if (override_slot >= 0) {
                        /* Override: replace the inherited entry in-place */
                        cls->methods[override_slot].fn_index  = fn_idx;
                        cls->methods[override_slot].is_static = m->is_static;
                    } else {
                        /* New method: append */
                        if (cls->method_count >= CLASS_MAX_METHODS) {
                            compile_error(c, s->line, "Class '%s': too many methods", cls->name);
                            break;
                        }
                        MethodDef *md = &cls->methods[cls->method_count++];
                        memcpy(md->name, m->fn->fn_decl.name, mlen);
                        md->name[mlen] = '\0';
                        md->fn_index   = fn_idx;
                        md->is_static  = m->is_static;
                    }
                }
            }
        }
        else if (s->kind == STMT_ENUM_DECL) {
            /* Register enum as a ClassDef with member name table for toString */
            if (c->module->class_count < MODULE_MAX_CLASSES) {
                int ci = c->module->class_count++;
                ClassDef *cls = &c->module->classes[ci];
                memset(cls, 0, sizeof(ClassDef));
                int nlen = s->enum_decl.length < CLASS_NAME_MAX - 1
                         ? s->enum_decl.length : CLASS_NAME_MAX - 1;
                memcpy(cls->name, s->enum_decl.name, nlen);
                cls->name[nlen] = '\0';
                cls->constructor_index = -1; cls->parent_index = -1;
                cls->is_enum = true; cls->enum_member_count = 0;
                for (struct EnumMemberNode *em = s->enum_decl.members; em; em = em->next) {
                    if (cls->enum_member_count >= 64) break;
                    char *copy = malloc(em->length + 1);
                    if (copy) {
                        memcpy(copy, em->name, em->length);
                        copy[em->length] = '\0';
                        int idx = cls->enum_member_count;
                        cls->enum_member_names[idx]  = copy;
                        cls->enum_member_values[idx] = em->value;
                        cls->enum_member_count++;
                    }
                }
            }
        }
    }


    /* ── PASS 2: Compile all bodies ─────────────────────────────────────────
     *
     * For each declaration, find its pre-allocated chunk by name and compile
     * into it. This is robust regardless of declaration order.
     * ────────────────────────────────────────────────────────────────────*/
    /* ── PASS 1.5: Emit static field initializers into __sinit__ chunk ──────────
     *
     * Static fields are initialized once at module load, not per-construction.
     * The VM runs __sinit__ before the entry point.
     * ──────────────────────────────────────────────────────────────────────────*/
    {
        int sinit_idx = module_add_chunk(c->module);
        memcpy(c->module->names[sinit_idx], "__sinit__", 9);
        c->module->names[sinit_idx][9] = '\0';
        c->module->sinit_index = sinit_idx;

        c->current_chunk     = &c->module->chunks[sinit_idx];
        c->current_fn        = sinit_idx;
        c->current_class_idx = -1;
        c->current_class_ast = NULL;
        c->local_count       = 0;
        c->scope_depth       = 0;
        c->next_slot         = 0;
        c->loop_depth        = 0;
        for (int d = 0; d < MAX_LOOP_DEPTH; d++)
            c->break_count[d] = c->continue_count[d] = 0;

        for (StmtNode *sn = program->stmts; sn; sn = sn->next) {
            Stmt *s = sn->stmt;
            if (s->kind != STMT_CLASS_DECL) continue;

            char cls_name_buf[CLASS_NAME_MAX];
            int  cls_nlen = s->class_decl.length < CLASS_NAME_MAX - 1
                          ? s->class_decl.length : CLASS_NAME_MAX - 1;
            memcpy(cls_name_buf, s->class_decl.name, cls_nlen);
            cls_name_buf[cls_nlen] = '\0';

            int ci = module_find_class(c->module, cls_name_buf);
            if (ci < 0) continue;

            c->current_class_idx = ci;
            c->current_class_ast = s;

            typedef struct ClassFieldNode CFNode_sinit;
            int fields_idx = 0;
            for (CFNode_sinit *f = s->class_decl.fields; f; f = f->next, fields_idx++) {
                if (!f->is_static || !f->initializer) continue;
                compile_expr(c, f->initializer);
                emit_op(c, OP_STORE_STATIC, s->line);
                emit_byte(c, (uint8_t)ci, s->line);
                emit_byte(c, (uint8_t)fields_idx, s->line);
            }
        }

        c->current_class_idx = -1;
        c->current_class_ast = NULL;
        c->current_chunk->local_count = c->next_slot;
        emit_op(c, OP_RETURN_VOID, 0);
    }

        for (StmtNode *n = program->stmts; n; n = n->next) {
        Stmt *s = n->stmt;

        if (s->kind == STMT_IMPORT) continue;

        if (s->kind == STMT_FN_DECL) {
            /* Find the chunk by name (handles any declaration order) */
            int fn_idx = module_find(c->module, s->fn_decl.name, s->fn_decl.length);
            if (fn_idx < 0) {
                compile_error(c, s->line, "Internal: chunk not found for '%.*s'",
                              s->fn_decl.length, s->fn_decl.name);
                continue;
            }

            c->current_chunk      = &c->module->chunks[fn_idx];
            c->current_fn         = fn_idx;
            c->current_class_idx  = -1;
            c->current_class_ast  = NULL;

            c->local_count = 0;
            c->scope_depth = 0;
            c->next_slot   = 0;
            c->loop_depth  = 0;
            for (int d = 0; d < MAX_LOOP_DEPTH; d++)
                c->break_count[d] = c->continue_count[d] = 0;

            for (ParamNode *p = s->fn_decl.params; p; p = p->next)
                declare_local(c, p->name, p->length);
            c->current_chunk->param_count = s->fn_decl.param_count;

            /* Store type signature so declare_staging can give checker real types */
            c->current_chunk->return_type_kind = (int)s->fn_decl.return_type.kind;
            {
                int pi = 0;
                for (ParamNode *p = s->fn_decl.params; p && pi < 16; p = p->next, pi++)
                    c->current_chunk->param_type_kinds[pi] = (int)p->type.kind;
            }

            for (StmtNode *b = s->fn_decl.body->block.stmts; b; b = b->next)
                compile_stmt(c, b->stmt);

            c->current_chunk->local_count = c->next_slot;

            if (s->fn_decl.return_type.kind == TYPE_VOID) {
                Chunk *ch = c->current_chunk;
                if (ch->count == 0 || ch->code[ch->count-1] != (uint8_t)OP_RETURN_VOID)
                    emit_op(c, OP_RETURN_VOID, s->line);
            }
        }
        else if (s->kind == STMT_CLASS_DECL) {
            /* Find this class's ClassDef — need null-terminated name */
            char cls_name_buf[CLASS_NAME_MAX];
            int  cls_nlen = s->class_decl.length < CLASS_NAME_MAX - 1
                          ? s->class_decl.length : CLASS_NAME_MAX - 1;
            memcpy(cls_name_buf, s->class_decl.name, cls_nlen);
            cls_name_buf[cls_nlen] = '\0';

            int ci = module_find_class(c->module, cls_name_buf);
            if (ci < 0) continue;
            ClassDef *cls = &c->module->classes[ci];

            c->current_class_idx = ci;
            c->current_class_ast = s;

            typedef struct ClassMethodNode CMNode;
            for (CMNode *m = s->class_decl.methods; m; m = m->next) {
                char mangled[128];
                snprintf(mangled, sizeof(mangled), "%s.%.*s",
                         cls->name,
                         m->fn->fn_decl.length, m->fn->fn_decl.name);
                int mfn_idx = module_find(c->module, mangled, (int)strlen(mangled));
                if (mfn_idx < 0) {
                    compile_error(c, s->line, "Internal: chunk not found for '%s'", mangled);
                    continue;
                }

                c->current_chunk = &c->module->chunks[mfn_idx];
                c->current_fn    = mfn_idx;
                c->local_count   = 0;
                c->scope_depth   = 0;
                c->next_slot     = 0;
                c->loop_depth    = 0;
                for (int d = 0; d < MAX_LOOP_DEPTH; d++)
                    c->break_count[d] = c->continue_count[d] = 0;

                /* Slot 0 is 'this' — but only for instance methods and constructors.
                 * Static methods have no 'this'. */
                if (!m->is_static) {
                    declare_local(c, "this", 4);
                }

                /* Declare parameters (after 'this' for instance, from 0 for static) */
                for (ParamNode *p = m->fn->fn_decl.params; p; p = p->next)
                    declare_local(c, p->name, p->length);

                /* param_count = explicit params only (not 'this') */
                c->current_chunk->param_count    = m->fn->fn_decl.param_count;
                c->current_chunk->is_constructor = m->is_constructor;

                /* ── Field initializers ─────────────────────────────────
                 * For constructors, emit this.field = initializer for every
                 * instance field that has an explicit initializer.
                 * Static field initializers are emitted separately into
                 * the __sinit__ chunk (see below), NOT here — they must
                 * run once at module load, not on every construction. */
                if (m->is_constructor && c->current_class_ast) {
                    typedef struct ClassFieldNode CFNode;
                    ClassDef *cur_cls = (c->current_class_idx >= 0)
                                      ? &c->module->classes[c->current_class_idx]
                                      : NULL;
                    int fields_idx = 0;   /* index into ClassDef->fields[] */
                    for (CFNode *f = c->current_class_ast->class_decl.fields;
                         f; f = f->next, fields_idx++) {
                        if (!f->initializer) continue;
                        if (f->is_static) continue;  /* handled in __sinit__ */
                        /* Instance field: use instance_slot from ClassDef */
                        int islot = (cur_cls && fields_idx < cur_cls->field_count)
                                  ? cur_cls->fields[fields_idx].instance_slot
                                  : fields_idx;
                        emit_op(c, OP_LOAD_THIS, s->line);
                        compile_expr(c, f->initializer);
                        emit_op(c, OP_SET_FIELD, s->line);
                        emit_byte(c, (uint8_t)islot, s->line);
                    }
                }

                for (StmtNode *b = m->fn->fn_decl.body->block.stmts; b; b = b->next)
                    compile_stmt(c, b->stmt);

                c->current_chunk->local_count = c->next_slot;

                /* All methods/constructors return void implicitly */
                Chunk *ch = c->current_chunk;
                if (ch->count == 0 || ch->code[ch->count-1] != (uint8_t)OP_RETURN_VOID)
                    emit_op(c, OP_RETURN_VOID, s->line);
            }

            c->current_class_idx = -1;
            c->current_class_ast = NULL;
        }
    }

    return !c->had_error;
}

void compiler_print_errors(const Compiler *c) {
    for (int i = 0; i < c->error_count; i++) {
        printf("[line %d] Compile error: %s\n",
               c->errors[i].line, c->errors[i].message);
    }
}