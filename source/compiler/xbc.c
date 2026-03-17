/*
 * xbc.c — .xbc file format implementation
 *
 * Writing and reading are symmetric operations. The write path walks
 * the Module and serializes each field in order. The read path allocates
 * and reconstructs the Module in the same order.
 *
 * All writes go through a simple WriteBuffer that grows on demand,
 * then gets flushed to file or returned as a heap buffer. This means
 * both xbc_write (file) and xbc_write_mem (buffer) share the same
 * serialization code.
 */

#include "xbc.h"
#include "bytecode.h"
#include <stdlib.h>
#include <string.h>
#include <stdint.h>

/* ─────────────────────────────────────────────────────────────────────────────
 * WRITE BUFFER
 * A growable byte buffer. We write everything here first, then flush.
 * ───────────────────────────────────────────────────────────────────────────*/

typedef struct {
    uint8_t *data;
    size_t   size;
    size_t   capacity;
} WriteBuf;

static bool wb_init(WriteBuf *wb) {
    wb->data     = malloc(256);
    wb->size     = 0;
    wb->capacity = 256;
    return wb->data != NULL;
}

static void wb_free(WriteBuf *wb) {
    free(wb->data);
    wb->data = NULL;
    wb->size = wb->capacity = 0;
}

static bool wb_grow(WriteBuf *wb, size_t needed) {
    if (wb->size + needed <= wb->capacity) return true;
    size_t new_cap = wb->capacity * 2;
    while (new_cap < wb->size + needed) new_cap *= 2;
    uint8_t *p = realloc(wb->data, new_cap);
    if (!p) return false;
    wb->data     = p;
    wb->capacity = new_cap;
    return true;
}

static bool wb_write(WriteBuf *wb, const void *data, size_t len) {
    if (!wb_grow(wb, len)) return false;
    memcpy(wb->data + wb->size, data, len);
    wb->size += len;
    return true;
}

/* Write individual types — always big-endian */
static bool wb_u8(WriteBuf *wb, uint8_t v) {
    return wb_write(wb, &v, 1);
}

static bool wb_u16(WriteBuf *wb, uint16_t v) {
    uint8_t b[2] = { (v >> 8) & 0xFF, v & 0xFF };
    return wb_write(wb, b, 2);
}

static bool wb_u32(WriteBuf *wb, uint32_t v) {
    uint8_t b[4] = {
        (v >> 24) & 0xFF, (v >> 16) & 0xFF,
        (v >>  8) & 0xFF,  v        & 0xFF
    };
    return wb_write(wb, b, 4);
}

static bool wb_u64(WriteBuf *wb, uint64_t v) {
    uint8_t b[8] = {
        (v >> 56) & 0xFF, (v >> 48) & 0xFF,
        (v >> 40) & 0xFF, (v >> 32) & 0xFF,
        (v >> 24) & 0xFF, (v >> 16) & 0xFF,
        (v >>  8) & 0xFF,  v        & 0xFF
    };
    return wb_write(wb, b, 8);
}

/* Write a string: 4-byte length followed by raw chars (no null terminator) */
static bool wb_str(WriteBuf *wb, const char *s) {
    uint32_t len = s ? (uint32_t)strlen(s) : 0;
    if (!wb_u32(wb, len)) return false;
    if (len > 0) return wb_write(wb, s, len);
    return true;
}


/* ─────────────────────────────────────────────────────────────────────────────
 * READ BUFFER
 * A cursor into a read-only byte buffer.
 * ───────────────────────────────────────────────────────────────────────────*/

typedef struct {
    const uint8_t *data;
    size_t         size;
    size_t         pos;
    bool           error;
} ReadBuf;

static void rb_init(ReadBuf *rb, const uint8_t *data, size_t size) {
    rb->data  = data;
    rb->size  = size;
    rb->pos   = 0;
    rb->error = false;
}

static bool rb_read(ReadBuf *rb, void *out, size_t len) {
    if (rb->error || rb->pos + len > rb->size) {
        rb->error = true;
        return false;
    }
    memcpy(out, rb->data + rb->pos, len);
    rb->pos += len;
    return true;
}

static uint8_t rb_u8(ReadBuf *rb) {
    uint8_t v = 0;
    rb_read(rb, &v, 1);
    return v;
}

static uint16_t rb_u16(ReadBuf *rb) {
    uint8_t b[2] = {0};
    rb_read(rb, b, 2);
    return (uint16_t)((b[0] << 8) | b[1]);
}

static uint32_t rb_u32(ReadBuf *rb) {
    uint8_t b[4] = {0};
    rb_read(rb, b, 4);
    return ((uint32_t)b[0] << 24) | ((uint32_t)b[1] << 16) |
           ((uint32_t)b[2] <<  8) |  (uint32_t)b[3];
}

static uint64_t rb_u64(ReadBuf *rb) {
    uint8_t b[8] = {0};
    rb_read(rb, b, 8);
    uint64_t v = 0;
    for (int i = 0; i < 8; i++) v = (v << 8) | b[i];
    return v;
}

/* Read a length-prefixed string — caller must free() the result */
static char *rb_str(ReadBuf *rb) {
    uint32_t len = rb_u32(rb);
    if (rb->error) return NULL;
    char *s = malloc(len + 1);
    if (!s) { rb->error = true; return NULL; }
    if (len > 0 && !rb_read(rb, s, len)) { free(s); return NULL; }
    s[len] = '\0';
    return s;
}


/* ─────────────────────────────────────────────────────────────────────────────
 * CONSTANT KIND TAGS (in .xbc file)
 * ───────────────────────────────────────────────────────────────────────────*/
#define CONST_INT    0
#define CONST_FLOAT  1
#define CONST_BOOL   2
#define CONST_STR    3


/* ─────────────────────────────────────────────────────────────────────────────
 * SERIALIZATION — Module -> WriteBuf
 * ───────────────────────────────────────────────────────────────────────────*/

/* ── Write one AttributeInstance to the buffer ──────────────────────────── */
static bool wb_attr_arg(WriteBuf *wb, const AttrArg *a) {
    if (!wb_u8(wb, (uint8_t)a->kind)) return false;
    switch (a->kind) {
        case ATTR_ARG_STRING: if (!wb_str(wb, a->s ? a->s : "")) return false; break;
        case ATTR_ARG_INT:    if (!wb_write(wb, &a->i, 8)) return false; break;
        case ATTR_ARG_FLOAT:  if (!wb_write(wb, &a->f, 8)) return false; break;
        case ATTR_ARG_BOOL:   if (!wb_u8(wb, a->b ? 1 : 0)) return false; break;
        case ATTR_ARG_ARRAY:
            if (!wb_u8(wb, (uint8_t)a->arr.count)) return false;
            for (int ei = 0; ei < a->arr.count; ei++)
                if (!wb_attr_arg(wb, &a->arr.elems[ei])) return false;
            break;
    }
    return true;
}

static bool wb_attribute_instances(WriteBuf *wb,
                                    const AttributeInstance *attrs, int count) {
    if (!wb_u8(wb, (uint8_t)count)) return false;
    for (int ai = 0; ai < count; ai++) {
        const AttributeInstance *inst = &attrs[ai];
        if (!wb_str(wb, inst->class_name))        return false;
        if (!wb_u8(wb, (uint8_t)inst->arg_count)) return false;
        for (int ki = 0; ki < inst->arg_count; ki++)
            if (!wb_attr_arg(wb, &inst->args[ki])) return false;
    }
    return true;
}

/*
 * Correct serialization — all in one pass, no double-writes.
 */
static bool serialize_module(WriteBuf *wb, const Module *module) {

    /* ── Header ──────────────────────────────────────────────────────── */
    if (!wb_write(wb, "XBC", 4)) return false;  /* includes null terminator */
    if (!wb_u8(wb, XBC_VERSION))  return false;
    if (!wb_u16(wb, (uint16_t)module->count))       return false;
    if (!wb_u16(wb, (uint16_t)module->class_count)) return false;

    /* ── sinit index ─────────────────────────────────────────────────── */
    if (!wb_u16(wb, (uint16_t)(module->sinit_index < 0 ? 0xFFFF : (uint16_t)module->sinit_index))) return false;

    /* ── uses_stdlib flag ───────────────────────────────────────────── */
    if (!wb_u8(wb, module->uses_stdlib ? 1 : 0)) return false;

    /* ── Class definitions (includes per-class attributes) ──────────── */
    for (int ci = 0; ci < module->class_count; ci++) {
        const ClassDef *cls = &module->classes[ci];

        /* Class name */
        if (!wb_str(wb, cls->name)) return false;

        /* constructor_index and parent_index as int32 (allow -1) */
        if (!wb_u32(wb, (uint32_t)(int32_t)cls->constructor_index)) return false;
        if (!wb_u32(wb, (uint32_t)(int32_t)cls->parent_index))      return false;

        /* Fields */
        if (!wb_u16(wb, (uint16_t)cls->field_count)) return false;
        for (int fi = 0; fi < cls->field_count; fi++) {
            const FieldDef *fd = &cls->fields[fi];
            if (!wb_str(wb, fd->name))                    return false;
            if (!wb_u8(wb, (uint8_t)fd->type_kind))       return false;
            if (!wb_str(wb, fd->class_name))              return false;
            if (!wb_u8(wb, fd->is_static ? 1 : 0))       return false;
            if (!wb_u8(wb, fd->is_final  ? 1 : 0))       return false;
        }

        /* Methods */
        if (!wb_u16(wb, (uint16_t)cls->method_count)) return false;
        for (int mi = 0; mi < cls->method_count; mi++) {
            const MethodDef *md = &cls->methods[mi];
            if (!wb_str(wb, md->name))                       return false;
            if (!wb_u32(wb, (uint32_t)md->fn_index))         return false;
            if (!wb_u8(wb, md->is_static ? 1 : 0))          return false;
            if (!wb_u8(wb, md->is_virtual ? 1 : 0))         return false;
            if (!wb_u8(wb, (uint8_t)md->return_type_kind))  return false;
            if (!wb_str(wb, md->return_class_name))          return false;
            /* v17: param signature */
            if (!wb_u8(wb, (uint8_t)md->param_count))        return false;
            for (int pi = 0; pi < md->param_count && pi < METHOD_MAX_PARAMS; pi++) {
                if (!wb_u8(wb, (uint8_t)md->param_type_kinds[pi])) return false;
                if (!wb_str(wb, md->param_class_names[pi]))         return false;
                if (!wb_u8(wb, md->param_is_nullable[pi] ? 1 : 0)) return false;
            }
            if (!wb_attribute_instances(wb, md->attributes, md->attribute_count)) return false;
        }

        /* Class attributes */
        if (!wb_attribute_instances(wb, cls->attributes, cls->attribute_count)) return false;

        /* Generic type parameters */
        if (!wb_u8(wb, (uint8_t)cls->type_param_count)) return false;
        for (int ti = 0; ti < cls->type_param_count; ti++) {
            if (!wb_str(wb, cls->type_param_names[ti])) return false;
        }

        /* Interface list and is_interface flag (v16+) */
        if (!wb_u8(wb, cls->is_interface ? 1 : 0)) return false;
        if (!wb_u8(wb, (uint8_t)cls->interface_count)) return false;
        for (int ii = 0; ii < cls->interface_count; ii++) {
            if (!wb_str(wb, cls->interface_names[ii])) return false;
        }


        if (!wb_u8(wb, (uint8_t)cls->event_count)) return false;
        for (int ei = 0; ei < cls->event_count; ei++) {
            const EventDef *ed = &cls->events[ei];
            if (!wb_str(wb, ed->name)) return false;
            if (!wb_u8(wb, (uint8_t)ed->param_count)) return false;
            for (int pi = 0; pi < ed->param_count; pi++) {
                if (!wb_u8(wb, (uint8_t)ed->param_type_kinds[pi])) return false;
                if (!wb_str(wb, ed->param_class_names[pi]))         return false;
                if (!wb_u8(wb, ed->param_is_nullable[pi] ? 1 : 0)) return false;
            }
        }
    }

    /* ── Top-level event definitions ─────────────────────────────────── */
    if (!wb_u16(wb, (uint16_t)module->event_count)) return false;
    for (int ei = 0; ei < module->event_count; ei++) {
        const EventDef *ed = &module->events[ei];
        if (!wb_str(wb, ed->name)) return false;
        if (!wb_u8(wb, (uint8_t)ed->param_count)) return false;
        for (int pi = 0; pi < ed->param_count; pi++) {
            if (!wb_u8(wb, (uint8_t)ed->param_type_kinds[pi])) return false;
            if (!wb_str(wb, ed->param_class_names[pi]))         return false;
            if (!wb_u8(wb, ed->param_is_nullable[pi] ? 1 : 0)) return false;
        }
    }

    /* ── Functions ───────────────────────────────────────────────────── */
    for (int fi = 0; fi < module->count; fi++) {
        const Chunk *chunk = &module->chunks[fi];
        const char  *name  =  module->names[fi];

        /* Name */
        uint8_t name_len = (uint8_t)(strlen(name) < 255 ? strlen(name) : 255);
        if (!wb_u8(wb, name_len))           return false;
        if (!wb_write(wb, name, name_len))  return false;

        /* Frame info */
        if (!wb_u8(wb, (uint8_t)chunk->param_count))       return false;
        if (!wb_u8(wb, (uint8_t)chunk->local_count))       return false;
        if (!wb_u8(wb, chunk->is_constructor ? 1 : 0))     return false;

        /* Type signature (return + params) */
        if (!wb_u8(wb, (uint8_t)chunk->return_type_kind))  return false;
        for (int pi = 0; pi < chunk->param_count && pi < 16; pi++)
            if (!wb_u8(wb, (uint8_t)chunk->param_type_kinds[pi])) return false;

        /* Scan bytecode to determine constant type tags.
         * The constant pool itself has no type info — we infer from opcodes. */
        uint8_t *const_kinds = calloc(chunk->constants.count + 1, 1);
        if (!const_kinds) return false;

        for (int pc = 0; pc < chunk->count; ) {
            OpCode op = (OpCode)chunk->code[pc++];

            switch (op) {
                case OP_LOAD_CONST_FLOAT:
                    if (pc+1 < chunk->count) {
                        uint16_t idx = (chunk->code[pc]<<8)|chunk->code[pc+1];
                        if (idx < chunk->constants.count)
                            const_kinds[idx] = CONST_FLOAT;
                    }
                    pc += 2; break;
                case OP_LOAD_CONST_STR:
                    if (pc+1 < chunk->count) {
                        uint16_t idx = (chunk->code[pc]<<8)|chunk->code[pc+1];
                        if (idx < chunk->constants.count)
                            const_kinds[idx] = CONST_STR;
                    }
                    pc += 2; break;

                /* 2-byte operand opcodes */
                case OP_LOAD_CONST_INT:
                case OP_JUMP:
                case OP_JUMP_IF_FALSE:
                    pc += 2; break;

                /* 3-byte operand opcodes (uint16 + uint8) */
                case OP_CALL:
                case OP_CALL_HOST:
                case OP_CALL_SUPER:
                case OP_CALL_METHOD:
                    pc += 3; break;

                case OP_CALL_IFACE:
                    /* uint16 name_const_idx (a CONST_STR) + uint8 argc */
                    if (pc+1 < chunk->count) {
                        uint16_t idx = (chunk->code[pc]<<8)|chunk->code[pc+1];
                        if (idx < chunk->constants.count)
                            const_kinds[idx] = CONST_STR;
                    }
                    pc += 3; break;

                /* OP_NEW: uint16 class_idx + uint8 argc + uint8 tac + tac bytes = variable */
                case OP_NEW: {
                    /* skip class_idx(2) + argc(1) = 3, then read tac to skip type args */
                    pc += 3;
                    uint8_t tac = (pc < chunk->count) ? chunk->code[pc] : 0;
                    pc += 1 + tac;  /* skip tac byte + tac kind bytes */
                    break;
                }

                /* 2-byte operand opcodes (two uint8_t operands = class_idx + field_idx) */
                case OP_LOAD_STATIC:
                case OP_STORE_STATIC:
                    pc += 2; break;  /* opcode already consumed; 2 operand bytes follow */

                /* 1-byte operand opcodes — always exactly 1 operand byte */
                case OP_LOAD_LOCAL:
                case OP_STORE_LOCAL:
                case OP_LOAD_CONST_BOOL:
                case OP_GET_FIELD:
                case OP_SET_FIELD:
                    pc += 1; break;

                /* OP_TO_STR: 1 byte kind; if kind==4 (enum) an extra class_index byte follows */
                case OP_TO_STR: {
                    uint8_t kind = (pc < chunk->count) ? chunk->code[pc] : 0;
                    pc += (kind == 4) ? 2 : 1; break;
                }
                case OP_NEW_ARRAY:
                case OP_ARRAY_LIT:
                case OP_IS_TYPE:
                case OP_AS_TYPE:
                case OP_TYPE_FIELD:
                    pc += 1; break;

                /* OP_TYPEOF: variable length [tag][name_len][name_bytes...] */
                case OP_TYPEOF: {
                    /* [tag][name_len][name_bytes...] */
                    if (pc + 1 < chunk->count) {
                        uint8_t name_len = chunk->code[pc + 1];
                        pc += 2 + name_len;
                    } else { pc += 1; }
                    break;
                }

                /* No operands */
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
                case OP_POP:
                case OP_ADD_INT:
                case OP_ADD_FLOAT:
                case OP_SUB_INT:
                case OP_SUB_FLOAT:
                case OP_MUL_INT:
                case OP_MUL_FLOAT:
                case OP_DIV_INT:
                case OP_DIV_FLOAT:
                case OP_MOD_INT:
                case OP_MOD_FLOAT:
                case OP_NEGATE_INT:
                case OP_NEGATE_FLOAT:
                case OP_NOT_BOOL:
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
                case OP_AND_BOOL:
                case OP_OR_BOOL:
                case OP_CONCAT_STR:
                case OP_LOAD_THIS:
                case OP_PUSH_NULL:
                case OP_IS_NULL:
                case OP_CMP_EQ_VAL:
                case OP_CMP_NEQ_VAL:
                    break;

                /* OP_JUMP_IF_TRUE: 2-byte signed offset */
                case OP_JUMP_IF_TRUE:
                    pc += 2; break;

                /* OP_NULL_COALESCE: 2-byte forward offset */
                case OP_NULL_COALESCE:
                    pc += 2; break;

                /* OP_NULL_ASSERT: 2-byte line number */
                case OP_NULL_ASSERT:
                    pc += 2; break;

                /* OP_TRY_BEGIN: 2-byte catch offset */
                case OP_TRY_BEGIN:
                    pc += 2; break;

                /* OP_TRY_END, OP_THROW, OP_LOAD_EXCEPTION: no operands */
                case OP_TRY_END:
                case OP_THROW:
                case OP_LOAD_EXCEPTION:
                    break;

                /* OP_EXCEPTION_IS_TYPE: [u8 name_len][name_bytes] */
                case OP_EXCEPTION_IS_TYPE: {
                    uint8_t nlen = (pc < chunk->count) ? chunk->code[pc] : 0;
                    pc += 1 + nlen;
                    break;
                }

                /* OP_EVENT_SUBSCRIBE / OP_EVENT_UNSUBSCRIBE: [u8 nlen][name][u16 fn_idx] */
                case OP_EVENT_SUBSCRIBE:
                case OP_EVENT_UNSUBSCRIBE: {
                    uint8_t nlen = (pc < chunk->count) ? chunk->code[pc] : 0;
                    pc += 1 + nlen + 2; /* name + u16 fn_idx */
                    break;
                }

                /* OP_EVENT_FIRE: [u8 nlen][name][u8 argc] */
                case OP_EVENT_FIRE: {
                    uint8_t nlen = (pc < chunk->count) ? chunk->code[pc] : 0;
                    pc += 1 + nlen + 1; /* name + u8 argc */
                    break;
                }

                /* OP_CALL_SUPER: 2-byte fn_idx + 1-byte argc */
                /* Already listed above but double-check: */

                /* OP_GET_FIELD: 1-byte field_idx (already listed, verify) */
                /* OP_SET_FIELD: 1-byte field_idx (already listed) */

                default: break;
            }
        }

        /* Constant pool */
        if (!wb_u16(wb, (uint16_t)chunk->constants.count)) {
            free(const_kinds);
            return false;
        }

        for (int i = 0; i < chunk->constants.count; i++) {
            Value    v    = chunk->constants.values[i];
            uint8_t  kind = const_kinds[i];
            if (!wb_u8(wb, kind)) { free(const_kinds); return false; }

            if (kind == CONST_STR) {
                if (!wb_str(wb, v.s)) { free(const_kinds); return false; }
            } else {
                /* Serialize the payload (union field) as raw 8 bytes.
                 * We extract the union portion only, skipping is_null. */
                uint64_t raw = 0;
                memcpy(&raw, &v.i, 8);   /* .i overlaps all union members */
                if (!wb_u64(wb, raw)) { free(const_kinds); return false; }
            }
        }

        free(const_kinds);

        /* Bytecode */
        if (!wb_u32(wb, (uint32_t)chunk->count)) return false;
        if (!wb_write(wb, chunk->code, chunk->count)) return false;
    }

    return true;
}


/* ─────────────────────────────────────────────────────────────────────────────
 * DESERIALIZATION — ReadBuf -> Module
 * ───────────────────────────────────────────────────────────────────────────*/

/* ── Read one AttrArg from the buffer ───────────────────────────────────── */
static XbcResult rb_attr_arg(ReadBuf *rb, AttrArg *a) {
    memset(a, 0, sizeof(AttrArg));
    a->kind = (AttrArgKind)rb_u8(rb);
    if (rb->error) return XBC_ERR_IO;
    switch (a->kind) {
        case ATTR_ARG_STRING: {
            char *sv = rb_str(rb);
            if (rb->error || !sv) return XBC_ERR_OOM;
            a->s = sv; break;
        }
        case ATTR_ARG_INT:   if (!rb_read(rb, &a->i, 8)) return XBC_ERR_IO; break;
        case ATTR_ARG_FLOAT: if (!rb_read(rb, &a->f, 8)) return XBC_ERR_IO; break;
        case ATTR_ARG_BOOL:  a->b = rb_u8(rb) != 0; if (rb->error) return XBC_ERR_IO; break;
        case ATTR_ARG_ARRAY: {
            uint8_t ecnt = rb_u8(rb);
            if (rb->error) return XBC_ERR_IO;
            a->arr.count = (int)ecnt;
            if (ecnt == 0) { a->arr.elems = NULL; break; }
            a->arr.elems = calloc(ecnt, sizeof(AttrArg));
            if (!a->arr.elems) return XBC_ERR_OOM;
            for (int ei = 0; ei < (int)ecnt; ei++) {
                XbcResult er = rb_attr_arg(rb, &a->arr.elems[ei]);
                if (er != XBC_OK) return er;
            }
            break;
        }
        default: break;
    }
    return XBC_OK;
}

static XbcResult rb_attribute_instances(ReadBuf *rb,
                                         AttributeInstance *attrs, int *count) {
    uint8_t attr_count = rb_u8(rb);
    if (rb->error) return XBC_ERR_IO;
    if (attr_count > CLASS_MAX_ATTRIBUTES) return XBC_ERR_CORRUPT;
    *count = (int)attr_count;
    for (int ai = 0; ai < *count; ai++) {
        AttributeInstance *inst = &attrs[ai];
        memset(inst, 0, sizeof(AttributeInstance));
        char *aname = rb_str(rb);
        if (rb->error || !aname) return XBC_ERR_OOM;
        strncpy(inst->class_name, aname, CLASS_NAME_MAX - 1);
        inst->class_name[CLASS_NAME_MAX - 1] = '\0';
        free(aname);
        uint8_t arg_count = rb_u8(rb);
        if (rb->error) return XBC_ERR_IO;
        if (arg_count > ATTR_MAX_ARGS) return XBC_ERR_CORRUPT;
        inst->arg_count = (int)arg_count;
        for (int ki = 0; ki < inst->arg_count; ki++) {
            XbcResult er = rb_attr_arg(rb, &inst->args[ki]);
            if (er != XBC_OK) return er;
        }
    }
    return XBC_OK;
}

static XbcResult deserialize_module(Module *module, ReadBuf *rb) {
    module_init(module);

    /* ── Header ──────────────────────────────────────────────────────── */
    char magic[4];
    if (!rb_read(rb, magic, 4)) return XBC_ERR_IO;
    if (memcmp(magic, "XBC", 4) != 0) return XBC_ERR_BAD_MAGIC;

    uint8_t version = rb_u8(rb);
    if (rb->error) return XBC_ERR_IO;
    if (version != XBC_VERSION) return XBC_ERR_BAD_VERSION;

    uint16_t fn_count    = rb_u16(rb);
    uint16_t class_count = rb_u16(rb);
    if (rb->error) return XBC_ERR_IO;

    /* ── sinit index ─────────────────────────────────────────────────── */
    uint16_t sinit_raw = rb_u16(rb);
    if (rb->error) return XBC_ERR_IO;
    module->sinit_index = (sinit_raw == 0xFFFF) ? -1 : (int)sinit_raw;

    /* ── uses_stdlib flag ───────────────────────────────────────────── */
    uint8_t uses_stdlib = rb_u8(rb);
    if (rb->error) return XBC_ERR_IO;
    module->uses_stdlib = uses_stdlib != 0;

    /* ── Class definitions (includes per-class attributes) ──────────── */
    for (int ci = 0; ci < class_count; ci++) {
        if (module->class_count >= MODULE_MAX_CLASSES) return XBC_ERR_CORRUPT;

        ClassDef *cls = &module->classes[module->class_count++];
        memset(cls, 0, sizeof(ClassDef));

        /* Class name */
        char *cname = rb_str(rb);
        if (rb->error || !cname) return XBC_ERR_OOM;
        strncpy(cls->name, cname, CLASS_NAME_MAX - 1);
        cls->name[CLASS_NAME_MAX - 1] = '\0';
        free(cname);

        /* constructor_index and parent_index */
        cls->constructor_index = (int)(int32_t)rb_u32(rb);
        cls->parent_index      = (int)(int32_t)rb_u32(rb);
        if (rb->error) return XBC_ERR_IO;

        /* Fields */
        uint16_t field_count = rb_u16(rb);
        if (rb->error) return XBC_ERR_IO;
        if (field_count > CLASS_MAX_FIELDS) return XBC_ERR_CORRUPT;
        cls->field_count = (int)field_count;

        for (int fi = 0; fi < cls->field_count; fi++) {
            FieldDef *fd = &cls->fields[fi];

            char *fname = rb_str(rb);
            if (rb->error || !fname) return XBC_ERR_OOM;
            strncpy(fd->name, fname, FIELD_NAME_MAX - 1);
            fd->name[FIELD_NAME_MAX - 1] = '\0';
            free(fname);

            fd->type_kind = (int)rb_u8(rb);

            char *class_name = rb_str(rb);
            if (rb->error || !class_name) return XBC_ERR_OOM;
            strncpy(fd->class_name, class_name, CLASS_NAME_MAX - 1);
            fd->class_name[CLASS_NAME_MAX - 1] = '\0';
            free(class_name);

            fd->is_static = rb_u8(rb) != 0;
            fd->is_final  = rb_u8(rb) != 0;
            if (rb->error) return XBC_ERR_IO;
        }

        /* Methods */
        uint16_t method_count = rb_u16(rb);
        if (rb->error) return XBC_ERR_IO;
        if (method_count > CLASS_MAX_METHODS) return XBC_ERR_CORRUPT;
        cls->method_count = (int)method_count;

        for (int mi = 0; mi < cls->method_count; mi++) {
            MethodDef *md = &cls->methods[mi];

            char *mname = rb_str(rb);
            if (rb->error || !mname) return XBC_ERR_OOM;
            strncpy(md->name, mname, FIELD_NAME_MAX - 1);
            md->name[FIELD_NAME_MAX - 1] = '\0';
            free(mname);

            md->fn_index        = (int)rb_u32(rb);
            md->is_static       = rb_u8(rb) != 0;
            md->is_virtual      = rb_u8(rb) != 0;
            md->return_type_kind = (int)rb_u8(rb);
            char *rcname = rb_str(rb);
            if (rb->error || !rcname) return XBC_ERR_OOM;
            strncpy(md->return_class_name, rcname, CLASS_NAME_MAX - 1);
            md->return_class_name[CLASS_NAME_MAX - 1] = '\0';
            free(rcname);
            if (rb->error) return XBC_ERR_IO;
            /* v17: param signature */
            md->param_count = (int)rb_u8(rb);
            if (rb->error) return XBC_ERR_IO;
            if (md->param_count > METHOD_MAX_PARAMS) md->param_count = METHOD_MAX_PARAMS;
            for (int pi = 0; pi < md->param_count; pi++) {
                md->param_type_kinds[pi] = (int)rb_u8(rb);
                if (rb->error) return XBC_ERR_IO;
                char *pcname = rb_str(rb);
                if (rb->error || !pcname) return XBC_ERR_OOM;
                strncpy(md->param_class_names[pi], pcname, METHOD_PARAM_CLASS_MAX - 1);
                md->param_class_names[pi][METHOD_PARAM_CLASS_MAX - 1] = '\0';
                free(pcname);
                md->param_is_nullable[pi] = rb_u8(rb) != 0;
                if (rb->error) return XBC_ERR_IO;
            }
            /* Read method attribute count and heap-allocate if needed */
            {
                uint8_t mac = rb_u8(rb);
                if (rb->error) return XBC_ERR_IO;
                md->attribute_count = (int)mac;
                if (mac > 0) {
                    md->attributes = calloc(mac, sizeof(AttributeInstance));
                    if (!md->attributes) return XBC_ERR_OOM;
                    /* Re-read using a temporary count var since we already read the byte */
                    int tmp = 0;
                    ReadBuf rb2 = *rb; /* snapshot — but we already consumed the count */
                    /* Actually read each instance manually */
                    for (int ai = 0; ai < (int)mac; ai++) {
                        AttributeInstance *inst = &md->attributes[ai];
                        memset(inst, 0, sizeof(AttributeInstance));
                        char *aname = rb_str(rb);
                        if (rb->error || !aname) return XBC_ERR_OOM;
                        strncpy(inst->class_name, aname, CLASS_NAME_MAX - 1);
                        inst->class_name[CLASS_NAME_MAX - 1] = '\0';
                        free(aname);
                        uint8_t argc = rb_u8(rb);
                        if (rb->error) return XBC_ERR_IO;
                        inst->arg_count = (int)argc;
                        for (int ki = 0; ki < inst->arg_count; ki++) {
                            XbcResult er = rb_attr_arg(rb, &inst->args[ki]);
                            if (er != XBC_OK) return er;
                        }
                    }
                    (void)tmp; (void)rb2;
                } else {
                    md->attributes = NULL;
                }
            }
        }

        /* Class attributes */
        XbcResult car = rb_attribute_instances(rb, cls->attributes, &cls->attribute_count);
        if (car != XBC_OK) return car;

        /* Generic type parameters */
        cls->type_param_count = (int)rb_u8(rb);
        if (rb->error) return XBC_ERR_IO;
        if (cls->type_param_count > 8) cls->type_param_count = 8;
        for (int ti = 0; ti < cls->type_param_count; ti++) {
            char *tname = rb_str(rb);
            if (rb->error || !tname) { cls->type_param_count = ti; break; }
            int tl = (int)strlen(tname);
            if (tl > 7) tl = 7;
            memcpy(cls->type_param_names[ti], tname, tl);
            cls->type_param_names[ti][tl] = '\0';
            free(tname);
        }

        /* Interface list and is_interface flag (v16+) */
        cls->is_interface = (rb_u8(rb) != 0);
        if (rb->error) return XBC_ERR_IO;
        cls->interface_count = (int)rb_u8(rb);
        if (rb->error) return XBC_ERR_IO;
        if (cls->interface_count > 8) cls->interface_count = 8;
        for (int ii = 0; ii < cls->interface_count; ii++) {
            char *iname = rb_str(rb);
            if (rb->error || !iname) { cls->interface_count = ii; break; }
            int il = (int)strlen(iname);
            if (il > 63) il = 63;
            memcpy(cls->interface_names[ii], iname, il);
            cls->interface_names[ii][il] = '\0';
            free(iname);
        }

        /* Class event definitions */
        uint8_t ev_count = rb_u8(rb);
        if (rb->error) return XBC_ERR_IO;
        if (ev_count > CLASS_MAX_EVENTS) ev_count = CLASS_MAX_EVENTS;
        cls->event_count = (int)ev_count;
        for (int ei = 0; ei < (int)ev_count; ei++) {
            EventDef *ed = &cls->events[ei];
            memset(ed, 0, sizeof(EventDef));
            char *ename = rb_str(rb);
            if (rb->error || !ename) return XBC_ERR_OOM;
            strncpy(ed->name, ename, FIELD_NAME_MAX - 1); free(ename);
            ed->param_count = (int)rb_u8(rb);
            if (rb->error) return XBC_ERR_IO;
            for (int pi = 0; pi < ed->param_count && pi < EVENT_MAX_PARAMS; pi++) {
                ed->param_type_kinds[pi]  = (int)rb_u8(rb);
                char *cn = rb_str(rb);
                if (rb->error || !cn) return XBC_ERR_OOM;
                strncpy(ed->param_class_names[pi], cn, EVENT_CLASS_NAME_MAX - 1); free(cn);
                ed->param_is_nullable[pi] = rb_u8(rb) != 0;
                if (rb->error) return XBC_ERR_IO;
            }
        }
    }

    /* ── Top-level event definitions ─────────────────────────────────── */
    {
        uint16_t tev_count = rb_u16(rb);
        if (rb->error) return XBC_ERR_IO;
        if (tev_count > MODULE_MAX_EVENTS) tev_count = MODULE_MAX_EVENTS;
        module->event_count = (int)tev_count;
        for (int ei = 0; ei < (int)tev_count; ei++) {
            EventDef *ed = &module->events[ei];
            memset(ed, 0, sizeof(EventDef));
            char *ename = rb_str(rb);
            if (rb->error || !ename) return XBC_ERR_OOM;
            strncpy(ed->name, ename, FIELD_NAME_MAX - 1); free(ename);
            ed->param_count = (int)rb_u8(rb);
            if (rb->error) return XBC_ERR_IO;
            for (int pi = 0; pi < ed->param_count && pi < EVENT_MAX_PARAMS; pi++) {
                ed->param_type_kinds[pi]  = (int)rb_u8(rb);
                char *cn = rb_str(rb);
                if (rb->error || !cn) return XBC_ERR_OOM;
                strncpy(ed->param_class_names[pi], cn, EVENT_CLASS_NAME_MAX - 1); free(cn);
                ed->param_is_nullable[pi] = rb_u8(rb) != 0;
                if (rb->error) return XBC_ERR_IO;
            }
        }
    }

    /* ── Functions ───────────────────────────────────────────────────── */
    for (int fi = 0; fi < fn_count; fi++) {
        if (module->count >= MODULE_MAX_FUNCTIONS) return XBC_ERR_CORRUPT;

        int fn_idx = module_add_chunk(module);
        Chunk *chunk = &module->chunks[fn_idx];

        /* Name */
        uint8_t name_len = rb_u8(rb);
        if (rb->error) return XBC_ERR_IO;
        if (name_len >= 64) return XBC_ERR_CORRUPT;

        if (!rb_read(rb, module->names[fn_idx], name_len)) return XBC_ERR_IO;
        module->names[fn_idx][name_len] = '\0';

        /* Frame info */
        chunk->param_count    = rb_u8(rb);
        chunk->local_count    = rb_u8(rb);
        chunk->is_constructor = rb_u8(rb) != 0;

        /* Type signature (return + params) */
        chunk->return_type_kind = rb_u8(rb);
        for (int pi = 0; pi < chunk->param_count && pi < 16; pi++)
            chunk->param_type_kinds[pi] = rb_u8(rb);
        if (rb->error) return XBC_ERR_IO;

        /* Constant pool */
        uint16_t const_count = rb_u16(rb);
        if (rb->error) return XBC_ERR_IO;

        for (int i = 0; i < const_count; i++) {
            uint8_t kind = rb_u8(rb);
            if (rb->error) return XBC_ERR_IO;
    
            Value v; memset(&v, 0, sizeof(Value));
            if (kind == CONST_STR) {
                char *s = rb_str(rb);
                if (rb->error || !s) return XBC_ERR_OOM;
                v.is_null = 0; v.s = s;
            } else {
                uint64_t raw = rb_u64(rb);
                if (rb->error) return XBC_ERR_IO;
                memcpy(&v.i, &raw, 8);   /* restore union, leave is_null=0 */
            }

            chunk_add_constant(chunk, v);
        }

        /* Bytecode */
        uint32_t code_len = rb_u32(rb);
        if (rb->error) return XBC_ERR_IO;
        if (code_len > 1024 * 1024) return XBC_ERR_CORRUPT;

        for (uint32_t i = 0; i < code_len; i++) {
            uint8_t byte = rb_u8(rb);
            if (rb->error) return XBC_ERR_IO;
            chunk_write(chunk, byte, 0);
        }
    }

    return rb->error ? XBC_ERR_IO : XBC_OK;
}


/* ─────────────────────────────────────────────────────────────────────────────
 * PUBLIC INTERFACE
 * ───────────────────────────────────────────────────────────────────────────*/

const char *xbc_result_str(XbcResult r) {
    switch (r) {
        case XBC_OK:              return "OK";
        case XBC_ERR_IO:          return "I/O error";
        case XBC_ERR_BAD_MAGIC:   return "Not a valid .xbc file";
        case XBC_ERR_BAD_VERSION: return "Unsupported .xbc version";
        case XBC_ERR_CORRUPT:     return "File appears corrupt";
        case XBC_ERR_OOM:         return "Out of memory";
        default:                  return "Unknown error";
    }
}

XbcResult xbc_write_mem(const Module *module, uint8_t **buf, size_t *size) {
    WriteBuf wb;
    if (!wb_init(&wb)) return XBC_ERR_OOM;

    if (!serialize_module(&wb, module)) {
        wb_free(&wb);
        return XBC_ERR_IO;
    }

    *buf  = wb.data;   /* Caller owns this memory */
    *size = wb.size;
    /* Don't call wb_free — caller owns the buffer now */
    return XBC_OK;
}

XbcResult xbc_read_mem(Module *module, const uint8_t *buf, size_t size) {
    ReadBuf rb;
    rb_init(&rb, buf, size);
    return deserialize_module(module, &rb);
}

XbcResult xbc_write(const Module *module, const char *path) {
    uint8_t *buf  = NULL;
    size_t   size = 0;

    XbcResult r = xbc_write_mem(module, &buf, &size);
    if (r != XBC_OK) return r;

    FILE *f = fopen(path, "wb");
    if (!f) { free(buf); return XBC_ERR_IO; }

    size_t written = fwrite(buf, 1, size, f);
    fclose(f);
    free(buf);

    return written == size ? XBC_OK : XBC_ERR_IO;
}

XbcResult xbc_read(Module *module, const char *path) {
    FILE *f = fopen(path, "rb");
    if (!f) return XBC_ERR_IO;

    fseek(f, 0, SEEK_END);
    long file_size = ftell(f);
    fseek(f, 0, SEEK_SET);

    if (file_size <= 0 || file_size > 16 * 1024 * 1024) {
        fclose(f);
        return XBC_ERR_CORRUPT;
    }

    uint8_t *buf = malloc((size_t)file_size);
    if (!buf) { fclose(f); return XBC_ERR_OOM; }

    size_t read = fread(buf, 1, (size_t)file_size, f);
    fclose(f);

    if (read != (size_t)file_size) { free(buf); return XBC_ERR_IO; }

    XbcResult r = xbc_read_mem(module, buf, (size_t)file_size);
    free(buf);
    return r;
}