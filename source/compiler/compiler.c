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

#define _POSIX_C_SOURCE 200809L
#include "compiler.h"
#include "vm.h"
#include <stdio.h>
#include <string.h>

/* Forward declarations */
static int host_table_find(const CompilerHostTable *t, const char *name, int len);
static void compile_expr(Compiler *c, const Expr *expr);

/* After emitting `provided` explicit args, emit default expressions for the
 * remaining params and return the padded total arg count. */
static int emit_default_args(Compiler *c, ParamNode *params,
                             int provided, int line)
{
    (void)line;
    int idx = 0, total = provided;
    for (ParamNode *p = params; p; p = p->next, idx++)
    {
        if (idx < provided)
            continue;
        if (p->default_value)
        {
            compile_expr(c, p->default_value);
            total++;
        }
    }
    return total;
}
#include <string.h>
#include <stdlib.h>
#include <stdarg.h>

/* ─────────────────────────────────────────────────────────────────────────────
 * MODULE
 * ───────────────────────────────────────────────────────────────────────────*/

void module_init(Module *m)
{
    m->chunks = NULL;
    m->count = 0;
    m->capacity = 0;
    m->class_count = 0;
    m->sinit_index = -1;
    m->uses_stdlib = false;
    memset(m->names, 0, sizeof(m->names));
    memset(m->classes, 0, sizeof(m->classes));
    memset(&m->metadata, 0, sizeof(m->metadata));
}

static void attr_arg_free(AttrArg *a); /* forward declaration */

void module_free(Module *m)
{
    for (int i = 0; i < m->count; i++)
        chunk_free(&m->chunks[i]);
    free(m->chunks);
    m->chunks = NULL;
    m->count = 0;
    m->capacity = 0;

    /* Free heap-allocated method attributes and AttrArg strings */
    for (int ci = 0; ci < m->class_count; ci++)
    {
        ClassDef *cls = &m->classes[ci];
        for (int mi = 0; mi < cls->method_count; mi++)
        {
            MethodDef *md = &cls->methods[mi];
            if (md->attributes)
            {
                for (int ai = 0; ai < md->attribute_count; ai++)
                {
                    AttributeInstance *inst = &md->attributes[ai];
                    for (int ki = 0; ki < inst->arg_count; ki++)
                        attr_arg_free(&inst->args[ki]);
                }
                free(md->attributes);
                md->attributes = NULL;
            }
        }
        /* Free class-level AttrArg strings */
        for (int ai = 0; ai < cls->attribute_count; ai++)
        {
            AttributeInstance *inst = &cls->attributes[ai];
            for (int ki = 0; ki < inst->arg_count; ki++)
                attr_arg_free(&inst->args[ki]);
        }
    }
    m->class_count = 0;
}

/* Grow the module's chunk array if needed, returns index of new slot */
int module_add_chunk(Module *m)
{
    if (m->count >= m->capacity)
    {
        int new_cap = m->capacity < 8 ? 8 : m->capacity * 2;
        m->chunks = realloc(m->chunks, new_cap * sizeof(Chunk));
        m->capacity = new_cap;
    }
    int idx = m->count++;
    chunk_init(&m->chunks[idx]);
    return idx;
}

int module_find(const Module *m, const char *name, int len)
{
    for (int i = 0; i < m->count; i++)
    {
        if ((int)strlen(m->names[i]) == len &&
            memcmp(m->names[i], name, len) == 0)
            return i;
    }
    return -1;
}

int module_find_class(const Module *m, const char *name)
{
    for (int i = 0; i < m->class_count; i++)
    {
        if (strcmp(m->classes[i].name, name) == 0)
            return i;
    }
    return -1;
}

/* Free heap memory inside an AttrArg */
static void attr_arg_free(AttrArg *a)
{
    if (a->kind == ATTR_ARG_STRING && a->s)
    {
        free(a->s);
        a->s = NULL;
    }
    else if (a->kind == ATTR_ARG_ARRAY && a->arr.elems)
    {
        for (int i = 0; i < a->arr.count; i++)
            attr_arg_free(&a->arr.elems[i]);
        free(a->arr.elems);
        a->arr.elems = NULL;
        a->arr.count = 0;
    }
}

/* Deep-copy an AttrArg into dst (dst must be zeroed by caller).
 * Returns false on allocation failure. */
static bool attr_arg_deep_copy(AttrArg *dst, const AttrArg *src)
{
    *dst = *src;
    if (src->kind == ATTR_ARG_STRING && src->s)
    {
        dst->s = strdup(src->s);
        if (!dst->s)
            return false;
    }
    else if (src->kind == ATTR_ARG_ARRAY && src->arr.count > 0)
    {
        dst->arr.elems = calloc((size_t)src->arr.count, sizeof(AttrArg));
        if (!dst->arr.elems)
            return false;
        for (int i = 0; i < src->arr.count; i++)
        {
            if (!attr_arg_deep_copy(&dst->arr.elems[i], &src->arr.elems[i]))
            {
                for (int j = 0; j < i; j++)
                    attr_arg_free(&dst->arr.elems[j]);
                free(dst->arr.elems);
                dst->arr.elems = NULL;
                dst->arr.count = 0;
                return false;
            }
        }
    }
    return true;
}

/* Deep-copy an AttributeInstance array into a newly heap-allocated block.
 * Returns NULL on failure or when count==0. */
static AttributeInstance *attribute_instances_deep_copy(
    const AttributeInstance *src, int count)
{
    if (count == 0 || !src)
        return NULL;
    AttributeInstance *copy = calloc((size_t)count, sizeof(AttributeInstance));
    if (!copy)
        return NULL;
    for (int ai = 0; ai < count; ai++)
    {
        const AttributeInstance *s = &src[ai];
        AttributeInstance *d = &copy[ai];
        strncpy(d->class_name, s->class_name, CLASS_NAME_MAX - 1);
        d->arg_count = s->arg_count;
        for (int ki = 0; ki < s->arg_count; ki++)
        {
            if (!attr_arg_deep_copy(&d->args[ki], &s->args[ki]))
            {
                for (int bi = 0; bi <= ai; bi++)
                    for (int bk = 0; bk < copy[bi].arg_count; bk++)
                        attr_arg_free(&copy[bi].args[bk]);
                free(copy);
                return NULL;
            }
        }
    }
    return copy;
}

/* Populate module->metadata by scanning ClassDef.attributes for an @Mod instance.
 *
 * Arg→field mapping: if `staging` contains a compiled Mod class whose
 * constructor parameter names are known, we use the Mod constructor's parameter
 * list order (so developers can rename/reorder params and it stays in sync).
 * We derive param names from the constructor chunk name convention
 * "Mod.Mod" and the field names stored in the ClassDef.
 *
 * For robustness, we fall back to the fixed positional order
 * (name, version, author, description) when no staging Mod class is found.
 */
void module_extract_mod_metadata(Module *module, const Module *staging)
{
    module->metadata.has_mod = false;

    for (int ci = 0; ci < module->class_count; ci++)
    {
        ClassDef *cls = &module->classes[ci];
        for (int ai = 0; ai < cls->attribute_count; ai++)
        {
            AttributeInstance *inst = &cls->attributes[ai];
            if (strcmp(inst->class_name, "Mod") != 0)
                continue;

            module->metadata.has_mod = true;
            strncpy(module->metadata.entry_class, cls->name, MOD_STRING_MAX - 1);
            module->metadata.entry_class[MOD_STRING_MAX - 1] = '\0';

            /* Try to get Mod constructor field order from staging.
             * The Mod class's fields are in declaration order — we use that
             * as the canonical positional order for annotation args. */
            const char *field_order[8] = {
                "name", "version", "author", "description",
                NULL, NULL, NULL, NULL};
            int field_count = 4;

            if (staging)
            {
                int mod_ci = module_find_class(staging, "Mod");
                if (mod_ci >= 0)
                {
                    const ClassDef *mod_cls = &staging->classes[mod_ci];
                    /* Use the field names in declaration order, skipping
                     * fields inherited from Attribute (the base class). */
                    int fc = mod_cls->field_count < 8 ? mod_cls->field_count : 8;
                    for (int fi = 0; fi < fc; fi++)
                        field_order[fi] = mod_cls->fields[fi].name;
                    field_count = fc;
                }
            }

            /* Map annotation args to metadata fields */
            char *dest_map[8];
            memset(dest_map, 0, sizeof(dest_map));
            for (int fi = 0; fi < field_count && field_order[fi]; fi++)
            {
                const char *fn = field_order[fi];
                if (strcmp(fn, "name") == 0)
                    dest_map[fi] = module->metadata.name;
                else if (strcmp(fn, "version") == 0)
                    dest_map[fi] = module->metadata.version;
                else if (strcmp(fn, "author") == 0)
                    dest_map[fi] = module->metadata.author;
                else if (strcmp(fn, "description") == 0)
                    dest_map[fi] = module->metadata.description;
            }

            for (int k = 0; k < inst->arg_count && k < field_count; k++)
            {
                if (!dest_map[k])
                    continue;
                if (inst->args[k].kind == ATTR_ARG_STRING && inst->args[k].s)
                {
                    strncpy(dest_map[k], inst->args[k].s, MOD_STRING_MAX - 1);
                    dest_map[k][MOD_STRING_MAX - 1] = '\0';
                }
            }
            return; /* first @Mod wins */
        }
    }
}

/* Deep-copy a Chunk from src into dst. */
static bool chunk_deep_copy(Chunk *dst, const Chunk *src)
{
    chunk_init(dst);
    dst->param_count = src->param_count;
    dst->local_count = src->local_count;
    dst->is_constructor = src->is_constructor;
    dst->return_type_kind = src->return_type_kind;
    for (int i = 0; i < src->param_count && i < 16; i++)
        dst->param_type_kinds[i] = src->param_type_kinds[i];

    if (src->count > 0)
    {
        dst->code = malloc(src->count);
        dst->lines = malloc(src->count * sizeof(int));
        if (!dst->code || !dst->lines)
        {
            chunk_free(dst);
            return false;
        }
        memcpy(dst->code, src->code, src->count);
        memcpy(dst->lines, src->lines, src->count * sizeof(int));
        dst->count = src->count;
        dst->capacity = src->count;
    }

    if (src->constants.count > 0)
    {
        dst->constants.values = malloc(src->constants.count * sizeof(Value));
        if (!dst->constants.values)
        {
            chunk_free(dst);
            return false;
        }
        memcpy(dst->constants.values, src->constants.values,
               src->constants.count * sizeof(Value));
        dst->constants.count = src->constants.count;
        dst->constants.capacity = src->constants.count;
    }
    return true;
}

/* Rewrite fn_idx / class_idx operands in a chunk's bytecode.
 *
 * When bytecode from `src` is copied into `dst`, opcodes that encode chunk
 * indices (OP_CALL, OP_CALL_METHOD, OP_CALL_SUPER) and class indices
 * (OP_NEW) need to be translated from src-relative values to dst-relative
 * values.  We do this by name lookup: for each src index we look up the
 * chunk/class name in dst and patch the two-byte operand in-place.
 *
 * fn_remap[i]    = dst chunk index for src chunk i  (-1 = no mapping)
 * class_remap[i] = dst class index for src class i  (-1 = no mapping)
 */
static void chunk_remap_indices(Chunk *chunk,
                                const int *fn_remap, int fn_remap_len,
                                const int *class_remap, int class_remap_len)
{
    if (!chunk->code || chunk->count == 0)
        return;

    uint8_t *code = chunk->code;
    int n = chunk->count;

    for (int pc = 0; pc < n;)
    {
        uint8_t op = code[pc++];

        switch ((OpCode)op)
        {

        /* ── OP_CALL: [uint16 fn_idx][uint8 argc] — absolute chunk index ── */
        case OP_CALL:
        {
            if (pc + 2 >= n)
            {
                pc += 3;
                break;
            }
            int old_fn = (code[pc] << 8) | code[pc + 1];
            if (old_fn >= 0 && old_fn < fn_remap_len && fn_remap[old_fn] >= 0)
            {
                int new_fn = fn_remap[old_fn];
                code[pc] = (uint8_t)(new_fn >> 8);
                code[pc + 1] = (uint8_t)(new_fn & 0xFF);
            }
            pc += 3;
            break;
        }

        /* ── OP_CALL_METHOD: [uint16 method_slot][uint8 argc]
         *   slot is an index into obj->class_def->methods[], NOT a chunk
         *   index — no remapping needed. ────────────────────────────── */
        case OP_CALL_METHOD:
            pc += 3;
            break;

        /* ── OP_CALL_SUPER: [uint16 parent_class_idx][uint8 argc]
         *   parent_class_idx indexes module->classes[], not chunks.
         *   Remap via class_remap. ─────────────────────────────────── */
        case OP_CALL_SUPER:
        {
            if (pc + 1 < n)
            {
                int old_ci = (code[pc] << 8) | code[pc + 1];
                if (old_ci >= 0 && old_ci < class_remap_len && class_remap[old_ci] >= 0)
                {
                    int new_ci = class_remap[old_ci];
                    code[pc] = (uint8_t)(new_ci >> 8);
                    code[pc + 1] = (uint8_t)(new_ci & 0xFF);
                }
            }
            pc += 3;
            break;
        }

        /* ── OP_NEW: uint16 class_idx + uint8 argc + type-arg bytes ─ */
        case OP_NEW:
        {
            if (pc + 1 >= n)
            {
                pc += 3;
                break;
            }
            int old_ci = (code[pc] << 8) | code[pc + 1];
            if (old_ci >= 0 && old_ci < class_remap_len && class_remap[old_ci] >= 0)
            {
                int new_ci = class_remap[old_ci];
                code[pc] = (uint8_t)(new_ci >> 8);
                code[pc + 1] = (uint8_t)(new_ci & 0xFF);
            }
            pc += 3; /* skip class_idx(2) + argc(1) */
            /* consume tac + tac type-arg bytes */
            if (pc < n)
            {
                uint8_t tac = code[pc++];
                if (tac > 16)
                    tac = 16;
                pc += tac;
            }
            break;
        }

        /* ── OP_CALL_HOST: uint16 host_fn_idx (no remapping) ───────── */
        case OP_CALL_HOST:
            pc += 3;
            break;

        /* ── OP_CALL_IFACE: uint16 name_const_idx + uint8 argc ──────── */
        case OP_CALL_IFACE:
            pc += 3;
            break;

        /* ── 2-byte operand opcodes ───────────────────────────────── */
        case OP_LOAD_CONST_INT:
        case OP_LOAD_CONST_FLOAT:
        case OP_LOAD_CONST_STR:
        case OP_JUMP:
        case OP_JUMP_IF_FALSE:
        case OP_JUMP_IF_TRUE:
        case OP_NULL_ASSERT:   /* [uint16 line]  */
        case OP_NULL_COALESCE: /* [uint16 offset] */
            pc += 2;
            break;

        /* ── LOAD_STATIC / STORE_STATIC: [uint8 class_idx][uint8 field_idx] */
        case OP_LOAD_STATIC:
        case OP_STORE_STATIC:
        {
            if (pc < n)
            {
                uint8_t old_ci = code[pc];
                if (old_ci < class_remap_len && class_remap[old_ci] >= 0)
                    code[pc] = (uint8_t)class_remap[old_ci];
            }
            pc += 2;
            break;
        }

        /* ── 1-byte operand opcodes ───────────────────────────────── */
        case OP_LOAD_CONST_BOOL:
        case OP_LOAD_LOCAL:
        case OP_STORE_LOCAL:
        case OP_GET_FIELD:
        case OP_SET_FIELD:
        case OP_NEW_ARRAY:
        case OP_ARRAY_LIT:
        case OP_IS_TYPE:
        case OP_AS_TYPE:
        case OP_TYPE_FIELD:
            pc += 1;
            break;

        /* ── OP_TO_STR: 1 byte kind; enum adds an extra byte ─────── */
        case OP_TO_STR:
        {
            uint8_t kind = (pc < n) ? code[pc] : 0;
            pc += (kind == 4) ? 2 : 1;
            break;
        }

        /* ── OP_TYPEOF: variable length ──────────────────────────── */
        case OP_TYPEOF:
        {
            if (pc + 1 < n)
            {
                uint8_t nlen = code[pc + 1];
                pc += 2 + nlen;
            }
            else
            {
                pc += 1;
            }
            break;
        }

        /* ── No operands ─────────────────────────────────────────── */
        case OP_ADD_INT:
        case OP_SUB_INT:
        case OP_MUL_INT:
        case OP_DIV_INT:
        case OP_MOD_INT:
        case OP_NEGATE_INT:
        case OP_ADD_FLOAT:
        case OP_SUB_FLOAT:
        case OP_MUL_FLOAT:
        case OP_DIV_FLOAT:
        case OP_MOD_FLOAT:
        case OP_NEGATE_FLOAT:
        case OP_CONCAT_STR:
        case OP_NOT_BOOL:
        case OP_AND_BOOL:
        case OP_OR_BOOL:
        case OP_CMP_EQ_INT:
        case OP_CMP_NEQ_INT:
        case OP_CMP_LT_INT:
        case OP_CMP_LTE_INT:
        case OP_CMP_GT_INT:
        case OP_CMP_GTE_INT:
        case OP_CMP_EQ_FLOAT:
        case OP_CMP_NEQ_FLOAT:
        case OP_CMP_LT_FLOAT:
        case OP_CMP_LTE_FLOAT:
        case OP_CMP_GT_FLOAT:
        case OP_CMP_GTE_FLOAT:
        case OP_CMP_EQ_BOOL:
        case OP_CMP_NEQ_BOOL:
        case OP_CMP_EQ_STR:
        case OP_CMP_NEQ_STR:
        case OP_CMP_EQ_VAL:
        case OP_CMP_NEQ_VAL:
        case OP_POP:
        case OP_RETURN:
        case OP_RETURN_VOID:
        case OP_MATCH_FAIL:
        case OP_TRUNC_I8:
        case OP_TRUNC_U8:
        case OP_TRUNC_I16:
        case OP_TRUNC_U16:
        case OP_TRUNC_I32:
        case OP_TRUNC_U32:
        case OP_TRUNC_U64:
        case OP_TRUNC_CHAR:
        case OP_ARRAY_GET:
        case OP_ARRAY_SET:
        case OP_ARRAY_LEN:
        case OP_LOAD_THIS:
            break; /* pc already advanced past opcode byte */

        default:
            pc++; /* safety: unknown/future opcode — skip one byte */
            break;
        }
    }
}

bool module_merge(Module *dst, const Module *src)
{
    /* ── Add missing chunks from src into dst ────────────────────────── */
    /* First pass: place all src chunks not already in dst.
     * User-owned chunks already in dst are left untouched.
     * __sinit__ is always skipped — each module has its own. */
    for (int i = 0; i < src->count; i++)
    {
        const char *name = src->names[i];
        if (strcmp(name, "__sinit__") == 0)
            continue;
        if (module_find(dst, name, (int)strlen(name)) >= 0)
            continue;
        int idx = module_add_chunk(dst);
        if (!chunk_deep_copy(&dst->chunks[idx], &src->chunks[i]))
            return false;
        strncpy(dst->names[idx], name, 63);
    }

    /* ── Add missing class definitions from src into dst ────────────────
     * Must happen BEFORE building class_remap so that class_remap[i] can
     * find the correct dst index for every src class.  Method fn_indices
     * are updated later once fn_remap is known.                          */
    for (int i = 0; i < src->class_count; i++)
    {
        const ClassDef *sc = &src->classes[i];
        int existing = module_find_class(dst, sc->name);
        if (existing >= 0)
            continue; /* already present — skip for now */
        if (dst->class_count >= MODULE_MAX_CLASSES)
            continue;
        ClassDef *dc = &dst->classes[dst->class_count++];
        *dc = *sc;
        if (sc->is_enum)
        {
            for (int j = 0; j < sc->enum_member_count; j++)
                dc->enum_member_names[j] = sc->enum_member_names[j]
                                               ? strdup(sc->enum_member_names[j])
                                               : NULL;
        }
        /* Deep-copy heap-allocated method attribute arrays */
        for (int j = 0; j < dc->method_count; j++)
        {
            MethodDef *dm = &dc->methods[j];
            dm->attributes = attribute_instances_deep_copy(
                dm->attributes, dm->attribute_count);
            if (dm->attribute_count > 0 && !dm->attributes)
                dm->attribute_count = 0;
        }
        /* Deep-copy class-level attribute arg strings */
        for (int j = 0; j < dc->attribute_count; j++)
        {
            AttributeInstance *inst = &dc->attributes[j];
            for (int ki = 0; ki < inst->arg_count; ki++)
            {
                AttrArg tmp = {0};
                attr_arg_deep_copy(&tmp, &inst->args[ki]);
                inst->args[ki] = tmp;
            }
        }
        /* fn_indices will be fixed in the remap pass below */
    }

    /* ── Build fn_remap: src chunk index → dst chunk index ──────────── */
    int *fn_remap = malloc(src->count * sizeof(int));
    if (!fn_remap)
        return false;
    for (int i = 0; i < src->count; i++)
    {
        fn_remap[i] = module_find(dst, src->names[i], (int)strlen(src->names[i]));
    }

    /* ── Build class_remap ───────────────────────────────────────────── */
    /* Classes have already been added to dst above, so this lookup succeeds. */
    int *class_remap = malloc((src->class_count + 1) * sizeof(int));
    if (!class_remap)
    {
        free(fn_remap);
        return false;
    }
    for (int i = 0; i < src->class_count; i++)
    {
        class_remap[i] = module_find_class(dst, src->classes[i].name);
    }

    /* ── Remap OP_CALL operands in freshly-added src chunks ─────────── */
    for (int i = 0; i < src->count; i++)
    {
        const char *name = src->names[i];
        if (strcmp(name, "__sinit__") == 0)
            continue;
        if (src->chunks[i].count == 0)
            continue;
        int dst_idx = fn_remap[i];
        if (dst_idx < 0)
            continue;
        chunk_remap_indices(&dst->chunks[dst_idx],
                            fn_remap, src->count,
                            class_remap, src->class_count);
    }

    free(fn_remap);

    /* ── Finalise class definitions: remap method fn_indices ──────────── */
    for (int i = 0; i < src->class_count; i++)
    {
        const ClassDef *sc = &src->classes[i];
        int di = module_find_class(dst, sc->name);
        if (di < 0)
            continue;
        ClassDef *dc = &dst->classes[di];

        /* Remap each method's fn_index and the constructor_index */
        for (int j = 0; j < sc->method_count && j < dc->method_count; j++)
        {
            int old_fn = sc->methods[j].fn_index;
            if (old_fn < 0 || old_fn >= src->count)
                continue;
            const char *chunk_name = src->names[old_fn];
            int new_idx = module_find(dst, chunk_name, (int)strlen(chunk_name));
            if (new_idx >= 0)
                dc->methods[j].fn_index = new_idx;
        }
        if (sc->constructor_index >= 0 && sc->constructor_index < src->count)
        {
            const char *ctor_name = src->names[sc->constructor_index];
            int new_ctor = module_find(dst, ctor_name, (int)strlen(ctor_name));
            if (new_ctor >= 0)
                dc->constructor_index = new_ctor;
        }
    }

    free(class_remap);
    return true;
}

/*
 * module_strip — remove a specific set of chunks and classes from `module`,
 * compacting the arrays and rewriting all fn_idx / class_idx operands in the
 * surviving bytecode.
 *
 * strip_names[0..strip_fn_count)  — chunk names to remove
 * strip_classes[0..strip_cl_count) — class names to remove
 *
 * "__sinit__" is always preserved regardless of the strip list.
 */
void module_strip(Module *module,
                  const char **strip_names, int strip_fn_count,
                  const char **strip_classes, int strip_cl_count)
{
    if (module->count == 0 && module->class_count == 0)
        return;

    /* ── Step 1: mark ──────────────────────────────────────────────────── */
    bool *strip_chunk = calloc(module->count, sizeof(bool));
    bool *strip_cls = calloc(module->class_count + 1, sizeof(bool));
    if (!strip_chunk || !strip_cls)
    {
        free(strip_chunk);
        free(strip_cls);
        return;
    }

    for (int si = 0; si < strip_fn_count; si++)
    {
        if (!strip_names[si] || strcmp(strip_names[si], "__sinit__") == 0)
            continue;
        int idx = module_find(module, strip_names[si], (int)strlen(strip_names[si]));
        if (idx >= 0)
            strip_chunk[idx] = true;
    }
    for (int si = 0; si < strip_cl_count; si++)
    {
        if (!strip_classes[si])
            continue;
        int idx = module_find_class(module, strip_classes[si]);
        if (idx >= 0)
            strip_cls[idx] = true;
    }

    /* ── Step 1b: protect chunks inherited by surviving user classes ────── *
     * If a user class (not stripped) has inherited methods from a stdlib
     * class (which would be stripped), keep those method chunks alive.
     * This handles e.g. class NameList : List<string> inheriting List.add. */
    for (int ci = 0; ci < module->class_count; ci++)
    {
        if (strip_cls[ci])
            continue; /* skip stripped classes */
        ClassDef *dc = &module->classes[ci];
        for (int mi = 0; mi < dc->method_count; mi++)
        {
            int fi = dc->methods[mi].fn_index;
            if (fi >= 0 && fi < module->count && strip_chunk[fi])
                strip_chunk[fi] = false; /* keep it — user class depends on it */
        }
        if (dc->constructor_index >= 0 && dc->constructor_index < module->count && strip_chunk[dc->constructor_index])
            strip_chunk[dc->constructor_index] = false;
    }

    /* ── Step 2: build remap tables ────────────────────────────────────── */
    int *fn_remap = malloc(module->count * sizeof(int));
    int *class_remap = malloc((module->class_count + 1) * sizeof(int));
    if (!fn_remap || !class_remap)
    {
        free(strip_chunk);
        free(strip_cls);
        free(fn_remap);
        free(class_remap);
        return;
    }
    for (int i = 0; i < module->count; i++)
        fn_remap[i] = -1;
    int new_fn = 0;
    for (int i = 0; i < module->count; i++)
        fn_remap[i] = strip_chunk[i] ? -1 : new_fn++;
    /* Class indices in OP_NEW/OP_CALL_SUPER/LOAD_STATIC/STORE_STATIC are kept
     * as their compile-time values. At runtime, stdlib is always loaded in the
     * same deterministic order so class indices are stable across compile and
     * load. The identity remap here preserves those baked-in indices. */
    for (int i = 0; i < module->class_count; i++)
        class_remap[i] = i;

    /* ── Step 3: rewrite fn_idx and class_idx operands in surviving chunks */
    for (int i = 0; i < module->count; i++)
    {
        if (strip_chunk[i])
            continue;
        chunk_remap_indices(&module->chunks[i],
                            fn_remap, module->count,
                            class_remap, module->class_count);
    }

    /* ── Step 4: remap method/ctor fn_indices in surviving class defs ───── */
    for (int i = 0; i < module->class_count; i++)
    {
        if (strip_cls[i])
            continue;
        ClassDef *dc = &module->classes[i];
        for (int j = 0; j < dc->method_count; j++)
        {
            int old = dc->methods[j].fn_index;
            if (old >= 0 && old < module->count && fn_remap[old] >= 0)
                dc->methods[j].fn_index = fn_remap[old];
        }
        if (dc->constructor_index >= 0 && dc->constructor_index < module->count)
        {
            int r = fn_remap[dc->constructor_index];
            dc->constructor_index = (r >= 0) ? r : -1;
        }
    }

    /* ── Step 5: compact chunks ─────────────────────────────────────────── */
    if (module->sinit_index >= 0 && module->sinit_index < module->count)
        module->sinit_index = fn_remap[module->sinit_index];

    int compact_fn = 0;
    for (int i = 0; i < module->count; i++)
    {
        if (strip_chunk[i])
        {
            chunk_free(&module->chunks[i]);
            continue;
        }
        if (compact_fn != i)
        {
            module->chunks[compact_fn] = module->chunks[i];
            chunk_init(&module->chunks[i]);
            memcpy(module->names[compact_fn], module->names[i], 64);
        }
        compact_fn++;
    }
    module->count = compact_fn;

    /* ── Step 6: compact class definitions ──────────────────────────────── */
    int compact_cl = 0;
    for (int i = 0; i < module->class_count; i++)
    {
        if (strip_cls[i])
        {
            ClassDef *dc = &module->classes[i];
            if (dc->is_enum)
                for (int j = 0; j < dc->enum_member_count; j++)
                    free(dc->enum_member_names[j]);
            continue;
        }
        if (compact_cl != i)
            module->classes[compact_cl] = module->classes[i];
        compact_cl++;
    }
    module->class_count = compact_cl;

    free(fn_remap);
    free(class_remap);
    free(strip_chunk);
    free(strip_cls);
}

void module_disassemble(const Module *m)
{
    printf("\n+==========================================+\n");
    printf("|           MODULE DISASSEMBLY             |\n");
    printf("+==========================================+\n");
    if (m->metadata.has_mod)
    {
        printf("\n@Mod metadata:\n");
        printf("  name:        %s\n", m->metadata.name);
        if (m->metadata.version[0])
            printf("  version:     %s\n", m->metadata.version);
        if (m->metadata.author[0])
            printf("  author:      %s\n", m->metadata.author);
        if (m->metadata.description[0])
            printf("  description: %s\n", m->metadata.description);
        printf("  entry class: %s\n", m->metadata.entry_class);
    }
    for (int i = 0; i < m->count; i++)
        chunk_disassemble(&m->chunks[i], m->names[i]);
}

/* ─────────────────────────────────────────────────────────────────────────────
 * ERROR REPORTING
 * ───────────────────────────────────────────────────────────────────────────*/

static void compile_error(Compiler *c, int line, const char *fmt, ...)
{
    if (c->error_count >= COMPILER_MAX_ERRORS)
        return;
    CompileError *e = &c->errors[c->error_count++];
    e->line = line;
    c->had_error = true;
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

static void emit_byte(Compiler *c, uint8_t byte, int line)
{
    chunk_write(c->current_chunk, byte, line);
}

static void emit_op(Compiler *c, OpCode op, int line)
{
    emit_byte(c, (uint8_t)op, line);
}

/* Emit a uint16_t operand. Returns the offset of the first byte. */
static int emit_u16(Compiler *c, uint16_t value, int line)
{
    return chunk_write_u16(c->current_chunk, value, line);
}

/* Emit a jump instruction with a placeholder operand.
 * Returns the offset of the placeholder so it can be patched later. */
static int emit_jump(Compiler *c, OpCode jump_op, int line)
{
    emit_op(c, jump_op, line);
    return emit_u16(c, 0, line); /* placeholder */
}

/* Patch a previously emitted jump to point to the current position. */
static void patch_jump(Compiler *c, int placeholder_offset)
{
    /* Current position is where execution continues after the jump.
     * The jump offset is relative to the byte AFTER the 2-byte operand. */
    int after_operand = placeholder_offset + 2;
    int jump_dist = c->current_chunk->count - after_operand;

    if (jump_dist > INT16_MAX)
    {
        compile_error(c, 0, "Jump distance too large (%d)", jump_dist);
        return;
    }
    chunk_patch_u16(c->current_chunk, placeholder_offset, (uint16_t)(int16_t)jump_dist);
}

/* Emit a backward jump to a known position (for while/for loops). */
static void emit_loop(Compiler *c, int loop_start, int line)
{
    emit_op(c, OP_JUMP, line);
    /* Distance from the byte after this instruction back to loop_start.
     * Negative because we're jumping backward. */
    int after_operand = c->current_chunk->count + 2;
    int dist = loop_start - after_operand;
    emit_u16(c, (uint16_t)(int16_t)dist, line);
}

/* Add a constant to the current chunk's pool and emit a load instruction. */
static void emit_const_int(Compiler *c, __int128_t value, int line)
{
    int idx = chunk_add_constant(c->current_chunk, val_int(value));
    emit_op(c, OP_LOAD_CONST_INT, line);
    emit_u16(c, (uint16_t)idx, line);
}

static void emit_const_float(Compiler *c, double value, int line)
{
    int idx = chunk_add_constant(c->current_chunk, val_float(value));
    emit_op(c, OP_LOAD_CONST_FLOAT, line);
    emit_u16(c, (uint16_t)idx, line);
}

static void emit_const_str(Compiler *c, const char *chars, int len, int line)
{
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
static int add_const_str(Compiler *c, const char *s)
{
    int len = s ? (int)strlen(s) : 0;
    char *copy = malloc(len + 1);
    if (!copy)
        return -1;
    memcpy(copy, s ? s : "", len);
    copy[len] = '\0';
    return chunk_add_constant(c->current_chunk, val_str(copy));
}

/* Return the C string name for a TypeKind (used for typeof array names). */
static const char *type_kind_cname(TypeKind k)
{
    switch (k)
    {
    case TYPE_BOOL:
        return "bool";
    case TYPE_INT:
        return "int";
    case TYPE_FLOAT:
        return "float";
    case TYPE_STRING:
        return "string";
    case TYPE_SBYTE:
        return "sbyte";
    case TYPE_BYTE:
        return "byte";
    case TYPE_SHORT:
        return "short";
    case TYPE_USHORT:
        return "ushort";
    case TYPE_UINT:
        return "uint";
    case TYPE_LONG:
        return "long";
    case TYPE_ULONG:
        return "ulong";
    case TYPE_DOUBLE:
        return "double";
    case TYPE_CHAR:
        return "char";
    case TYPE_ENUM:
        return "enum";
    case TYPE_VOID:
        return "void";
    default:
        return "unknown";
    }
}

/* ─────────────────────────────────────────────────────────────────────────────
 * LOCAL VARIABLE MANAGEMENT
 * ───────────────────────────────────────────────────────────────────────────*/

/* Open a new lexical scope (entering a block). */
static void scope_push(Compiler *c)
{
    c->scope_depth++;
}

/* Close a scope, discarding all locals declared in it.
 * Emits OP_POP for each discarded local to clean the stack. */
static void scope_pop(Compiler *c, int line)
{
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
static int declare_local(Compiler *c, const char *name, int length)
{
    if (c->local_count >= MAX_LOCALS)
    {
        compile_error(c, 0, "Too many local variables in function");
        return 0;
    }
    LocalVar *local = &c->locals[c->local_count++];
    local->name = name;
    local->length = length;
    local->slot = c->next_slot++;
    local->depth = c->scope_depth;
    return local->slot;
}

/* Look up a local variable by name. Returns its slot index, or -1. */
static int resolve_local(Compiler *c, const char *name, int length)
{
    /* Search backward so inner scopes shadow outer ones */
    for (int i = c->local_count - 1; i >= 0; i--)
    {
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
static const char *strip_generic(const char *class_name, char *buf, size_t buf_size)
{
    if (!class_name)
    {
        buf[0] = '\0';
        return buf;
    }
    const char *lt = strchr(class_name, '<');
    if (!lt)
    {
        strncpy(buf, class_name, buf_size - 1);
        buf[buf_size - 1] = '\0';
        return buf;
    }
    size_t len = (size_t)(lt - class_name);
    if (len >= buf_size)
        len = buf_size - 1;
    memcpy(buf, class_name, len);
    buf[len] = '\0';
    return buf;
}

/* Look up a field index from a class by name, using the module's ClassDef.
 * Returns the fields[] array index (used by LOAD_STATIC/STORE_STATIC). */
static int find_field_index_by_class(Compiler *c, const char *class_name,
                                     const char *field_name, int field_len)
{
    int ci = module_find_class(c->module, class_name);
    if (ci < 0)
        return -1;
    ClassDef *cls = &c->module->classes[ci];
    for (int i = 0; i < cls->field_count; i++)
    {
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
                                       const char *field_name, int field_len)
{
    int ci = module_find_class(c->module, class_name);
    if (ci < 0)
        return -1;
    ClassDef *cls = &c->module->classes[ci];
    for (int i = 0; i < cls->field_count; i++)
    {
        if ((int)strlen(cls->fields[i].name) == field_len &&
            memcmp(cls->fields[i].name, field_name, field_len) == 0)
            return cls->fields[i].instance_slot;
    }
    return -1;
}

/* compiler_ensure_class — find class in c->module; if not there, copy it
 * from c->staging (stdlib ClassDef) so the compiler can reference it.
 * Returns the class index in c->module, or -1 if not found anywhere.
 * This allows stdlib classes like Exception to be instantiated with 'new'
 * without requiring their source to be inlined into the merged source. */
static int compiler_ensure_class(Compiler *c, const char *class_name)
{
    int ci = module_find_class(c->module, class_name);
    if (ci >= 0)
        return ci;

    /* Not in module — try staging */
    if (!c->staging)
        return -1;
    int sci = module_find_class(c->staging, class_name);
    if (sci < 0)
        return -1;

    /* Copy the ClassDef from staging into the module, and also
     * copy its constructor and method chunks so the VM can call them. */
    if (c->module->class_count >= MODULE_MAX_CLASSES)
        return -1;
    ci = c->module->class_count++;
    ClassDef *dst = &c->module->classes[ci];
    const ClassDef *src = &c->staging->classes[sci];
    *dst = *src; /* shallow copy — name/fields/methods arrays are value-embedded */

    /* Copy constructor chunk if present */
    if (src->constructor_index >= 0 && src->constructor_index < c->staging->count)
    {
        const char *ctor_name = c->staging->names[src->constructor_index];
        int existing = module_find(c->module, ctor_name, (int)strlen(ctor_name));
        if (existing >= 0)
        {
            dst->constructor_index = existing;
        }
        else
        {
            int new_idx = module_add_chunk(c->module);
            if (new_idx >= 0)
            {
                if (chunk_deep_copy(&c->module->chunks[new_idx],
                                    &c->staging->chunks[src->constructor_index]))
                {
                    strncpy(c->module->names[new_idx], ctor_name, 63);
                    dst->constructor_index = new_idx;
                }
                else
                {
                    dst->constructor_index = -1;
                }
            }
            else
            {
                dst->constructor_index = -1;
            }
        }
    }

    /* Copy method chunks */
    for (int mi = 0; mi < dst->method_count; mi++)
    {
        int old_fn = src->methods[mi].fn_index;
        if (old_fn < 0 || old_fn >= c->staging->count)
        {
            dst->methods[mi].fn_index = -1;
            continue;
        }
        const char *mname = c->staging->names[old_fn];
        int existing = module_find(c->module, mname, (int)strlen(mname));
        if (existing >= 0)
        {
            dst->methods[mi].fn_index = existing;
        }
        else
        {
            int new_idx = module_add_chunk(c->module);
            if (new_idx >= 0 &&
                chunk_deep_copy(&c->module->chunks[new_idx],
                                &c->staging->chunks[old_fn]))
            {
                strncpy(c->module->names[new_idx], mname, 63);
                dst->methods[mi].fn_index = new_idx;
            }
            else
            {
                dst->methods[mi].fn_index = -1;
            }
        }
    }

    return ci;
}

/* Returns the method SLOT index (position in cls->methods[]) for the named
 * method on the given class.  The VM resolves the actual chunk at runtime via
 * obj->class_def->methods[slot].fn_index, which is always up-to-date after
 * module_merge remaps the class def.  This decouples bytecode from absolute
 * chunk indices, allowing stdlib chunks to be fully stripped from .xar output.
 */
static int find_method_slot(Compiler *c, const char *class_name,
                            const char *method_name, int method_len)
{
    int ci = module_find_class(c->module, class_name);
    if (ci < 0)
        return -1;
    ClassDef *cls = &c->module->classes[ci];
    for (int i = 0; i < cls->method_count; i++)
    {
        if ((int)strlen(cls->methods[i].name) == method_len &&
            memcmp(cls->methods[i].name, method_name, method_len) == 0)
            return i; /* slot index, NOT fn_index */
    }
    return -1;
}

/* Returns the absolute chunk fn_index for a named method.  Used only for
 * OP_CALL (static method calls with no receiver object) where the class_def
 * is not available at runtime to resolve slots.
 */
static int find_method_fn_index(Compiler *c, const char *class_name,
                                const char *method_name, int method_len)
{
    int ci = module_find_class(c->module, class_name);
    if (ci < 0)
        return -1;
    ClassDef *cls = &c->module->classes[ci];
    for (int i = 0; i < cls->method_count; i++)
    {
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

static void compile_expr(Compiler *c, const Expr *expr)
{
    int line = expr->line;

    switch (expr->kind)
    {

    /* ── Literals ──────────────────────────────────────────────────── */
    case EXPR_INT_LIT:
        emit_const_int(c, expr->int_lit.value, line);
        break;

    case EXPR_CHAR_LIT:
        emit_const_int(c, (int64_t)expr->char_lit.value, line);
        break;

    case EXPR_NEW_ARRAY:
    {
        /* new ElementType[length] — push length, emit OP_NEW_ARRAY + elem_kind tag.
         * For generic element types (TYPE_PARAM), emit 0x80|param_index so the VM
         * can look up the concrete type from the constructor frame's type_args[]. */
        compile_expr(c, expr->new_array.length);
        emit_op(c, OP_NEW_ARRAY, line);
        TypeKind ek = expr->new_array.element_type.kind;
        const char *elem_name = (ek == TYPE_OBJECT || ek == TYPE_PARAM)
                                    ? expr->new_array.element_type.class_name
                                    : expr->new_array.element_type.param_name;
        /* Check if element type is a generic type parameter of the current class.
         * The parser produces TYPE_OBJECT("T") for unknown type names, so we
         * match against the class's type param list by name. */
        int param_idx = -1;
        if (c->current_class_ast && elem_name)
        {
            TypeParamNode *tp = c->current_class_ast->class_decl.type_params;
            for (int _pi = 0; tp; tp = tp->next, _pi++)
            {
                if (strncmp(tp->name, elem_name, tp->length) == 0 && elem_name[tp->length] == '\0')
                {
                    param_idx = _pi;
                    break;
                }
            }
        }
        if (param_idx >= 0)
        {
            emit_byte(c, (uint8_t)(0x80 | param_idx), line); /* sentinel: resolve from frame */
        }
        else
        {
            if (ek == TYPE_UNKNOWN || ek == TYPE_PARAM)
                ek = TYPE_ANY;
            emit_byte(c, (uint8_t)ek, line);
        }
        break;
    }

    case EXPR_IS:
    {
        /* expr is TypeName — statically typed, so evaluate at compile time.
         * Emit the operand (for side effects), pop it, push constant bool. */
        compile_expr(c, expr->type_op.operand);
        emit_op(c, OP_POP, line); /* discard value, keep side effects */
        TypeKind src = expr->type_op.operand->resolved_type.kind;
        TypeKind tgt = expr->type_op.check_type.kind;
        /* Types match if kinds are equal, or both are int-family */
        bool int_kinds[] = {0, 0, 0, 1, 0, 0, 0, 0, 0, 1, 1, 1, 1, 1, 1, 1, 0, 0, 0, 0}; /* TYPE_* indices */
        bool result;
        if (src == tgt)
        {
            result = true;
        }
        else if (src < 20 && tgt < 20 && int_kinds[src] && int_kinds[tgt])
        {
            /* int-family cross-check: int is ushort etc — true at runtime */
            result = true;
        }
        else if (src == TYPE_ARRAY && tgt == TYPE_ARRAY)
        {
            /* array is array[] — check element type */
            Type *se = expr->type_op.operand->resolved_type.element_type;
            Type *te = expr->type_op.check_type.element_type;
            result = (se && te && se->kind == te->kind);
        }
        else
        {
            result = false;
        }
        emit_op(c, OP_LOAD_CONST_BOOL, line);
        emit_byte(c, result ? 1 : 0, line);
        break;
    }

    case EXPR_AS:
    {
        /* expr as TypeName -> val : emit operand then OP_AS_TYPE [tag] */
        compile_expr(c, expr->type_op.operand);

        /* Special case: casting to string is a VALUE CONVERSION, not a
         * type check. Use OP_TO_STR with the source type kind byte so the
         * VM actually converts int/float/bool to their string representation. */
        if (expr->type_op.check_type.kind == TYPE_STRING)
        {
            TypeKind src = expr->type_op.operand->resolved_type.kind;
            uint8_t to_str_kind;
            if (src == TYPE_INT || src == TYPE_LONG || src == TYPE_SBYTE || src == TYPE_SHORT)
                to_str_kind = 0; /* signed int kind in OP_TO_STR */
            else if (src == TYPE_UINT || src == TYPE_ULONG || src == TYPE_BYTE || src == TYPE_USHORT)
                to_str_kind = 5; /* unsigned int kind in OP_TO_STR */
            else if (src == TYPE_FLOAT || src == TYPE_DOUBLE)
                to_str_kind = 1; /* float kind */
            else if (src == TYPE_BOOL)
                to_str_kind = 2; /* bool kind */
            else if (src == TYPE_CHAR)
                to_str_kind = 3; /* char kind */
            else
                to_str_kind = 255; /* string/already string → default no-op */
            emit_op(c, OP_TO_STR, line);
            emit_byte(c, to_str_kind, line);
        }
        else
        {
            emit_op(c, OP_AS_TYPE, line);
            emit_byte(c, (uint8_t)expr->type_op.check_type.kind, line);
        }
        break;
    }

    case EXPR_TYPEOF:
    {
        /* typeof(expr) -> Type object
         * Emit the operand, then OP_TYPEOF [tag] [name_hi] [name_lo].
         * For named types (objects, enums) we store the name as a string
         * constant and pass its index; otherwise 0xFFFF means use built-in. */
        /* For class-reference operands (typeof(ClassName)), the operand
         * is just a name — no runtime value to load. Push a dummy null
         * (OP_TYPEOF pops it) so the stack math stays consistent. */
        TypeKind tk = expr->type_of.operand->resolved_type.kind;
        if (tk == TYPE_CLASS_REF)
        {
            emit_op(c, OP_PUSH_NULL, line); /* placeholder — popped by OP_TYPEOF */
        }
        else
        {
            compile_expr(c, expr->type_of.operand);
        }
        uint8_t tag = (uint8_t)tk;
        uint16_t name_idx = 0xFFFF;
        /* For object/class/enum types, embed the name as a string const */
        if (tk == TYPE_OBJECT || tk == TYPE_CLASS_REF)
        {
            const char *cname = expr->type_of.operand->resolved_type.class_name;
            if (!cname)
                cname = "object";
            /* store as string constant */
            int ci = add_const_str(c, cname);
            if (ci >= 0 && ci < 0xFFFF)
                name_idx = (uint16_t)ci;
        }
        /* For enums, embed the enum name */
        if (tk == TYPE_ENUM)
        {
            const char *ename = expr->type_of.operand->resolved_type.enum_name;
            if (!ename)
                ename = "enum";
            int ci = add_const_str(c, ename);
            if (ci >= 0 && ci < 0xFFFF)
                name_idx = (uint16_t)ci;
        }
        /* For arrays, embed element type name + "[]" */
        if (tk == TYPE_ARRAY)
        {
            const char *ename = "unknown";
            if (expr->type_of.operand->resolved_type.element_type)
                ename = type_kind_cname(expr->type_of.operand->resolved_type.element_type->kind);
            char arr_name[64];
            snprintf(arr_name, sizeof(arr_name), "%s[]", ename);
            int ci = add_const_str(c, arr_name);
            if (ci >= 0 && ci < 0xFFFF)
                name_idx = (uint16_t)ci;
        }
        emit_op(c, OP_TYPEOF, line);
        emit_byte(c, tag, line);
        /* Emit name inline: [len][bytes...] len=0 means use built-in */
        if (name_idx != 0xFFFF)
        {
            /* name was added as const; retrieve the string we just stored */
            const char *stored = c->current_chunk->constants.values[c->current_chunk->constants.count - 1].s;
            uint8_t nlen = stored ? (uint8_t)strlen(stored) : 0;
            emit_byte(c, nlen, line);
            for (uint8_t ni = 0; ni < nlen; ni++)
                emit_byte(c, (uint8_t)stored[ni], line);
        }
        else
        {
            emit_byte(c, 0, line); /* len=0: use built-in name */
        }
        break;
    }

        /* ── Nullable operators ──────────────────────────────────────────── */

    case EXPR_NULL_LIT:
        emit_op(c, OP_PUSH_NULL, line);
        break;

    case EXPR_NULL_ASSERT:
        /* Evaluate operand, then assert it's not null at runtime */
        compile_expr(c, expr->null_assert.operand);
        emit_op(c, OP_NULL_ASSERT, line);
        emit_u16(c, (uint16_t)line, line);
        break;

    case EXPR_NULL_COALESCE:
    {
        /* left ?? right
         * Stack: eval left. If non-null, keep it and jump past right.
         * If null, pop null and eval right.
         *
         * Bytecode:
         *   <eval left>
         *   OP_NULL_COALESCE           <- pops nothing; peeks TOS, jumps if non-null
         *   [uint16 offset to after right]
         *   <eval right>
         *   <label: after right>
         */
        compile_expr(c, expr->null_coalesce.left);
        emit_op(c, OP_NULL_COALESCE, line);
        int patch_pos = c->current_chunk->count;
        emit_u16(c, 0, line); /* placeholder jump offset */
        compile_expr(c, expr->null_coalesce.right);
        /* Patch the jump */
        int after = c->current_chunk->count;
        int offset = after - (patch_pos + 2);
        c->current_chunk->code[patch_pos] = (uint8_t)((offset >> 8) & 0xFF);
        c->current_chunk->code[patch_pos + 1] = (uint8_t)(offset & 0xFF);
        break;
    }

    case EXPR_NULL_SAFE_GET:
    {
        /* obj?.field — eval obj, if null push null, else get field.
         * Uses a temp local to preserve obj for the non-null path. */
        compile_expr(c, expr->null_safe_get.object);
        int tmp_slot = c->next_slot++;
        emit_op(c, OP_STORE_LOCAL, line);
        emit_byte(c, (uint8_t)tmp_slot, line);
        emit_op(c, OP_LOAD_LOCAL, line);
        emit_byte(c, (uint8_t)tmp_slot, line);
        emit_op(c, OP_IS_NULL, line);
        int jmp_null = c->current_chunk->count;
        emit_op(c, OP_JUMP_IF_TRUE, line);
        emit_u16(c, 0, line);
        /* Non-null: load obj, get field */
        emit_op(c, OP_LOAD_LOCAL, line);
        emit_byte(c, (uint8_t)tmp_slot, line);
        int fslot = 0;
        {
            Type ot = expr->null_safe_get.object->resolved_type;
            if (ot.kind == TYPE_OBJECT && ot.class_name)
            {
                int ci = module_find_class(c->module, ot.class_name);
                if (ci >= 0)
                {
                    ClassDef *cd = &c->module->classes[ci];
                    char fname[FIELD_NAME_MAX];
                    snprintf(fname, sizeof(fname), "%.*s",
                             expr->null_safe_get.field_name_len,
                             expr->null_safe_get.field_name);
                    for (int fi = 0; fi < cd->field_count; fi++)
                    {
                        if (strcmp(cd->fields[fi].name, fname) == 0)
                        {
                            fslot = cd->fields[fi].instance_slot;
                            break;
                        }
                    }
                }
            }
        }
        emit_op(c, OP_GET_FIELD, line);
        emit_u16(c, (uint16_t)fslot, line);
        int jmp_end = c->current_chunk->count;
        emit_op(c, OP_JUMP, line);
        emit_u16(c, 0, line);
        /* Null path */
        int null_tgt = c->current_chunk->count;
        c->current_chunk->code[jmp_null + 1] = (uint8_t)((null_tgt - (jmp_null + 3)) >> 8);
        c->current_chunk->code[jmp_null + 2] = (uint8_t)((null_tgt - (jmp_null + 3)) & 0xFF);
        emit_op(c, OP_PUSH_NULL, line);
        int end_tgt = c->current_chunk->count;
        c->current_chunk->code[jmp_end + 1] = (uint8_t)((end_tgt - (jmp_end + 3)) >> 8);
        c->current_chunk->code[jmp_end + 2] = (uint8_t)((end_tgt - (jmp_end + 3)) & 0xFF);
        break;
    }
    case EXPR_NULL_SAFE_CALL:
    {
        /* obj?.method(args)
         * Similar to null_safe_get but calls a method.
         * Use same temp-local approach. */
        compile_expr(c, expr->null_safe_call.object);
        int tmp_slot = c->next_slot++;
        emit_op(c, OP_STORE_LOCAL, line);
        emit_u16(c, (uint16_t)tmp_slot, line);
        emit_op(c, OP_LOAD_LOCAL, line);
        emit_u16(c, (uint16_t)tmp_slot, line);
        emit_op(c, OP_IS_NULL, line);
        int jmp_to_null = c->current_chunk->count;
        emit_op(c, OP_JUMP_IF_TRUE, line);
        emit_u16(c, 0, line);
        /* Non-null: load obj, push args, call method */
        emit_op(c, OP_LOAD_LOCAL, line);
        emit_u16(c, (uint16_t)tmp_slot, line);
        for (ArgNode *arg = expr->null_safe_call.args; arg; arg = arg->next)
            compile_expr(c, arg->expr);
        /* Look up method slot */
        Type obj_type2 = expr->null_safe_call.object->resolved_type;
        int mslot = 0;
        if (obj_type2.kind == TYPE_OBJECT && obj_type2.class_name)
        {
            int ci = module_find_class(c->module, obj_type2.class_name);
            if (ci >= 0)
            {
                ClassDef *cd = &c->module->classes[ci];
                char mname[64];
                snprintf(mname, sizeof(mname), "%.*s",
                         expr->null_safe_call.method_name_len,
                         expr->null_safe_call.method_name);
                for (int mi = 0; mi < cd->method_count; mi++)
                {
                    if (strcmp(cd->methods[mi].name, mname) == 0)
                    {
                        mslot = mi;
                        break;
                    }
                }
            }
        }
        emit_op(c, OP_CALL_METHOD, line);
        emit_u16(c, (uint16_t)mslot, line);
        emit_byte(c, (uint8_t)expr->null_safe_call.arg_count, line);
        int jmp_to_end = c->current_chunk->count;
        emit_op(c, OP_JUMP, line);
        emit_u16(c, 0, line);
        /* Null path: push null */
        int null_target = c->current_chunk->count;
        int null_offset = null_target - (jmp_to_null + 3);
        c->current_chunk->code[jmp_to_null + 1] = (uint8_t)(null_offset >> 8);
        c->current_chunk->code[jmp_to_null + 2] = (uint8_t)(null_offset);
        emit_op(c, OP_PUSH_NULL, line);
        int end_target = c->current_chunk->count;
        int end_offset = end_target - (jmp_to_end + 3);
        c->current_chunk->code[jmp_to_end + 1] = (uint8_t)(end_offset >> 8);
        c->current_chunk->code[jmp_to_end + 2] = (uint8_t)(end_offset);
        c->local_count--;
        break;
    }

    case EXPR_EVENT_SUBSCRIBE:
    case EXPR_EVENT_UNSUBSCRIBE:
    {
        const char *ename = expr->event_sub.event_name;
        int elen = expr->event_sub.event_name_len;
        const char *hname = expr->event_sub.handler_name;
        int hlen = expr->event_sub.handler_name_len;

        /* If the handler is a static method of the current class, mangle it
         * so the VM can resolve it by chunk name. */
        char mangled[128];
        if (expr->event_sub.object == NULL && c->current_class_idx >= 0)
        {
            ClassDef *cls = &c->module->classes[c->current_class_idx];
            for (int mi = 0; mi < cls->method_count; mi++)
            {
                if (!cls->methods[mi].is_static) continue;
                int mlen = (int)strlen(cls->methods[mi].name);
                if (mlen != hlen) continue;
                if (strncmp(cls->methods[mi].name, hname, hlen) != 0) continue;
                /* Found a matching static method — mangle to "Class.method" */
                snprintf(mangled, sizeof(mangled), "%s.%.*s", cls->name, hlen, hname);
                hname = mangled;
                hlen = (int)strlen(mangled);
                break;
            }
        }

        if (expr->event_sub.object != NULL)
        {
            /* Bound delegate — push receiver then emit BOUND opcode */
            compile_expr(c, expr->event_sub.object);
            OpCode op = (expr->kind == EXPR_EVENT_SUBSCRIBE)
                            ? OP_EVENT_SUBSCRIBE_BOUND
                            : OP_EVENT_UNSUBSCRIBE_BOUND;
            emit_op(c, op, line);
        }
        else
        {
            OpCode op = (expr->kind == EXPR_EVENT_SUBSCRIBE)
                            ? OP_EVENT_SUBSCRIBE
                            : OP_EVENT_UNSUBSCRIBE;
            emit_op(c, op, line);
        }
        /* Emit event name */
        int nlen = elen < 255 ? elen : 255;
        emit_byte(c, (uint8_t)nlen, line);
        for (int i = 0; i < nlen; i++)
            emit_byte(c, (uint8_t)ename[i], line);
        /* Emit handler name */
        int hnlen = hlen < 255 ? hlen : 255;
        emit_byte(c, (uint8_t)hnlen, line);
        for (int i = 0; i < hnlen; i++)
            emit_byte(c, (uint8_t)hname[i], line);
        break;
    }

    case EXPR_EVENT_FIRE:
    {
        for (ArgNode *arg = expr->event_fire.args; arg; arg = arg->next)
            compile_expr(c, arg->expr);
        emit_op(c, OP_EVENT_FIRE, line);
        const char *ename2 = expr->event_fire.event_name;
        int elen2 = expr->event_fire.event_name_len;
        int nlen2 = elen2 < 255 ? elen2 : 255;
        emit_byte(c, (uint8_t)nlen2, line);
        for (int i = 0; i < nlen2; i++)
            emit_byte(c, (uint8_t)ename2[i], line);
        emit_byte(c, (uint8_t)expr->event_fire.arg_count, line);
        break;
    }
    case EXPR_ARRAY_LIT:
    {
        /* {e0, e1, ..., eN} — push all elements then OP_ARRAY_LIT count */
        int count = 0;
        for (ArgNode *elem = expr->array_lit.elements; elem; elem = elem->next)
        {
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
    case EXPR_INTERP_STRING:
    {
        typedef struct InterpSegment ISeg;
        int seg_count = expr->interp_string.segment_count;

        if (seg_count == 0)
        {
            /* Empty interpolated string — just push "" */
            emit_const_str(c, "", 0, line);
            break;
        }

        int pushed = 0;
        for (ISeg *seg = expr->interp_string.segments; seg; seg = seg->next)
        {
            if (!seg->is_expr)
            {
                /* Text segment — push literal string */
                emit_const_str(c, seg->text, seg->text_len, line);
            }
            else
            {
                /* Expression segment — compile and convert to string */
                compile_expr(c, seg->expr);
                TypeKind tk = seg->expr->resolved_type.kind;
                if (tk != TYPE_STRING)
                {
                    emit_op(c, OP_TO_STR, line);
                    if (tk == TYPE_ENUM)
                    {
                        /* kind=4: enum name lookup, extra byte = class index */
                        emit_byte(c, 4, line);
                        int _eci = -1;
                        if (seg->expr->resolved_type.enum_name)
                            _eci = module_find_class(c->module,
                                                     seg->expr->resolved_type.enum_name);
                        emit_byte(c, (uint8_t)(_eci >= 0 ? _eci : 0xFF), line);
                    }
                    else
                    {
                        uint8_t kind_byte =
                            (tk == TYPE_INT ||
                             tk == TYPE_SBYTE || tk == TYPE_BYTE ||
                             tk == TYPE_SHORT || tk == TYPE_USHORT ||
                             tk == TYPE_UINT || tk == TYPE_LONG ||
                             tk == TYPE_ULONG)
                                ? 0
                            : (tk == TYPE_FLOAT || tk == TYPE_DOUBLE) ? 1
                            : (tk == TYPE_CHAR)                       ? 3
                                                                      : /* char: print as character */
                                2;                                      /* bool=2 */
                        emit_byte(c, kind_byte, line);
                    }
                }
            }
            pushed++;
            /* After every second segment, concat the top two */
            if (pushed >= 2)
            {
                emit_op(c, OP_CONCAT_STR, line);
                pushed = 1; /* result is now one string on stack */
            }
        }
        break;
    }

    /* ── Variable reference ────────────────────────────────────────── */
    case EXPR_IDENT:
    {
        int slot = resolve_local(c, expr->ident.name, expr->ident.length);
        if (slot < 0)
        {
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
        if (expr->unary.op == TOK_MINUS)
        {
            if (expr->resolved_type.kind == TYPE_INT)
                emit_op(c, OP_NEGATE_INT, line);
            else
                emit_op(c, OP_NEGATE_FLOAT, line);
        }
        else
        { /* TOK_BANG */
            emit_op(c, OP_NOT_BOOL, line);
        }
        break;

    /* ── Binary operators ──────────────────────────────────────────── */
    case EXPR_BINARY:
    {
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
            if (lk != TYPE_STRING)
            {
                emit_op(c, OP_TO_STR, line);
                if (lk == TYPE_ENUM)
                {
                    emit_byte(c, 4, line);
                    int _eci = -1;
                    const char *_en = expr->binary.left->resolved_type.enum_name;
                    if (_en)
                        _eci = module_find_class(c->module, _en);
                    emit_byte(c, (uint8_t)(_eci >= 0 ? _eci : 0xFF), line);
                }
                else
                {
                    bool lk_int = (lk == TYPE_INT || lk == TYPE_SBYTE || lk == TYPE_SHORT || lk == TYPE_LONG);
                    bool lk_uint = (lk == TYPE_BYTE || lk == TYPE_USHORT || lk == TYPE_UINT || lk == TYPE_ULONG);
                    bool lk_float = (lk == TYPE_FLOAT || lk == TYPE_DOUBLE);
                    emit_byte(c, (uint8_t)(lk_int ? 0 : lk_uint       ? 5
                                                    : lk_float        ? 1
                                                    : lk == TYPE_CHAR ? 3
                                                                      : 2),
                              line);
                }
            }
            compile_expr(c, expr->binary.right);
            if (rk != TYPE_STRING)
            {
                emit_op(c, OP_TO_STR, line);
                if (rk == TYPE_ENUM)
                {
                    emit_byte(c, 4, line);
                    int _eci = -1;
                    if (expr->binary.right->resolved_type.enum_name)
                        _eci = module_find_class(c->module, expr->binary.right->resolved_type.enum_name);
                    emit_byte(c, (uint8_t)(_eci >= 0 ? _eci : 0xFF), line);
                }
                else
                {
                    bool rk_int = (rk == TYPE_INT || rk == TYPE_SBYTE || rk == TYPE_SHORT || rk == TYPE_LONG);
                    bool rk_uint = (rk == TYPE_BYTE || rk == TYPE_USHORT || rk == TYPE_UINT || rk == TYPE_ULONG);
                    bool rk_float = (rk == TYPE_FLOAT || rk == TYPE_DOUBLE);
                    emit_byte(c, (uint8_t)(rk_int ? 0 : rk_uint       ? 5
                                                    : rk_float        ? 1
                                                    : rk == TYPE_CHAR ? 3
                                                                      : 2),
                              line);
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
        if (tk == TYPE_BOOL)
        {
            TypeKind lk2 = expr->binary.left->resolved_type.kind;
            /* Check if TYPE_OBJECT is actually a generic type param name (e.g. T, K, V).
             * The parser produces TYPE_OBJECT("T") for unknown idents; we detect this
             * by matching the class_name against the current class's type param list. */
            bool lk2_is_type_param = false;
            if (lk2 == TYPE_OBJECT && c->current_class_ast)
            {
                const char *cn = expr->binary.left->resolved_type.class_name;
                if (cn)
                {
                    TypeParamNode *_tp = c->current_class_ast->class_decl.type_params;
                    for (; _tp; _tp = _tp->next)
                    {
                        if (strncmp(_tp->name, cn, _tp->length) == 0 && cn[_tp->length] == '\0')
                        {
                            lk2_is_type_param = true;
                            break;
                        }
                    }
                }
            }
            if (lk2_is_type_param)
            {
                tk = TYPE_PARAM; /* treat as generic — emit OP_CMP_EQ_VAL */
            }
            else if (lk2 != TYPE_UNKNOWN && lk2 != TYPE_OBJECT && lk2 != TYPE_PARAM && lk2 != TYPE_ANY)
            {
                tk = lk2;
            }
        }
        /* Map all int-family and char types to TYPE_INT for opcode selection */
        bool tk_is_int = (tk == TYPE_INT || tk == TYPE_LONG ||
                          tk == TYPE_SBYTE || tk == TYPE_BYTE ||
                          tk == TYPE_SHORT || tk == TYPE_USHORT ||
                          tk == TYPE_UINT || tk == TYPE_ULONG ||
                          tk == TYPE_CHAR);
        bool tk_is_float = (tk == TYPE_FLOAT || tk == TYPE_DOUBLE);

        switch (op)
        {
        /* Arithmetic */
        case TOK_PLUS:
            if (tk_is_int)
                emit_op(c, OP_ADD_INT, line);
            else if (tk_is_float)
                emit_op(c, OP_ADD_FLOAT, line);
            else
                emit_op(c, OP_CONCAT_STR, line);
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
            emit_op(c, tk == TYPE_INT ? OP_CMP_LT_INT : OP_CMP_LT_FLOAT, line);
            break;
        case TOK_LTE:
            emit_op(c, tk == TYPE_INT ? OP_CMP_LTE_INT : OP_CMP_LTE_FLOAT, line);
            break;
        case TOK_GT:
            emit_op(c, tk == TYPE_INT ? OP_CMP_GT_INT : OP_CMP_GT_FLOAT, line);
            break;
        case TOK_GTE:
            emit_op(c, tk == TYPE_INT ? OP_CMP_GTE_INT : OP_CMP_GTE_FLOAT, line);
            break;
        case TOK_EQ:
            /* Null comparison: x == null or null == x */
            if (expr->binary.left->resolved_type.kind == TYPE_NULL ||
                expr->binary.right->resolved_type.kind == TYPE_NULL)
            {
                /* Stack: ( nullable_val null_val -- bool )
                 * Pop the null literal, then IS_NULL on the other value.
                 * The non-null operand is left, null is right → pop right, IS_NULL left.
                 * Or null is left, non-null is right → need a swap... use CMP_EQ_VAL(int). */
                if (expr->binary.right->resolved_type.kind == TYPE_NULL)
                {
                    /* x == null: stack has (x, null). Pop null, then IS_NULL x */
                    emit_op(c, OP_POP, line);     /* discard null */
                    emit_op(c, OP_IS_NULL, line); /* ( x -- bool ) */
                }
                else
                {
                    /* null == x: stack has (null, x). Pop x oddly... use IS_NULL on null */
                    /* Just pop both and push true/false via CMP on i field */
                    emit_op(c, OP_CMP_EQ_VAL, line); /* uses a.i==b.i, safe for null */
                }
                break;
            }
            if (tk == TYPE_PARAM || tk == TYPE_ANY)
                emit_op(c, OP_CMP_EQ_VAL, line);
            else if (tk == TYPE_INT || tk == TYPE_ENUM || tk == TYPE_UNKNOWN)
                emit_op(c, OP_CMP_EQ_INT, line);
            else if (tk == TYPE_FLOAT)
                emit_op(c, OP_CMP_EQ_FLOAT, line);
            else if (tk == TYPE_BOOL)
                emit_op(c, OP_CMP_EQ_BOOL, line);
            else
                emit_op(c, OP_CMP_EQ_STR, line);
            break;
        case TOK_NEQ:
            /* Null comparison: x != null or null != x */
            if (expr->binary.left->resolved_type.kind == TYPE_NULL ||
                expr->binary.right->resolved_type.kind == TYPE_NULL)
            {
                if (expr->binary.right->resolved_type.kind == TYPE_NULL)
                {
                    emit_op(c, OP_POP, line);
                    emit_op(c, OP_IS_NULL, line);
                    emit_op(c, OP_NOT_BOOL, line); /* negate: IS_NULL gives true-if-null */
                }
                else
                {
                    emit_op(c, OP_CMP_NEQ_VAL, line);
                }
                break;
            }
            if (tk == TYPE_PARAM || tk == TYPE_ANY)
                emit_op(c, OP_CMP_NEQ_VAL, line);
            else if (tk == TYPE_INT || tk == TYPE_ENUM || tk == TYPE_UNKNOWN)
                emit_op(c, OP_CMP_NEQ_INT, line);
            else if (tk == TYPE_FLOAT)
                emit_op(c, OP_CMP_NEQ_FLOAT, line);
            else if (tk == TYPE_BOOL)
                emit_op(c, OP_CMP_NEQ_BOOL, line);
            else
                emit_op(c, OP_CMP_NEQ_STR, line);
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
    case EXPR_POSTFIX:
    {
        OpCode add_or_sub = expr->postfix.op == TOK_PLUS_PLUS ? OP_ADD_INT : OP_SUB_INT;

        if (!expr->postfix.is_field)
        {
            /* Simple variable lvalue */
            int slot = resolve_local(c, expr->postfix.name, expr->postfix.length);
            if (slot < 0)
            {
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
        }
        else if (expr->postfix.is_static_field)
        {
            /* Static field lvalue — ClassName.field++ / ClassName.field--
             * Strategy: LOAD_STATIC, add/sub 1, STORE_STATIC, LOAD_STATIC */
            char _cname_buf[128];
            const char *cname = strip_generic(expr->postfix.object->resolved_type.class_name, _cname_buf, sizeof(_cname_buf));
            int ci = module_find_class(c->module, cname);
            if (ci < 0)
            {
                compile_error(c, line, "Unknown class '%s'", cname);
                break;
            }
            ClassDef *cls = &c->module->classes[ci];
            int fi = -1;
            for (int k = 0; k < cls->field_count; k++)
            {
                if ((int)strlen(cls->fields[k].name) == expr->postfix.field_name_len &&
                    memcmp(cls->fields[k].name, expr->postfix.field_name,
                           expr->postfix.field_name_len) == 0)
                {
                    fi = k;
                    break;
                }
            }
            if (fi < 0)
            {
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
        }
        else
        {
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
            if (field_idx < 0)
            {
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
    case EXPR_ASSIGN:
    {
        compile_expr(c, expr->assign.value);

        /* Emit truncation if assigning into a narrow integer type */
        switch (expr->resolved_type.kind)
        {
        case TYPE_SBYTE:
            emit_op(c, OP_TRUNC_I8, line);
            break;
        case TYPE_BYTE:
            emit_op(c, OP_TRUNC_U8, line);
            break;
        case TYPE_SHORT:
            emit_op(c, OP_TRUNC_I16, line);
            break;
        case TYPE_USHORT:
            emit_op(c, OP_TRUNC_U16, line);
            break;
        case TYPE_UINT:
            emit_op(c, OP_TRUNC_U32, line);
            break;
        case TYPE_ULONG:
            emit_op(c, OP_TRUNC_U64, line);
            break;
        case TYPE_CHAR:
            emit_op(c, OP_TRUNC_CHAR, line);
            break;
        default:
            break;
        }

        /* Resolve the target slot */
        int slot = resolve_local(c, expr->assign.name, expr->assign.length);
        if (slot < 0)
        {
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
    case EXPR_CALL:
    {
        /* Check host table first so we know if args need auto-conversion */
        int host_idx = host_table_find(c->host_table,
                                       expr->call.name,
                                       expr->call.length);
        bool any_param = (host_idx >= 0 &&
                          c->host_table->decls[host_idx].has_any_param);

        /* Push all arguments onto the stack in order.
         * For TYPE_ANY host functions, auto-convert each arg to string. */
        for (ArgNode *arg = expr->call.args; arg; arg = arg->next)
        {
            compile_expr(c, arg->expr);
            if (any_param)
            {
                TypeKind ak = arg->expr->resolved_type.kind;
                if (ak != TYPE_STRING && ak != TYPE_ANY)
                {
                    emit_op(c, OP_TO_STR, line);
                    if (ak == TYPE_ENUM)
                    {
                        /* kind=4: enum name lookup.
                         * Extra byte: class index of this enum type.
                         * Find the ClassDef by enum_name. */
                        emit_byte(c, 4, line);
                        const char *ename = arg->expr->resolved_type.enum_name;
                        int eci = ename ? module_find_class(c->module, ename) : -1;
                        emit_byte(c, (uint8_t)(eci >= 0 ? eci : 0xFF), line);
                    }
                    else
                    {
                        bool ak_int = (ak == TYPE_INT || ak == TYPE_SBYTE || ak == TYPE_SHORT ||
                                       ak == TYPE_LONG);
                        bool ak_uint = (ak == TYPE_BYTE || ak == TYPE_USHORT || ak == TYPE_UINT ||
                                        ak == TYPE_ULONG);
                        bool ak_float = (ak == TYPE_FLOAT || ak == TYPE_DOUBLE);
                        emit_byte(c, (uint8_t)(ak_int ? 0 : ak_uint       ? 5
                                                        : ak_float        ? 1
                                                        : ak == TYPE_CHAR ? 3
                                                                          : 2),
                                  line);
                    }
                }
            }
        }

        if (host_idx >= 0)
        {
            emit_op(c, OP_CALL_HOST, line);
            emit_u16(c, (uint16_t)c->host_table->decls[host_idx].index, line);
            emit_byte(c, (uint8_t)expr->call.arg_count, line);
            break;
        }

        /* Resolve script function index in the module */
        int fn_idx = module_find(c->module,
                                 expr->call.name,
                                 expr->call.length);
        if (fn_idx < 0)
        {
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
    case EXPR_NEW:
    {
        /* Push constructor arguments */
        for (ArgNode *arg = expr->new_expr.args; arg; arg = arg->next)
            compile_expr(c, arg->expr);

        /* expr->new_expr.class_name is a raw source pointer — make a
         * null-terminated copy for module_find_class (which uses strcmp) */
        char class_name_buf[64];
        int clen = expr->new_expr.class_name_len < 63
                       ? expr->new_expr.class_name_len
                       : 63;
        memcpy(class_name_buf, expr->new_expr.class_name, clen);
        class_name_buf[clen] = '\0';

        int ci = compiler_ensure_class(c, class_name_buf);
        if (ci < 0)
        {
            compile_error(c, line, "Unknown class '%s'", class_name_buf);
            break;
        }
        /* Emit any missing default constructor args BEFORE the opcode */
        int ctor_argc = emit_default_args(c, expr->new_expr.resolved_params,
                                          expr->new_expr.arg_count, line);
        emit_op(c, OP_NEW, line);
        emit_u16(c, (uint16_t)ci, line);
        emit_byte(c, (uint8_t)ctor_argc, line);
        /* Emit concrete type arg kinds so the constructor frame can tag
         * generic arrays (new T[n]) with the right elem_kind at runtime.
         * Format: [uint8_t type_arg_count][uint8_t kind0][uint8_t kind1]... */
        {
            uint8_t tac = (uint8_t)(expr->new_expr.type_arg_count < XENO_MAX_TYPE_ARGS
                                        ? expr->new_expr.type_arg_count
                                        : XENO_MAX_TYPE_ARGS);
            emit_byte(c, tac, line);
            TypeArgNode *ta = expr->new_expr.type_args;
            for (int _i = 0; _i < tac && ta; _i++, ta = ta->next)
            {
                TypeKind ek = ta->type.kind;
                /* Normalize: TYPE_OBJECT "string"/"bool" → their primitive kinds */
                if (ek == TYPE_OBJECT && ta->type.class_name)
                {
                    if (strcmp(ta->type.class_name, "string") == 0)
                        ek = TYPE_STRING;
                    else if (strcmp(ta->type.class_name, "bool") == 0)
                        ek = TYPE_BOOL;
                    else if (strcmp(ta->type.class_name, "int") == 0)
                        ek = TYPE_INT;
                    else if (strcmp(ta->type.class_name, "float") == 0)
                        ek = TYPE_FLOAT;
                }
                if (ek == TYPE_PARAM || ek == TYPE_UNKNOWN)
                    ek = TYPE_ANY;
                emit_byte(c, (uint8_t)ek, line);
            }
        }
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
    case EXPR_STATIC_GET:
    {
        const char *cname = expr->static_get.class_name;
        const char *fname = expr->static_get.field_name;
        int flen = expr->static_get.field_name_len;
        int ci = module_find_class(c->module, cname);
        if (ci < 0)
        {
            compile_error(c, line, "Unknown class '%s'", cname);
            break;
        }
        int fi = find_field_index_by_class(c, cname, fname, flen);
        if (fi < 0)
        {
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
    case EXPR_STATIC_SET:
    {
        const char *cname = expr->static_set.class_name;
        const char *fname = expr->static_set.field_name;
        int flen = expr->static_set.field_name_len;
        compile_expr(c, expr->static_set.value);
        int ci = module_find_class(c->module, cname);
        if (ci < 0)
        {
            compile_error(c, line, "Unknown class '%s'", cname);
            break;
        }
        int fi = find_field_index_by_class(c, cname, fname, flen);
        if (fi < 0)
        {
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
    case EXPR_STATIC_CALL:
    {
        const char *cname = expr->static_call.class_name;
        const char *mname = expr->static_call.method_name;
        int mlen = expr->static_call.method_name_len;
        /* Push arguments */
        for (ArgNode *arg = expr->static_call.args; arg; arg = arg->next)
            compile_expr(c, arg->expr);
        /* Find the fn_index for static (no-receiver) call */
        int fn_idx = find_method_fn_index(c, cname, mname, mlen);
        if (fn_idx < 0)
        {
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
    case EXPR_SUPER_CALL:
    {
        if (c->current_class_idx < 0)
        {
            compile_error(c, line, "'super' outside class context");
            break;
        }
        ClassDef *cur_cls = &c->module->classes[c->current_class_idx];
        int par_ci = cur_cls->parent_index;
        if (par_ci < 0)
        {
            compile_error(c, line, "'super' called in class with no parent");
            break;
        }
        ClassDef *par_cls = &c->module->classes[par_ci];
        if (par_cls->constructor_index < 0)
        {
            /* Parent has no constructor — super() is a no-op */
            break;
        }
        /* Push args only — 'this' is already slot 0 of the current frame.
         * OP_CALL_SUPER emits the parent CLASS INDEX so the VM can resolve
         * the constructor via module->classes[par_ci].constructor_index at
         * runtime (always correct after module_merge remaps class defs). */
        for (ArgNode *arg = expr->super_call.args; arg; arg = arg->next)
            compile_expr(c, arg->expr);
        emit_op(c, OP_CALL_SUPER, line);
        emit_u16(c, (uint16_t)par_ci, line); /* parent class index */
        emit_byte(c, (uint8_t)expr->super_call.arg_count, line);
        break;
    }

    /* ── obj.field (read) ───────────────────────────────────────────── */
    case EXPR_FIELD_GET:
    {
        /* Type object field access: typeof(x).name / .isArray / etc */
        if (expr->field_get.object->resolved_type.kind == TYPE_CLASS_REF &&
            strncmp(expr->field_get.object->resolved_type.class_name, "Type", 4) == 0 &&
            expr->field_get.object->resolved_type.class_name[4] == '\0')
        {
            compile_expr(c, expr->field_get.object);
            /* Map field name to field_id byte */
            const char *fn = expr->field_get.field_name;
            int flen = expr->field_get.field_name_len;
            uint8_t fid = 0xFF;
            if (flen == 4 && strncmp(fn, "name", 4) == 0)
                fid = 0;
            else if (flen == 7 && strncmp(fn, "isArray", 7) == 0)
                fid = 1;
            else if (flen == 11 && strncmp(fn, "isPrimitive", 11) == 0)
                fid = 2;
            else if (flen == 6 && strncmp(fn, "isEnum", 6) == 0)
                fid = 3;
            else if (flen == 7 && strncmp(fn, "isClass", 7) == 0)
                fid = 4;
            else
            {
                compile_error(c, line, "Type has no field '%.*s'", flen, fn);
                break;
            }
            emit_op(c, OP_TYPE_FIELD, line);
            emit_byte(c, fid, line);
            break;
        }

        /* Array .length special case */
        if (expr->field_get.object->resolved_type.kind == TYPE_ARRAY)
        {
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
        if (field_idx < 0)
        {
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
    case EXPR_FIELD_SET:
    {
        compile_expr(c, expr->field_set.object);
        compile_expr(c, expr->field_set.value);

        char _fs_buf[128];
        const char *class_name = strip_generic(expr->field_set.object->resolved_type.class_name, _fs_buf, sizeof(_fs_buf));
        int field_idx = find_instance_slot_by_class(c, class_name,
                                                    expr->field_set.field_name,
                                                    expr->field_set.field_name_len);
        if (field_idx < 0)
        {
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
    case EXPR_METHOD_CALL:
    {
        /* ── Type object attribute reflection methods ───────────────────── */
        {
            Type obj_rt = expr->method_call.object->resolved_type;
            if (obj_rt.kind == TYPE_CLASS_REF &&
                obj_rt.class_name &&
                strcmp(obj_rt.class_name, "Type") == 0)
            {
                const char *mname = expr->method_call.method_name;
                int mlen = expr->method_call.method_name_len;
                if (mlen == 12 && memcmp(mname, "hasAttribute", 12) == 0)
                {
                    /* Push: arg0 (name string), then Type object */
                    compile_expr(c, expr->method_call.args->expr);
                    compile_expr(c, expr->method_call.object);
                    emit_op(c, OP_TYPE_HAS_ATTR, line);
                    break;
                }
                if (mlen == 15 && memcmp(mname, "getAttributeArg", 15) == 0)
                {
                    /* Push: arg1 (index int), arg0 (name string), then Type object */
                    compile_expr(c, expr->method_call.args->next->expr); /* index */
                    compile_expr(c, expr->method_call.args->expr);       /* name  */
                    compile_expr(c, expr->method_call.object);
                    emit_op(c, OP_TYPE_GET_ATTR_ARG, line);
                    break;
                }
            }
        }

        /* Push receiver object first, then arguments */
        compile_expr(c, expr->method_call.object);
        for (ArgNode *arg = expr->method_call.args; arg; arg = arg->next)
            compile_expr(c, arg->expr);

        const char *class_name_raw = expr->method_call.object->resolved_type.class_name;
        char class_name_buf[128];
        const char *class_name = strip_generic(class_name_raw, class_name_buf, sizeof(class_name_buf));
        int ci = module_find_class(c->module, class_name);

        if (ci < 0)
        {
            /* class_name is an interface — emit a virtual dispatch.
             * Store the method name as a string constant; the VM
             * looks up the concrete fn_index on the actual object. */
            char mname_buf[256];
            int mlen = expr->method_call.method_name_len < 255
                           ? expr->method_call.method_name_len
                           : 255;
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

        int fn_idx = find_method_slot(c, class_name,
                                      expr->method_call.method_name,
                                      expr->method_call.method_name_len);
        if (fn_idx < 0)
        {
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

static void compile_block(Compiler *c, const Stmt *block)
{
    scope_push(c);
    for (StmtNode *n = block->block.stmts; n; n = n->next)
        compile_stmt(c, n->stmt);
    scope_pop(c, block->line);
}

static void compile_stmt(Compiler *c, const Stmt *stmt)
{
    int line = stmt->line;

    switch (stmt->kind)
    {

    /* ── Variable declaration ──────────────────────────────────────── */
    case STMT_VAR_DECL:
    {
        TypeKind vtype = stmt->var_decl.type.kind;
        /* Compile the initializer if present, else push a zero value */
        if (stmt->var_decl.init)
        {
            compile_expr(c, stmt->var_decl.init);
        }
        else
        {
            /* Default zero-initialize based on declared type */
            switch (vtype)
            {
            case TYPE_INT:
            case TYPE_SBYTE:
            case TYPE_BYTE:
            case TYPE_SHORT:
            case TYPE_USHORT:
            case TYPE_UINT:
            case TYPE_LONG:
            case TYPE_ULONG:
            case TYPE_CHAR:
                emit_const_int(c, 0, line);
                break;
            case TYPE_ARRAY:
                /* Default: empty zero-length array */
                emit_const_int(c, 0, line);
                emit_op(c, OP_NEW_ARRAY, line);
                emit_byte(c, (uint8_t)TYPE_ANY, line); /* elem_kind unknown at default init */
                break;
            case TYPE_FLOAT:
            case TYPE_DOUBLE:
                emit_const_float(c, 0.0, line);
                break;
            case TYPE_BOOL:
                emit_op(c, OP_LOAD_CONST_BOOL, line);
                emit_byte(c, 0, line);
                break;
            case TYPE_STRING:
                emit_const_str(c, "", 0, line);
                break;
            default:
                break;
            }
        }

        /* Emit truncation for narrow integer types so overflow wraps correctly */
        switch (vtype)
        {
        case TYPE_SBYTE:
            emit_op(c, OP_TRUNC_I8, line);
            break;
        case TYPE_BYTE:
            emit_op(c, OP_TRUNC_U8, line);
            break;
        case TYPE_SHORT:
            emit_op(c, OP_TRUNC_I16, line);
            break;
        case TYPE_USHORT:
            emit_op(c, OP_TRUNC_U16, line);
            break;
        case TYPE_UINT:
            emit_op(c, OP_TRUNC_U32, line);
            break;
        case TYPE_ULONG:
            emit_op(c, OP_TRUNC_U64, line);
            break;
        case TYPE_CHAR:
            emit_op(c, OP_TRUNC_CHAR, line);
            break;
        default:
            break;
        }

        /* Declare the local and store the value into its slot */
        int slot = declare_local(c, stmt->var_decl.name,
                                 stmt->var_decl.length);
        emit_op(c, OP_STORE_LOCAL, line);
        emit_byte(c, (uint8_t)slot, line);
        break;
    }

    /* ── Expression statement ──────────────────────────────────────── */
    case STMT_EXPR:
    {
        compile_expr(c, stmt->expr.expr);
        /* Expression statements discard the result value.
         * EXPR_SUPER_CALL produces no stack value (constructor return
         * suppresses the dummy push), so skip the POP for it.
         * EXPR_EVENT_SUBSCRIBE / UNSUBSCRIBE are void — they consume
         * their receiver (if bound) internally and push nothing. */
        ExprKind k = stmt->expr.expr->kind;
        if (k != EXPR_SUPER_CALL &&
            k != EXPR_EVENT_SUBSCRIBE &&
            k != EXPR_EVENT_UNSUBSCRIBE &&
            k != EXPR_EVENT_FIRE)
        {
            emit_op(c, OP_POP, line);
        }
        break;
    }

    /* ── Block ─────────────────────────────────────────────────────── */
    case STMT_BLOCK:
        compile_block(c, stmt);
        break;

    /* ── If statement ──────────────────────────────────────────────── */
    case STMT_IF:
    {
        /* Compile condition — leaves a bool on the stack */
        compile_expr(c, stmt->if_stmt.condition);

        /* Jump past then-branch if false */
        int then_jump = emit_jump(c, OP_JUMP_IF_FALSE, line);

        /* Compile then-branch */
        compile_block(c, stmt->if_stmt.then_branch);

        if (stmt->if_stmt.else_branch)
        {
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
        }
        else
        {
            /* No else — patch then-jump to land right here */
            patch_jump(c, then_jump);
        }
        break;
    }

    /* ── While loop ────────────────────────────────────────────────── */
    case STMT_WHILE:
    {
        int depth = c->loop_depth;
        c->break_count[depth] = 0;
        c->continue_count[depth] = 0;

        /* Record where the loop condition starts */
        int loop_start = c->current_chunk->count;
        c->loop_start = loop_start;
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
        for (int i = 0; i < c->continue_count[depth]; i++)
        {
            int pos = c->continue_patches[depth][i];
            int offset = loop_start - (pos + 2);
            c->current_chunk->code[pos] = (offset >> 8) & 0xFF;
            c->current_chunk->code[pos + 1] = offset & 0xFF;
        }
        break;
    }

    /* ── For loop ──────────────────────────────────────────────────── */
    case STMT_FOR:
    {
        int depth = c->loop_depth;
        c->break_count[depth] = 0;
        c->continue_count[depth] = 0;

        /* For loop introduces its own scope for the init variable */
        scope_push(c);

        /* Init */
        if (stmt->for_stmt.init)
            compile_stmt(c, stmt->for_stmt.init);

        /* Condition */
        int loop_start = c->current_chunk->count;
        int exit_jump = -1;

        if (stmt->for_stmt.condition)
        {
            compile_expr(c, stmt->for_stmt.condition);
            exit_jump = emit_jump(c, OP_JUMP_IF_FALSE, line);
        }

        /* Body */
        c->loop_depth++;
        compile_block(c, stmt->for_stmt.body);
        c->loop_depth--;

        /* Step — continue jumps land HERE (before the step runs) */
        int continue_target = c->current_chunk->count;
        if (stmt->for_stmt.step)
        {
            compile_expr(c, stmt->for_stmt.step);
            emit_op(c, OP_POP, line); /* step result is discarded */
        }

        /* Jump back to condition */
        emit_loop(c, loop_start, line);

        /* Patch exit jump */
        if (exit_jump >= 0)
            patch_jump(c, exit_jump);

        scope_pop(c, line);

        /* Patch all break jumps to land here (after loop) */
        for (int i = 0; i < c->break_count[depth]; i++)
            patch_jump(c, c->break_patches[depth][i]);

        /* Patch all continue jumps to land at continue_target (before step) */
        for (int i = 0; i < c->continue_count[depth]; i++)
        {
            int pos = c->continue_patches[depth][i];
            int offset = continue_target - (pos + 2);
            c->current_chunk->code[pos] = (offset >> 8) & 0xFF;
            c->current_chunk->code[pos + 1] = offset & 0xFF;
        }
        break;
    }

    case STMT_FOREACH:
    {
        int depth = c->loop_depth;
        c->break_count[depth] = 0;
        c->continue_count[depth] = 0;
        scope_push(c);
        compile_expr(c, stmt->foreach_stmt.array);
        int arr_slot = declare_local(c, "__arr", 5);
        emit_op(c, OP_STORE_LOCAL, line);
        emit_byte(c, (uint8_t)arr_slot, line);
        emit_const_int(c, 0, line);
        int idx_slot = declare_local(c, "__i", 2);
        emit_op(c, OP_STORE_LOCAL, line);
        emit_byte(c, (uint8_t)idx_slot, line);
        int loop_start = c->current_chunk->count;
        emit_op(c, OP_LOAD_LOCAL, line);
        emit_byte(c, (uint8_t)idx_slot, line);
        emit_op(c, OP_LOAD_LOCAL, line);
        emit_byte(c, (uint8_t)arr_slot, line);
        emit_op(c, OP_ARRAY_LEN, line);
        emit_op(c, OP_CMP_LT_INT, line);
        int exit_jump = emit_jump(c, OP_JUMP_IF_FALSE, line);
        emit_op(c, OP_LOAD_LOCAL, line);
        emit_byte(c, (uint8_t)arr_slot, line);
        emit_op(c, OP_LOAD_LOCAL, line);
        emit_byte(c, (uint8_t)idx_slot, line);
        emit_op(c, OP_ARRAY_GET, line);
        int var_slot = declare_local(c, stmt->foreach_stmt.var_name, stmt->foreach_stmt.var_len);
        emit_op(c, OP_STORE_LOCAL, line);
        emit_byte(c, (uint8_t)var_slot, line);
        c->loop_depth++;
        compile_block(c, stmt->foreach_stmt.body);
        c->loop_depth--;
        int continue_target = c->current_chunk->count;
        emit_op(c, OP_LOAD_LOCAL, line);
        emit_byte(c, (uint8_t)idx_slot, line);
        emit_const_int(c, 1, line);
        emit_op(c, OP_ADD_INT, line);
        emit_op(c, OP_STORE_LOCAL, line);
        emit_byte(c, (uint8_t)idx_slot, line);
        emit_loop(c, loop_start, line);
        patch_jump(c, exit_jump);
        scope_pop(c, line);
        for (int i = 0; i < c->break_count[depth]; i++)
            patch_jump(c, c->break_patches[depth][i]);
        for (int i = 0; i < c->continue_count[depth]; i++)
        {
            int pos = c->continue_patches[depth][i], off = continue_target - (pos + 2);
            c->current_chunk->code[pos] = (off >> 8) & 0xFF;
            c->current_chunk->code[pos + 1] = off & 0xFF;
        }
        break;
    }

    /* ── Return ────────────────────────────────────────────────────── */
    case STMT_RETURN:
        if (stmt->return_stmt.value)
        {
            compile_expr(c, stmt->return_stmt.value);
            emit_op(c, OP_RETURN, line);
        }
        else
        {
            emit_op(c, OP_RETURN_VOID, line);
        }
        break;

    /* ── Break / Continue ───────────────────────────────────────────── */
    case STMT_BREAK:
        /* break is valid inside both loops and match arms —
         * both use loop_depth to track the break-patch level */
        if (c->loop_depth == 0)
        {
            compile_error(c, line, "'break' outside of loop or match");
            break;
        }
        else
        {
            int depth = c->loop_depth - 1;
            if (c->break_count[depth] >= MAX_LOOP_PATCHES)
            {
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
    case STMT_MATCH:
    {
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
        int pending_cmp_skip = -1;    /* JUMP_IF_FALSE awaiting comp_{N+1} */
        int pending_fallthrough = -1; /* JUMP awaiting body_{N+1}          */
        TypeKind subject_kind = stmt->match_stmt.subject->resolved_type.kind;

        for (MANode *arm = stmt->match_stmt.arms; arm; arm = arm->next)
        {
            /* ── Start of this arm's comparison ──────────────────────────
             * Patch the previous arm's failed comparison jump to HERE. */
            if (pending_cmp_skip >= 0)
            {
                patch_jump(c, pending_cmp_skip);
                pending_cmp_skip = -1;
            }

            if (!arm->is_default)
            {
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
            if (pending_fallthrough >= 0)
            {
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
            if (arm->next)
            {
                pending_fallthrough = emit_jump(c, OP_JUMP, line);
            }
            (void)break_count_before;
        }

        /* Patch the last failed-comparison jump to land here (no-match zone) */
        if (pending_cmp_skip >= 0)
        {
            patch_jump(c, pending_cmp_skip);
        }
        /* Any remaining pending fall-through also lands here
         * (last arm has no break and no next arm — falls into no-match zone) */
        if (pending_fallthrough >= 0)
        {
            patch_jump(c, pending_fallthrough);
        }

        /* If no default, a no-match is a runtime error */
        if (!stmt->match_stmt.has_default)
        {
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
        if (c->loop_depth == 0)
        {
            compile_error(c, line, "'continue' outside of loop");
            break;
        }
        else
        {
            int depth = c->loop_depth - 1;
            if (c->continue_count[depth] >= MAX_LOOP_PATCHES)
            {
                compile_error(c, line, "Too many continue statements in one loop");
                break;
            }
            int patch = emit_jump(c, OP_JUMP, line);
            c->continue_patches[depth][c->continue_count[depth]++] = patch;
        }
        break;

    /* ── throw statement ─────────────────────────────────────────────── */
    case STMT_THROW:
    {
        compile_expr(c, stmt->throw_stmt.value);
        emit_op(c, OP_THROW, line);
        break;
    }

    /* ── try / catch / finally ──────────────────────────────────────── */
    case STMT_TRY:
    {
        int catch_count = stmt->try_stmt.catch_count;

        /* OP_TRY_BEGIN [u16 catch_dispatch_offset] */
        int try_begin_patch = emit_jump(c, OP_TRY_BEGIN, line);

        /* Compile try body */
        compile_stmt(c, stmt->try_stmt.body);

        /* OP_TRY_END — normal exit, pop handler frame */
        emit_op(c, OP_TRY_END, line);

        /* Jump over all catch blocks */
        int over_catch_patch = emit_jump(c, OP_JUMP, line);

        /* Patch TRY_BEGIN to point here — the catch dispatch area */
        patch_jump(c, try_begin_patch);

        /* Catch dispatch: for each clause, check the type and branch */
        int *clause_end_patches = NULL;
        if (catch_count > 0)
            clause_end_patches = malloc(catch_count * sizeof(int));

        for (int ci = 0; ci < catch_count; ci++)
        {
            /* If this is not the last clause, we need a type check.
             * Load exception, check isinstance, jump-if-false to next. */
            int skip_patch = -1;
            if (ci < catch_count - 1)
            {
                /* Check: is caught_exception instanceof catch_types[ci]?
                 * We use a new opcode OP_EXCEPTION_IS_TYPE [u8 name_len][name_bytes] */
                emit_op(c, OP_EXCEPTION_IS_TYPE, line);
                /* emit the type name as length-prefixed bytes */
                int tlen = stmt->try_stmt.catch_type_lens[ci];
                const char *tname = stmt->try_stmt.catch_types[ci];
                if (tlen > 255)
                    tlen = 255;
                emit_byte(c, (uint8_t)tlen, line);
                for (int bi = 0; bi < tlen; bi++)
                    emit_byte(c, (uint8_t)tname[bi], line);
                skip_patch = emit_jump(c, OP_JUMP_IF_FALSE, line);
            }

            /* Catch body: introduce the variable */
            scope_push(c);
            emit_op(c, OP_LOAD_EXCEPTION, line);
            int slot = declare_local(c, stmt->try_stmt.catch_vars[ci],
                                     stmt->try_stmt.catch_var_lens[ci]);
            emit_byte(c, OP_STORE_LOCAL, line);
            emit_byte(c, (uint8_t)slot, line);

            compile_stmt(c, stmt->try_stmt.catch_bodies[ci]);
            scope_pop(c, line);

            /* Jump past remaining clauses */
            clause_end_patches[ci] = emit_jump(c, OP_JUMP, line);

            if (skip_patch >= 0)
                patch_jump(c, skip_patch);
        }

        /* Patch all clause-end jumps to here */
        for (int ci = 0; ci < catch_count; ci++)
            patch_jump(c, clause_end_patches[ci]);
        if (clause_end_patches)
            free(clause_end_patches);

        /* Patch the over-catch jump to here */
        patch_jump(c, over_catch_patch);

        /* Finally block — emitted after both normal and exception paths.
         * Simple: just compile it inline (it runs in both paths for now). */
        if (stmt->try_stmt.finally_body)
            compile_stmt(c, stmt->try_stmt.finally_body);

        break;
    }

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

    case STMT_EVENT_DECL:
        /* Event declarations are registered in Pass 1.6 — nothing to emit
         * at the statement level. The runtime handler table is managed by
         * the VM using OP_EVENT_SUBSCRIBE / OP_EVENT_FIRE. */
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

void compiler_host_table_init(CompilerHostTable *t)
{
    t->count = 0;
}

void compiler_host_table_add(CompilerHostTable *t,
                             const char *name, int index, int param_count)
{
    if (t->count >= COMPILER_MAX_HOST_DECLS)
        return;
    CompilerHostDecl *d = &t->decls[t->count++];
    d->name = name;
    d->index = index;
    d->param_count = param_count;
    d->has_any_param = false;
}

void compiler_host_table_add_any(CompilerHostTable *t,
                                 const char *name, int index, int param_count)
{
    if (t->count >= COMPILER_MAX_HOST_DECLS)
        return;
    CompilerHostDecl *d = &t->decls[t->count++];
    d->name = name;
    d->index = index;
    d->param_count = param_count;
    d->has_any_param = true;
}

static int host_table_find(const CompilerHostTable *t,
                           const char *name, int len)
{
    if (!t)
        return -1;
    for (int i = 0; i < t->count; i++)
    {
        if ((int)strlen(t->decls[i].name) == len &&
            memcmp(t->decls[i].name, name, len) == 0)
            return i;
    }
    return -1;
}

/* ─────────────────────────────────────────────────────────────────────────────
 * ANNOTATION EVALUATION
 *
 * Walks an annotation argument expression and resolves it to an AttrArg.
 * Handles: string/int/float/bool literals, enum member access (Foo.Bar),
 * static field references with literal initializers (constant folding),
 * and array literals (recursively).
 *
 * Returns true on success. On failure writes a compile_error and returns false.
 * ───────────────────────────────────────────────────────────────────────────*/
static bool eval_attr_arg(Compiler *c, const Expr *expr, AttrArg *out)
{
    if (!expr || !out)
        return false;
    memset(out, 0, sizeof(AttrArg));

    switch (expr->kind)
    {
    case EXPR_STRING_LIT:
        out->kind = ATTR_ARG_STRING;
        {
            int vlen = expr->string_lit.length;
            out->s = malloc((size_t)vlen + 1);
            if (!out->s)
                return false;
            memcpy(out->s, expr->string_lit.chars, vlen);
            out->s[vlen] = '\0';
        }
        return true;

    case EXPR_INT_LIT:
        out->kind = ATTR_ARG_INT;
        out->i = expr->int_lit.value;
        return true;

    case EXPR_FLOAT_LIT:
        out->kind = ATTR_ARG_FLOAT;
        out->f = expr->float_lit.value;
        return true;

    case EXPR_BOOL_LIT:
        out->kind = ATTR_ARG_BOOL;
        out->b = expr->bool_lit.value;
        return true;

    case EXPR_ENUM_ACCESS:
        /* Checker fills in .value — treat as int */
        out->kind = ATTR_ARG_INT;
        out->i = (int64_t)expr->enum_access.value;
        return true;

    case EXPR_FIELD_GET:
        /* Unresolved enum access — checker couldn't rewrite because the enum
         * type wasn't in scope (happens during stdlib bootstrap compilation).
         * Store as int 0; the resulting annotation data is not used for
         * enforcement in the bootstrap phase. */
        out->kind = ATTR_ARG_INT;
        out->i = 0;
        return true;

    case EXPR_STATIC_GET:
    {
        /* Constant-fold: walk the AST to find the class's static field
         * initializer and recursively evaluate it. class_idx/field_idx
         * are always -1 at this point (filled by the compiler later for
         * runtime use), so we resolve purely by name. */
        if (!c->program)
        {
            compile_error(c, expr->line,
                          "Annotation: cannot constant-fold static field (no program ref)");
            return false;
        }
        char target_class[CLASS_NAME_MAX], target_field[FIELD_NAME_MAX];
        int tcl = expr->static_get.class_name_len < CLASS_NAME_MAX - 1
                      ? expr->static_get.class_name_len
                      : CLASS_NAME_MAX - 1;
        int tfl = expr->static_get.field_name_len < FIELD_NAME_MAX - 1
                      ? expr->static_get.field_name_len
                      : FIELD_NAME_MAX - 1;
        memcpy(target_class, expr->static_get.class_name, tcl);
        target_class[tcl] = '\0';
        memcpy(target_field, expr->static_get.field_name, tfl);
        target_field[tfl] = '\0';

        for (StmtNode *sn = c->program->stmts; sn; sn = sn->next)
        {
            if (sn->stmt->kind != STMT_CLASS_DECL)
                continue;
            if ((int)sn->stmt->class_decl.length != tcl ||
                memcmp(sn->stmt->class_decl.name, target_class, tcl) != 0)
                continue;
            for (struct ClassFieldNode *f = sn->stmt->class_decl.fields; f; f = f->next)
            {
                if (!f->is_static || !f->initializer)
                    continue;
                if (f->length != tfl ||
                    memcmp(f->name, target_field, tfl) != 0)
                    continue;
                return eval_attr_arg(c, f->initializer, out);
            }
        }
        compile_error(c, expr->line,
                      "Annotation: static field '%s.%s' has no constant initializer",
                      target_class, target_field);
        return false;
    }

    case EXPR_ARRAY_LIT:
    {
        out->kind = ATTR_ARG_ARRAY;
        int cnt = expr->array_lit.count;
        if (cnt > ATTR_MAX_ARGS)
            cnt = ATTR_MAX_ARGS;
        out->arr.count = cnt;
        if (cnt == 0)
        {
            out->arr.elems = NULL;
            return true;
        }
        out->arr.elems = malloc((size_t)cnt * sizeof(AttrArg));
        if (!out->arr.elems)
            return false;
        ArgNode *el = expr->array_lit.elements;
        for (int i = 0; i < cnt && el; i++, el = el->next)
        {
            if (!eval_attr_arg(c, el->expr, &out->arr.elems[i]))
            {
                /* cleanup already-allocated elements */
                for (int j = 0; j < i; j++)
                    if (out->arr.elems[j].kind == ATTR_ARG_ARRAY)
                        free(out->arr.elems[j].arr.elems);
                free(out->arr.elems);
                out->arr.elems = NULL;
                return false;
            }
        }
        return true;
    }

    default:
        compile_error(c, expr->line,
                      "Annotation argument must be a literal, enum member, static field, or array");
        return false;
    }
}

/* ── Apply all annotations on a class declaration to cls->attributes[] ─── */
static void compile_class_annotations(Compiler *c, const Stmt *s, ClassDef *cls)
{
    for (AnnotationNode *ann = s->class_decl.annotations; ann; ann = ann->next)
    {
        if (cls->attribute_count >= CLASS_MAX_ATTRIBUTES)
        {
            compile_error(c, s->line, "Too many attributes on class '%s'", cls->name);
            break;
        }
        AttributeInstance *inst = &cls->attributes[cls->attribute_count++];
        memset(inst, 0, sizeof(AttributeInstance));

        int nlen = ann->name_len < CLASS_NAME_MAX - 1 ? ann->name_len : CLASS_NAME_MAX - 1;
        memcpy(inst->class_name, ann->name, nlen);
        inst->class_name[nlen] = '\0';

        for (AnnotationKVNode *kv = ann->args; kv; kv = kv->next)
        {
            if (inst->arg_count >= ATTR_MAX_ARGS)
                break;
            if (!eval_attr_arg(c, kv->value, &inst->args[inst->arg_count]))
                break;
            inst->arg_count++;
        }
    }
}

/* ── Apply annotations on a method to md->attributes[] ─────────────────── */
static void compile_method_annotations(Compiler *c, int line,
                                       AnnotationNode *annotations, MethodDef *md)
{
    /* Count annotations first */
    int count = 0;
    for (AnnotationNode *ann = annotations; ann; ann = ann->next)
        count++;
    if (count == 0)
    {
        md->attributes = NULL;
        md->attribute_count = 0;
        return;
    }
    if (count > CLASS_MAX_ATTRIBUTES)
        count = CLASS_MAX_ATTRIBUTES;

    md->attributes = calloc((size_t)count, sizeof(AttributeInstance));
    if (!md->attributes)
    {
        compile_error(c, line, "Out of memory for method attributes");
        return;
    }
    md->attribute_count = 0;

    for (AnnotationNode *ann = annotations; ann && md->attribute_count < count;
         ann = ann->next)
    {
        AttributeInstance *inst = &md->attributes[md->attribute_count++];
        memset(inst, 0, sizeof(AttributeInstance));

        int nlen = ann->name_len < CLASS_NAME_MAX - 1 ? ann->name_len : CLASS_NAME_MAX - 1;
        memcpy(inst->class_name, ann->name, nlen);
        inst->class_name[nlen] = '\0';

        for (AnnotationKVNode *kv = ann->args; kv; kv = kv->next)
        {
            if (inst->arg_count >= ATTR_MAX_ARGS)
                break;
            if (!eval_attr_arg(c, kv->value, &inst->args[inst->arg_count]))
                break;
            inst->arg_count++;
        }
    }
}

bool compiler_compile(Compiler *c, const Program *program, Module *module,
                      const CompilerHostTable *host_table)
{
    return compiler_compile_staged(c, program, module, host_table, NULL);
}

bool compiler_compile_staged(Compiler *c, const Program *program, Module *module,
                             const CompilerHostTable *host_table,
                             const Module *staging)
{
    /* Initialize compiler state */
    memset(c, 0, sizeof(Compiler));
    c->module = module;
    c->host_table = host_table;
    c->program = program;
    c->staging = staging; /* NULL if not provided */
    c->current_chunk = NULL;
    c->had_error = false;
    c->error_count = 0;
    c->current_class_idx = -1;
    c->current_class_ast = NULL;

    /* ── PASS 1: Register all names ─────────────────────────────────────────
     *
     * Walk top-level declarations and pre-allocate:
     *   - One chunk per top-level function
     *   - One ClassDef per class, plus one chunk per method/constructor
     *
     * This makes forward references work in both directions.
     * ────────────────────────────────────────────────────────────────────*/
    for (StmtNode *n = program->stmts; n; n = n->next)
    {
        Stmt *s = n->stmt;

        if (s->kind == STMT_IMPORT)
            continue;

        if (s->kind == STMT_FN_DECL)
        {
            if (c->module->count >= MODULE_MAX_FUNCTIONS)
            {
                compile_error(c, s->line, "Too many functions");
                break;
            }
            int fn_idx = module_add_chunk(c->module);
            int namelen = s->fn_decl.length < 63 ? s->fn_decl.length : 63;
            memcpy(c->module->names[fn_idx], s->fn_decl.name, namelen);
            c->module->names[fn_idx][namelen] = '\0';
        }
        else if (s->kind == STMT_CLASS_DECL)
        {
            if (c->module->class_count >= MODULE_MAX_CLASSES)
            {
                compile_error(c, s->line, "Too many classes");
                break;
            }

            /* Reuse existing slot if this class was pre-seeded from staging */
            int namelen = s->class_decl.length < CLASS_NAME_MAX - 1
                              ? s->class_decl.length
                              : CLASS_NAME_MAX - 1;
            char cname[CLASS_NAME_MAX];
            memcpy(cname, s->class_decl.name, namelen);
            cname[namelen] = '\0';

            int ci = module_find_class(c->module, cname);
            if (ci < 0)
            {
                ci = c->module->class_count++;
            }
            ClassDef *cls = &c->module->classes[ci];
            memset(cls, 0, sizeof(ClassDef));

            memcpy(cls->name, cname, namelen);
            cls->name[namelen] = '\0';
            cls->constructor_index = -1;
            cls->parent_index = -1;

            /* ── Store generic type parameter names ────────────────── */
            cls->type_param_count = 0;
            {
                TypeParamNode *tp = s->class_decl.type_params;
                for (; tp && cls->type_param_count < 8; tp = tp->next)
                {
                    int tl = tp->length < 7 ? tp->length : 7;
                    memcpy(cls->type_param_names[cls->type_param_count], tp->name, tl);
                    cls->type_param_names[cls->type_param_count][tl] = '\0';
                    cls->type_param_count++;
                }
            }

            /* ── Store implemented interface names ──────────────────── */
            cls->is_interface = false; /* concrete class */
            cls->interface_count = 0;
            {
                typedef struct IfaceNameNode IFNode2;
                IFNode2 *in = s->class_decl.interfaces;
                for (; in && cls->interface_count < 8; in = in->next)
                {
                    char buf[64];
                    int nlen = in->length < 63 ? in->length : 63;
                    memcpy(buf, in->name, nlen);
                    buf[nlen] = '\0';
                    /* Append type arg if present, e.g. "IEnumerable<int>" */
                    if (in->type_arg_count >= 1)
                    {
                        /* type_args[0].class_name for object types, or kind name */
                        const char *targ = NULL;
                        char kind_buf[16];
                        if (in->type_args[0].kind == TYPE_OBJECT && in->type_args[0].class_name)
                            targ = in->type_args[0].class_name;
                        else
                        {
                            const char *kn = type_kind_name(in->type_args[0].kind);
                            snprintf(kind_buf, sizeof(kind_buf), "%s", kn ? kn : "?");
                            targ = kind_buf;
                        }
                        int curlen = (int)strlen(buf);
                        int targlen = (int)strlen(targ);
                        if (curlen + targlen + 3 < 63)
                        {
                            buf[curlen] = '<';
                            memcpy(buf + curlen + 1, targ, targlen);
                            buf[curlen + 1 + targlen] = '>';
                            buf[curlen + 2 + targlen] = '\0';
                        }
                    }
                    memcpy(cls->interface_names[cls->interface_count], buf, strlen(buf) + 1);
                    cls->interface_count++;
                }
            }

            compile_class_annotations(c, s, cls);

            /* ── Flatten parent fields and methods into this ClassDef ──────
             * Copy ancestor members first so their field indices come before
             * the child's own fields — giving a stable, predictable layout.
             * Parent ClassDef must already be registered (Pass 1 is ordered
             * so that forward-declared parents work because the module is
             * built in source order; parents should be declared first).   */
            if (s->class_decl.parent_name && s->class_decl.parent_length > 0)
            {
                char par_name[CLASS_NAME_MAX];
                int par_len = s->class_decl.parent_length < CLASS_NAME_MAX - 1
                                  ? s->class_decl.parent_length
                                  : CLASS_NAME_MAX - 1;
                memcpy(par_name, s->class_decl.parent_name, par_len);
                par_name[par_len] = '\0';
                int par_ci = compiler_ensure_class(c, par_name);
                if (par_ci >= 0)
                {
                    cls->parent_index = par_ci;
                    ClassDef *par_cls = &c->module->classes[par_ci];

                    /* Resolve concrete args for generic parent (e.g. Box<int>) */
                    int par_tp_count = par_cls->type_param_count;
                    int par_ca_count = s->class_decl.parent_type_arg_count;
                    Type par_concrete[8];
                    if (par_ca_count > 8)
                        par_ca_count = 8;
                    for (int _i = 0; _i < par_ca_count; _i++)
                        par_concrete[_i] = s->class_decl.parent_type_args
                                               ? s->class_decl.parent_type_args[_i]
                                               : type_any();

                    /* Copy parent fields -- substitute TYPE_PARAM with concrete */
                    for (int fi = 0; fi < par_cls->field_count; fi++)
                    {
                        if (cls->field_count >= CLASS_MAX_FIELDS)
                        {
                            compile_error(c, s->line,
                                          "Class '%s': too many fields after inheriting from '%s'",
                                          cls->name, par_name);
                            break;
                        }
                        FieldDef fd = par_cls->fields[fi];
                        if (fd.type_kind == (int)TYPE_PARAM &&
                            par_tp_count > 0 && par_ca_count > 0)
                        {
                            for (int _pi = 0; _pi < par_tp_count && _pi < par_ca_count; _pi++)
                            {
                                if (strcmp(fd.class_name,
                                           par_cls->type_param_names[_pi]) == 0)
                                {
                                    Type conc = par_concrete[_pi];
                                    fd.type_kind = (int)conc.kind;
                                    if (conc.kind == TYPE_OBJECT && conc.class_name)
                                        strncpy(fd.class_name, conc.class_name,
                                                CLASS_NAME_MAX - 1);
                                    else
                                        fd.class_name[0] = '\0';
                                    break;
                                }
                            }
                        }
                        cls->fields[cls->field_count++] = fd;
                    }

                    /* Copy parent methods -- substitute return TYPE_PARAM */
                    for (int mi = 0; mi < par_cls->method_count; mi++)
                    {
                        if (cls->method_count >= CLASS_MAX_METHODS)
                        {
                            compile_error(c, s->line,
                                          "Class '%s': too many methods after inheriting from '%s'",
                                          cls->name, par_name);
                            break;
                        }
                        MethodDef md = par_cls->methods[mi];
                        if (md.return_type_kind == (int)TYPE_PARAM &&
                            par_tp_count > 0 && par_ca_count > 0)
                        {
                            for (int _pi = 0; _pi < par_tp_count && _pi < par_ca_count; _pi++)
                            {
                                if (strcmp(md.return_class_name,
                                           par_cls->type_param_names[_pi]) == 0)
                                {
                                    Type conc = par_concrete[_pi];
                                    md.return_type_kind = (int)conc.kind;
                                    if (conc.kind == TYPE_OBJECT && conc.class_name)
                                        strncpy(md.return_class_name, conc.class_name,
                                                CLASS_NAME_MAX - 1);
                                    else
                                        md.return_class_name[0] = '\0';
                                    break;
                                }
                            }
                        }
                        cls->methods[cls->method_count++] = md;
                    }
                    cls->constructor_index = par_cls->constructor_index;
                }
            }

            /* Register fields (both instance and static).
             * instance_slot tracks the consecutive slot index for instance
             * fields only — this is what GET_FIELD/SET_FIELD use at runtime.
             * Static fields get instance_slot = -1.
             * Start after any inherited instance fields so slots don't overlap. */
            typedef struct ClassFieldNode CFNode;
            int instance_slot = 0;
            /* Count inherited instance fields to find next available slot */
            if (cls->parent_index >= 0)
            {
                for (int _hi = 0; _hi < cls->field_count; _hi++)
                {
                    if (!cls->fields[_hi].is_static && cls->fields[_hi].instance_slot >= 0)
                        instance_slot = cls->fields[_hi].instance_slot + 1;
                }
            }
            for (CFNode *f = s->class_decl.fields; f; f = f->next)
            {
                if (cls->field_count >= CLASS_MAX_FIELDS)
                {
                    compile_error(c, s->line, "Class '%s': too many fields", cls->name);
                    break;
                }
                FieldDef *fd = &cls->fields[cls->field_count++];
                int flen = f->length < FIELD_NAME_MAX - 1 ? f->length : FIELD_NAME_MAX - 1;
                memcpy(fd->name, f->name, flen);
                fd->name[flen] = '\0';
                fd->type_kind = f->type.kind;
                fd->is_nullable = f->type.is_nullable;
                fd->is_static = f->is_static;
                fd->is_final = f->is_final;
                fd->instance_slot = f->is_static ? -1 : instance_slot++;
                if (f->type.kind == TYPE_OBJECT && f->type.class_name)
                {
                    int cnlen = (int)strlen(f->type.class_name);
                    cnlen = cnlen < CLASS_NAME_MAX - 1 ? cnlen : CLASS_NAME_MAX - 1;
                    memcpy(fd->class_name, f->type.class_name, cnlen);
                    fd->class_name[cnlen] = '\0';
                }
            }

            /* Register methods — allocate a chunk for each */
            typedef struct ClassMethodNode CMNode;
            for (CMNode *m = s->class_decl.methods; m; m = m->next)
            {
                if (c->module->count >= MODULE_MAX_FUNCTIONS)
                {
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
                if (m->is_constructor)
                {
                    cls->constructor_index = fn_idx;
                }
                else
                {
                    /* Check if this method overrides an inherited one */
                    int mlen = m->fn->fn_decl.length < FIELD_NAME_MAX - 1
                                   ? m->fn->fn_decl.length
                                   : FIELD_NAME_MAX - 1;
                    int override_slot = -1;
                    for (int mi = 0; mi < cls->method_count; mi++)
                    {
                        if ((int)strlen(cls->methods[mi].name) == mlen &&
                            memcmp(cls->methods[mi].name, m->fn->fn_decl.name, mlen) == 0)
                        {
                            override_slot = mi;
                            break;
                        }
                    }
                    if (override_slot >= 0)
                    {
                        /* Override: replace the inherited entry in-place */
                        cls->methods[override_slot].fn_index = fn_idx;
                        cls->methods[override_slot].is_static = m->is_static;
                        cls->methods[override_slot].return_type_kind =
                            (int)m->fn->fn_decl.return_type.kind;
                        if (m->fn->fn_decl.return_type.class_name)
                        {
                            strncpy(cls->methods[override_slot].return_class_name,
                                    m->fn->fn_decl.return_type.class_name,
                                    CLASS_NAME_MAX - 1);
                        }
                    }
                    else
                    {
                        /* New method: append */
                        if (cls->method_count >= CLASS_MAX_METHODS)
                        {
                            compile_error(c, s->line, "Class '%s': too many methods", cls->name);
                            break;
                        }
                        MethodDef *md = &cls->methods[cls->method_count++];
                        memcpy(md->name, m->fn->fn_decl.name, mlen);
                        md->name[mlen] = '\0';
                        md->fn_index = fn_idx;
                        md->is_static = m->is_static;
                        md->is_virtual = m->is_virtual;
                        md->return_type_kind = (int)m->fn->fn_decl.return_type.kind;
                        md->return_is_nullable = m->fn->fn_decl.return_type.is_nullable;
                        if (m->fn->fn_decl.return_type.class_name)
                        {
                            strncpy(md->return_class_name,
                                    m->fn->fn_decl.return_type.class_name,
                                    CLASS_NAME_MAX - 1);
                        }
                        /* v17: store param signature in MethodDef for stub generation */
                        {
                            int pi = 0;
                            for (ParamNode *p = m->fn->fn_decl.params;
                                 p && pi < METHOD_MAX_PARAMS; p = p->next, pi++)
                            {
                                md->param_type_kinds[pi] = (int)p->type.kind;
                                md->param_is_nullable[pi] = p->type.is_nullable;
                                if (p->type.class_name)
                                {
                                    strncpy(md->param_class_names[pi], p->type.class_name,
                                            METHOD_PARAM_CLASS_MAX - 1);
                                }
                                else
                                {
                                    md->param_class_names[pi][0] = '\0';
                                }
                            }
                            md->param_count = pi;
                        }
                        compile_method_annotations(c, s->line, m->annotations, md);
                    }
                }
            }
        }
        else if (s->kind == STMT_ENUM_DECL)
        {
            /* Register enum as a ClassDef with member name table for toString */
            if (c->module->class_count < MODULE_MAX_CLASSES)
            {
                int ci = c->module->class_count++;
                ClassDef *cls = &c->module->classes[ci];
                memset(cls, 0, sizeof(ClassDef));
                int nlen = s->enum_decl.length < CLASS_NAME_MAX - 1
                               ? s->enum_decl.length
                               : CLASS_NAME_MAX - 1;
                memcpy(cls->name, s->enum_decl.name, nlen);
                cls->name[nlen] = '\0';
                cls->constructor_index = -1;
                cls->parent_index = -1;
                cls->is_enum = true;
                cls->enum_member_count = 0;
                for (struct EnumMemberNode *em = s->enum_decl.members; em; em = em->next)
                {
                    if (cls->enum_member_count >= 64)
                        break;
                    char *copy = malloc(em->length + 1);
                    if (copy)
                    {
                        memcpy(copy, em->name, em->length);
                        copy[em->length] = '\0';
                        int idx = cls->enum_member_count;
                        cls->enum_member_names[idx] = copy;
                        cls->enum_member_values[idx] = em->value;
                        cls->enum_member_count++;
                    }
                }
            }
        }
    }

    /* ── PASS 1.6: Register EventDefs — top-level and class member events ──── */
    for (StmtNode *n = program->stmts; n; n = n->next)
    {
        Stmt *s = n->stmt;
        if (s->kind == STMT_IMPORT)
            continue;

        /* Top-level event declarations */
        if (s->kind == STMT_EVENT_DECL)
        {
            if (c->module->event_count >= MODULE_MAX_EVENTS)
            {
                compile_error(c, s->line, "Too many top-level events");
                continue;
            }
            EventDef *ed = &c->module->events[c->module->event_count++];
            memset(ed, 0, sizeof(EventDef));
            int nlen = s->event_decl.length < FIELD_NAME_MAX - 1
                           ? s->event_decl.length
                           : FIELD_NAME_MAX - 1;
            memcpy(ed->name, s->event_decl.name, nlen);
            ed->name[nlen] = '\0';
            ed->param_count = 0;
            ParamNode *p2 = s->event_decl.params;
            for (; p2 && ed->param_count < EVENT_MAX_PARAMS; p2 = p2->next)
            {
                int pi = ed->param_count++;
                ed->param_type_kinds[pi] = (int)p2->type.kind;
                ed->param_is_nullable[pi] = p2->type.is_nullable;
                if (p2->type.class_name)
                {
                    strncpy(ed->param_class_names[pi], p2->type.class_name,
                            CLASS_NAME_MAX - 1);
                }
            }
            continue;
        }

        /* Class member event declarations */
        if (s->kind == STMT_CLASS_DECL)
        {
            char cls_name_buf[CLASS_NAME_MAX];
            int cls_nlen = s->class_decl.length < CLASS_NAME_MAX - 1
                               ? s->class_decl.length
                               : CLASS_NAME_MAX - 1;
            memcpy(cls_name_buf, s->class_decl.name, cls_nlen);
            cls_name_buf[cls_nlen] = '\0';
            int ci = module_find_class(c->module, cls_name_buf);
            if (ci < 0)
                continue;
            ClassDef *cls = &c->module->classes[ci];

            typedef struct ClassEventNode CENode;
            for (CENode *ev = s->class_decl.events; ev; ev = ev->next)
            {
                if (cls->event_count >= CLASS_MAX_EVENTS)
                    break;
                EventDef *ed = &cls->events[cls->event_count++];
                memset(ed, 0, sizeof(EventDef));
                int nlen = ev->length < FIELD_NAME_MAX - 1
                               ? ev->length
                               : FIELD_NAME_MAX - 1;
                memcpy(ed->name, ev->name, nlen);
                ed->name[nlen] = '\0';
                ed->param_count = 0;
                ParamNode *p2 = ev->params;
                for (; p2 && ed->param_count < EVENT_MAX_PARAMS; p2 = p2->next)
                {
                    int pi = ed->param_count++;
                    ed->param_type_kinds[pi] = (int)p2->type.kind;
                    ed->param_is_nullable[pi] = p2->type.is_nullable;
                    if (p2->type.class_name)
                    {
                        strncpy(ed->param_class_names[pi], p2->type.class_name,
                                CLASS_NAME_MAX - 1);
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

        c->current_chunk = &c->module->chunks[sinit_idx];
        c->current_fn = sinit_idx;
        c->current_class_idx = -1;
        c->current_class_ast = NULL;
        c->local_count = 0;
        c->scope_depth = 0;
        c->next_slot = 0;
        c->loop_depth = 0;
        for (int d = 0; d < MAX_LOOP_DEPTH; d++)
            c->break_count[d] = c->continue_count[d] = 0;

        for (StmtNode *sn = program->stmts; sn; sn = sn->next)
        {
            Stmt *s = sn->stmt;
            if (s->kind != STMT_CLASS_DECL)
                continue;

            char cls_name_buf[CLASS_NAME_MAX];
            int cls_nlen = s->class_decl.length < CLASS_NAME_MAX - 1
                               ? s->class_decl.length
                               : CLASS_NAME_MAX - 1;
            memcpy(cls_name_buf, s->class_decl.name, cls_nlen);
            cls_name_buf[cls_nlen] = '\0';

            int ci = module_find_class(c->module, cls_name_buf);
            if (ci < 0)
                continue;

            c->current_class_idx = ci;
            c->current_class_ast = s;

            typedef struct ClassFieldNode CFNode_sinit;
            int fields_idx = 0;
            for (CFNode_sinit *f = s->class_decl.fields; f; f = f->next, fields_idx++)
            {
                if (!f->is_static || !f->initializer)
                    continue;
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

    for (StmtNode *n = program->stmts; n; n = n->next)
    {
        Stmt *s = n->stmt;

        if (s->kind == STMT_IMPORT)
            continue;

        if (s->kind == STMT_FN_DECL)
        {
            /* Find the chunk by name (handles any declaration order) */
            int fn_idx = module_find(c->module, s->fn_decl.name, s->fn_decl.length);
            if (fn_idx < 0)
            {
                compile_error(c, s->line, "Internal: chunk not found for '%.*s'",
                              s->fn_decl.length, s->fn_decl.name);
                continue;
            }

            c->current_chunk = &c->module->chunks[fn_idx];
            c->current_fn = fn_idx;
            c->current_class_idx = -1;
            c->current_class_ast = NULL;

            c->local_count = 0;
            c->scope_depth = 0;
            c->next_slot = 0;
            c->loop_depth = 0;
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

            if (s->fn_decl.return_type.kind == TYPE_VOID)
            {
                Chunk *ch = c->current_chunk;
                if (ch->count == 0 || ch->code[ch->count - 1] != (uint8_t)OP_RETURN_VOID)
                    emit_op(c, OP_RETURN_VOID, s->line);
            }
        }
        else if (s->kind == STMT_CLASS_DECL)
        {
            /* Find this class's ClassDef — need null-terminated name */
            char cls_name_buf[CLASS_NAME_MAX];
            int cls_nlen = s->class_decl.length < CLASS_NAME_MAX - 1
                               ? s->class_decl.length
                               : CLASS_NAME_MAX - 1;
            memcpy(cls_name_buf, s->class_decl.name, cls_nlen);
            cls_name_buf[cls_nlen] = '\0';

            int ci = module_find_class(c->module, cls_name_buf);
            if (ci < 0)
                continue;
            ClassDef *cls = &c->module->classes[ci];

            c->current_class_idx = ci;
            c->current_class_ast = s;

            typedef struct ClassMethodNode CMNode;
            for (CMNode *m = s->class_decl.methods; m; m = m->next)
            {
                char mangled[128];
                snprintf(mangled, sizeof(mangled), "%s.%.*s",
                         cls->name,
                         m->fn->fn_decl.length, m->fn->fn_decl.name);
                int mfn_idx = module_find(c->module, mangled, (int)strlen(mangled));
                if (mfn_idx < 0)
                {
                    compile_error(c, s->line, "Internal: chunk not found for '%s'", mangled);
                    continue;
                }

                c->current_chunk = &c->module->chunks[mfn_idx];
                c->current_fn = mfn_idx;
                c->local_count = 0;
                c->scope_depth = 0;
                c->next_slot = 0;
                c->loop_depth = 0;
                for (int d = 0; d < MAX_LOOP_DEPTH; d++)
                    c->break_count[d] = c->continue_count[d] = 0;

                /* Slot 0 is 'this' — but only for instance methods and constructors.
                 * Static methods have no 'this'. */
                if (!m->is_static)
                {
                    declare_local(c, "this", 4);
                }

                /* Declare parameters (after 'this' for instance, from 0 for static) */
                for (ParamNode *p = m->fn->fn_decl.params; p; p = p->next)
                    declare_local(c, p->name, p->length);

                /* param_count = explicit params only (not 'this') */
                c->current_chunk->param_count = m->fn->fn_decl.param_count;
                c->current_chunk->is_constructor = m->is_constructor;

                /* ── Field initializers ─────────────────────────────────
                 * For constructors, emit this.field = initializer for every
                 * instance field that has an explicit initializer.
                 * Static field initializers are emitted separately into
                 * the __sinit__ chunk (see below), NOT here — they must
                 * run once at module load, not on every construction. */
                if (m->is_constructor && c->current_class_ast)
                {
                    typedef struct ClassFieldNode CFNode;
                    ClassDef *cur_cls = (c->current_class_idx >= 0)
                                            ? &c->module->classes[c->current_class_idx]
                                            : NULL;
                    int fields_idx = 0; /* index into ClassDef->fields[] */
                    for (CFNode *f = c->current_class_ast->class_decl.fields;
                         f; f = f->next, fields_idx++)
                    {
                        if (!f->initializer)
                            continue;
                        if (f->is_static)
                            continue; /* handled in __sinit__ */
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
                if (ch->count == 0 || ch->code[ch->count - 1] != (uint8_t)OP_RETURN_VOID)
                    emit_op(c, OP_RETURN_VOID, s->line);
            }

            c->current_class_idx = -1;
            c->current_class_ast = NULL;
        }
    }

    return !c->had_error;
}

void compiler_print_errors(const Compiler *c)
{
    for (int i = 0; i < c->error_count; i++)
    {
        printf("[line %d] Compile error: %s\n",
               c->errors[i].line, c->errors[i].message);
    }
}
