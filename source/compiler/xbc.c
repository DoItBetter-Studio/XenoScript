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
    /* Reject before allocating — a forged 4GB length must not malloc. */
    if ((size_t)len > rb->size - rb->pos) {
        rb->error = true;
        return NULL;
    }
    char *s = malloc((size_t)len + 1);
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
            if (!wb_str(wb, fd->name))                    	return false;
            if (!wb_u8(wb, (uint8_t)fd->type_kind))       	return false;
            if (!wb_str(wb, fd->class_name))              	return false;
            if (!wb_u8(wb, fd->is_static ? 1 : 0))       	return false;
            if (!wb_u8(wb, fd->is_final  ? 1 : 0))       	return false;
            if (!wb_u8(wb, fd->is_nullable ? 1 : 0))     	return false;
			if (!wb_u8(wb, fd->access_flags))				return false;
        }

        /* Enum reflection metadata */
        if (!wb_u8(wb, cls->is_enum ? 1 : 0)) return false;
        if (!wb_u8(wb, (uint8_t)cls->enum_member_count)) return false;
        for (int ei = 0; ei < cls->enum_member_count; ei++) {
            if (!wb_str(wb, cls->enum_member_names[ei] ?
                                cls->enum_member_names[ei] : "")) return false;
            if (!wb_u32(wb, (uint32_t)cls->enum_member_values[ei])) return false;
        }
        /* Methods */
        if (!wb_u16(wb, (uint16_t)cls->method_count)) return false;
        for (int mi = 0; mi < cls->method_count; mi++) {
            const MethodDef *md = &cls->methods[mi];
            if (!wb_str(wb, md->name))                       return false;
            if (!wb_u32(wb, (uint32_t)md->fn_index))         return false;
            if (!wb_u8(wb, md->is_static ? 1 : 0))          return false;
            if (!wb_u8(wb, md->is_virtual ? 1 : 0))         return false;
			if (!wb_u8(wb, md->access_flags)) 				return false;
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
			if (!wb_u8(wb, ed->access_flags)) return false;
            if (!wb_u8(wb, (uint8_t)ed->param_count)) return false;
            for (int pi = 0; pi < ed->param_count; pi++) {
                if (!wb_u8(wb, (uint8_t)ed->param_type_kinds[pi])) return false;
                if (!wb_str(wb, ed->param_class_names[pi]))         return false;
                if (!wb_u8(wb, ed->param_is_nullable[pi] ? 1 : 0)) return false;
            }
            if (!wb_attribute_instances(wb, ed->attributes,
                                        ed->attribute_count)) return false;
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

        /* Constant kinds come from ConstPool.is_str (set at emit time).
         * Never infer from bytecode — the opcode walk desyncs on OP_NEW /
         * type-args and mis-tags string constants as raw ints, which then
         * load as null (field init: string ModId = "mymod"). */
        if (!wb_u16(wb, (uint16_t)chunk->constants.count))
            return false;

        for (int i = 0; i < chunk->constants.count; i++) {
            Value v = chunk->constants.values[i];
            uint8_t kind = CONST_INT; /* default numeric/raw */
            if (chunk->constants.is_str && chunk->constants.is_str[i])
                kind = CONST_STR;
            if (!wb_u8(wb, kind))
                return false;

            if (kind == CONST_STR) {
                if (!wb_str(wb, v.s ? v.s : ""))
                    return false;
            } else {
                uint64_t raw = 0;
                memcpy(&raw, &v.i, 8);
                if (!wb_u64(wb, raw))
                    return false;
            }
        }

        /* Bytecode */
        if (!wb_u32(wb, (uint32_t)chunk->count)) return false;
        if (!wb_write(wb, chunk->code, chunk->count)) return false;

        /* Source line table — RLE (run-length encoded).
         * Consecutive identical lines collapse to (length, line) pairs.
         * Typical scripts compress ~10-50x vs one uint32 per byte. */
        {
            int n = chunk->count;
            /* Count runs */
            uint32_t runs = 0;
            if (n > 0) {
                runs = 1;
                int prev = (chunk->lines) ? chunk->lines[0] : 0;
                if (prev < 0) prev = 0;
                for (int li = 1; li < n; li++) {
                    int line = (chunk->lines) ? chunk->lines[li] : 0;
                    if (line < 0) line = 0;
                    if (line != prev) {
                        runs++;
                        prev = line;
                    }
                }
            }
            if (!wb_u32(wb, runs)) return false;
            if (n > 0) {
                int run_start = 0;
                int prev = (chunk->lines) ? chunk->lines[0] : 0;
                if (prev < 0) prev = 0;
                for (int li = 1; li <= n; li++) {
                    int line = (li < n && chunk->lines) ? chunk->lines[li] : -1;
                    if (li < n && line < 0) line = 0;
                    if (li == n || line != prev) {
                        uint32_t len = (uint32_t)(li - run_start);
                        if (!wb_u32(wb, len)) return false;
                        if (!wb_u32(wb, (uint32_t)prev)) return false;
                        if (li < n) {
                            run_start = li;
                            prev = line;
                        }
                    }
                }
            }
        }

        /* Source file basename (v24+) */
        if (!wb_str(wb, chunk->source_file[0] ? chunk->source_file : ""))
            return false;
    }

    return true;
}


/* ─────────────────────────────────────────────────────────────────────────────
 * DESERIALIZATION — ReadBuf -> Module
 * ───────────────────────────────────────────────────────────────────────────*/

/* ── Read one AttrArg from the buffer (depth-limited for nested arrays) ── */
#define ATTR_ARG_MAX_DEPTH 8
static XbcResult rb_attr_arg_depth(ReadBuf *rb, AttrArg *a, int depth) {
    if (depth > ATTR_ARG_MAX_DEPTH) return XBC_ERR_CORRUPT;
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
            if (ecnt > ATTR_MAX_ARGS) return XBC_ERR_CORRUPT;
            a->arr.count = (int)ecnt;
            if (ecnt == 0) { a->arr.elems = NULL; break; }
            a->arr.elems = calloc(ecnt, sizeof(AttrArg));
            if (!a->arr.elems) return XBC_ERR_OOM;
            for (int ei = 0; ei < (int)ecnt; ei++) {
                XbcResult er = rb_attr_arg_depth(rb, &a->arr.elems[ei], depth + 1);
                if (er != XBC_OK) return er;
            }
            break;
        }
        default: return XBC_ERR_CORRUPT;
    }
    return XBC_OK;
}
static XbcResult rb_attr_arg(ReadBuf *rb, AttrArg *a) {
    return rb_attr_arg_depth(rb, a, 0);
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

static XbcResult rb_attribute_instances_heap(ReadBuf *rb,
                                             AttributeInstance **attrs,
                                             int *count) {
    uint8_t attr_count = rb_u8(rb);
    if (rb->error) return XBC_ERR_IO;
    if (attr_count > CLASS_MAX_ATTRIBUTES) return XBC_ERR_CORRUPT;
    *count = 0;
    *attrs = NULL;
    if (attr_count == 0) return XBC_OK;

    *attrs = calloc(attr_count, sizeof(AttributeInstance));
    if (!*attrs) return XBC_ERR_OOM;
    for (int ai = 0; ai < attr_count; ai++) {
        AttributeInstance *inst = &(*attrs)[ai];
        char *aname = rb_str(rb);
        if (rb->error || !aname) return XBC_ERR_OOM;
        strncpy(inst->class_name, aname, CLASS_NAME_MAX - 1);
        inst->class_name[CLASS_NAME_MAX - 1] = '\0';
        free(aname);
        *count = ai + 1;

        uint8_t arg_count = rb_u8(rb);
        if (rb->error) return XBC_ERR_IO;
        if (arg_count > ATTR_MAX_ARGS) return XBC_ERR_CORRUPT;
        for (int ki = 0; ki < arg_count; ki++) {
            XbcResult er = rb_attr_arg(rb, &inst->args[ki]);
            if (er != XBC_OK) return er;
            inst->arg_count = ki + 1;
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
    /* Exact version only — no silent partial support for older layouts. */
    if (version != XBC_VERSION)
        return XBC_ERR_BAD_VERSION;
    (void)version;

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
            if (version >= 20)
                fd->is_nullable = rb_u8(rb) != 0;
			fd->access_flags = rb_u8(rb);
            if (rb->error) return XBC_ERR_IO;
            /* instance_slot is not stored in XBC (opcodes embed the index).
             * Recompute it after the field list is complete so the debugger
             * (and any runtime reflection) can map FieldDef → obj->fields[]. */
            fd->instance_slot = -1;
        }

        /* Rebuild dense instance_slot numbers (statics stay -1). */
        {
            int next_slot = 0;
            for (int fi = 0; fi < cls->field_count; fi++)
            {
                if (cls->fields[fi].is_static)
                    cls->fields[fi].instance_slot = -1;
                else
                    cls->fields[fi].instance_slot = next_slot++;
            }
        }

        /* Enum reflection metadata (added in XBC v20) */
        if (version >= 20) {
            cls->is_enum = rb_u8(rb) != 0;
            uint8_t enum_member_count = rb_u8(rb);
            if (rb->error) return XBC_ERR_IO;
            if (enum_member_count > 64) return XBC_ERR_CORRUPT;
            cls->enum_member_count = enum_member_count;
            for (int ei = 0; ei < cls->enum_member_count; ei++) {
                char *member_name = rb_str(rb);
                if (rb->error || !member_name) return XBC_ERR_OOM;
                cls->enum_member_names[ei] = member_name;
                cls->enum_member_values[ei] = (int32_t)rb_u32(rb);
                if (rb->error) return XBC_ERR_IO;
            }
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
			md->access_flags 	= rb_u8(rb);
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
                        if (argc > ATTR_MAX_ARGS) return XBC_ERR_CORRUPT;
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
			ed->access_flags = rb_u8(rb);
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
            if (version >= 21) {
                XbcResult ear = rb_attribute_instances_heap(
                    rb, &ed->attributes, &ed->attribute_count);
                if (ear != XBC_OK) return ear;
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
        if (fn_idx < 0)
            return XBC_ERR_OOM;
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
    
            if (kind == CONST_STR) {
                char *s = rb_str(rb);
                if (rb->error || !s) return XBC_ERR_OOM;
                chunk_add_constant_str(chunk, s);
            } else if (kind == CONST_FLOAT) {
                int64_t raw = (int64_t)rb_u64(rb);
                if (rb->error) return XBC_ERR_IO;
                double d;
                memcpy(&d, &raw, sizeof(d));
                chunk_add_constant(chunk, val_float(d));
            } else {
                int64_t raw = (int64_t)rb_u64(rb);
                if (rb->error) return XBC_ERR_IO;
                chunk_add_constant(chunk, val_int((XenoInt)raw));
            }
        }

        /* Bytecode */
        uint32_t code_len = rb_u32(rb);
        if (rb->error) return XBC_ERR_IO;
        if (code_len > 1024 * 1024) return XBC_ERR_CORRUPT;

        for (uint32_t i = 0; i < code_len; i++) {
            uint8_t byte = rb_u8(rb);
            if (rb->error) return XBC_ERR_IO;
            if (chunk_write(chunk, byte, 0) < 0)
                return XBC_ERR_OOM;
        }
        if ((uint32_t)chunk->count != code_len)
            return XBC_ERR_OOM;

        /* Source line table — RLE expand into chunk->lines[] */
        {
            uint32_t runs = rb_u32(rb);
            if (rb->error) return XBC_ERR_IO;
            uint32_t filled = 0;
            for (uint32_t r = 0; r < runs; r++) {
                uint32_t len  = rb_u32(rb);
                uint32_t line = rb_u32(rb);
                if (rb->error) return XBC_ERR_IO;
                /* Avoid uint32 wrap: len > remaining is the safe form. */
                if (len > code_len - filled) return XBC_ERR_CORRUPT;
                if (chunk->lines) {
                    for (uint32_t k = 0; k < len; k++)
                        chunk->lines[filled + k] = (int)line;
                }
                filled += len;
            }
            if (filled != code_len) return XBC_ERR_CORRUPT;
        }

        /* Source file basename (v24+) */
        {
            char *sf = rb_str(rb);
            if (rb->error) return XBC_ERR_IO;
            if (sf) {
                snprintf(chunk->source_file, sizeof(chunk->source_file), "%s", sf);
                free(sf);
            } else {
                chunk->source_file[0] = '\0';
            }
        }
    }

    return rb->error ? XBC_ERR_IO : XBC_OK;
}


/* ─────────────────────────────────────────────────────────────────────────────
 * PUBLIC INTERFACE
 * ───────────────────────────────────────────────────────────────────────────*/

/* ─────────────────────────────────────────────────────────────────────────────
 * BYTECODE VERIFIER
 *
 * Phase 1 (PRE_MERGE): structural + instruction boundaries after deserialize.
 * Phase 2 (POST_MERGE): strict name resolution, arity, no hollow methods.
 * Phase 3 (both modes): fall-off-the-end, constant kinds, stack-depth sim.
 *
 * Goal: reject hostile / hand-edited .xbc that would cause out-of-bounds
 * reads, bad jumps, stack corruption, or invalid table indices.
 *
 * What we check:
 *   1. Module table limits and cross-references (class/method/fn indices)
 *   2. Every instruction decodes to a known opcode with complete operands
 *   3. Jump / try / null-coalesce targets land on instruction starts
 *   4. Const / local / function indices stay in range for this module
 *   5. Non-empty chunks end in an unconditional terminator
 *   6. LOAD_CONST_STR vs non-string pool entries (is_str)
 *   7. Stack depth never goes negative or above XENO_STACK_MAX
 *
 * Still deferred:
 *   - Host-fn index validity (host table is process-global)
 * ───────────────────────────────────────────────────────────────────────────*/

#include <stdio.h>
#include <stdarg.h>

static char g_xbc_ver_err[256];

/* Must match XENO_STACK_MAX in vm.h — kept local so the verifier does not
 * depend on the VM header. */
#define VER_STACK_MAX 256

static void ver_fail(const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(g_xbc_ver_err, sizeof(g_xbc_ver_err), fmt, ap);
    va_end(ap);
}

static bool ver_need(const Chunk *ch, int pos, int n)
{
    return n >= 0 && pos >= 0 && (size_t)pos + (size_t)n <= (size_t)ch->count;
}

static uint8_t ver_u8(const Chunk *ch, int *pos)
{
    return ch->code[(*pos)++];
}

static uint16_t ver_u16(const Chunk *ch, int *pos)
{
    uint16_t v = (uint16_t)ch->code[*pos] << 8 | ch->code[*pos + 1];
    *pos += 2;
    return v;
}

static int16_t ver_i16(const Chunk *ch, int *pos)
{
    return (int16_t)ver_u16(ch, pos);
}

static void ver_bit_set(uint8_t *bits, int i)
{
    bits[i >> 3] |= (uint8_t)(1u << (i & 7));
}

static bool ver_bit_test(const uint8_t *bits, int i)
{
    return (bits[i >> 3] >> (i & 7)) & 1;
}

/* Decode one instruction at *pos. On success advances *pos past operands
 * and returns true. On failure leaves *pos undefined and returns false.
 * If starts is non-NULL, records the instruction start for jump validation.
 * mode selects pre-merge (hollow-tolerant) vs post-merge (strict) rules. */
static bool ver_decode_one(const Module *module, const Chunk *ch,
                           int *pos, uint8_t *starts, XbcVerifyMode mode,
                           const XbcVerifyHostCtx *host)
{
    int start = *pos;
    if (!ver_need(ch, start, 1))
        return false;
    if (starts)
        ver_bit_set(starts, start);

    uint8_t op = ver_u8(ch, pos);
    bool strict = (mode == XBC_VERIFY_POST_MERGE);

    switch ((OpCode)op)
    {
    /* ── zero operand ─────────────────────────────────────────────── */
    case OP_ADD_INT: case OP_SUB_INT: case OP_MUL_INT: case OP_DIV_INT:
    case OP_MOD_INT: case OP_NEGATE_INT:
    case OP_ADD_FLOAT: case OP_SUB_FLOAT: case OP_MUL_FLOAT: case OP_DIV_FLOAT:
    case OP_MOD_FLOAT: case OP_NEGATE_FLOAT:
    case OP_CONCAT_STR:
    case OP_NOT_BOOL: case OP_AND_BOOL: case OP_OR_BOOL:
    case OP_CMP_EQ_INT: case OP_CMP_NEQ_INT: case OP_CMP_LT_INT:
    case OP_CMP_LTE_INT: case OP_CMP_GT_INT: case OP_CMP_GTE_INT:
    case OP_CMP_EQ_FLOAT: case OP_CMP_NEQ_FLOAT: case OP_CMP_LT_FLOAT:
    case OP_CMP_LTE_FLOAT: case OP_CMP_GT_FLOAT: case OP_CMP_GTE_FLOAT:
    case OP_CMP_EQ_BOOL: case OP_CMP_NEQ_BOOL:
    case OP_CMP_EQ_STR: case OP_CMP_NEQ_STR:
    case OP_CMP_EQ_VAL: case OP_CMP_NEQ_VAL:
    case OP_POP:
    case OP_RETURN: case OP_RETURN_VOID: case OP_MATCH_FAIL:
    case OP_TRUNC_I8: case OP_TRUNC_U8: case OP_TRUNC_I16: case OP_TRUNC_U16:
    case OP_TRUNC_I32: case OP_TRUNC_U32: case OP_TRUNC_U64: case OP_TRUNC_CHAR:
    case OP_ARRAY_GET: case OP_ARRAY_SET: case OP_ARRAY_LEN:
    case OP_LOAD_THIS: case OP_I2F: case OP_F2I:
    case OP_PUSH_NULL: case OP_IS_NULL:
    case OP_THROW: case OP_TRY_END: case OP_LOAD_EXCEPTION:
    case OP_TYPE_IS_ASSIGNABLE: case OP_TYPE_IS_INSTANCE:
    case OP_TYPE_GET_FIELD: case OP_TYPE_GET_METHOD:
    case OP_TYPE_HAS_ATTR: case OP_TYPE_GET_ATTR_ARG:
    case OP_METHOD_PARAM_AT:
        return true;

    /* ── u8 ───────────────────────────────────────────────────────── */
    case OP_LOAD_CONST_BOOL:
    case OP_LOAD_LOCAL:
    case OP_STORE_LOCAL:
    case OP_NEW_ARRAY:
    case OP_GET_FIELD:
    case OP_SET_FIELD:
    case OP_TYPE_FIELD:
    case OP_TYPE_NAMES:
    case OP_FIELD_PROP:
    case OP_METHOD_PROP:
    case OP_PARAM_PROP:
    {
        if (!ver_need(ch, *pos, 1)) return false;
        uint8_t a = ver_u8(ch, pos);
        /* Frame slots are uint8; hard cap matches MAX_LOCALS / VM layout. */
        if ((op == OP_LOAD_LOCAL || op == OP_STORE_LOCAL) && a >= 64)
            return false;
        return true;
    }

    /* ── u16 (const / host / jump-style) ──────────────────────────── */
    case OP_LOAD_CONST_INT:
    case OP_LOAD_CONST_FLOAT:
    case OP_LOAD_CONST_STR:
    {
        if (!ver_need(ch, *pos, 2)) return false;
        uint16_t idx = ver_u16(ch, pos);
        if ((int)idx >= ch->constants.count) return false;
        /* Constant kind tags required whenever the pool is non-empty. */
        if (ch->constants.count > 0 && !ch->constants.is_str)
        {
            ver_fail("const pool missing is_str tags");
            return false;
        }
        if (ch->constants.is_str)
        {
            int is_str = ch->constants.is_str[idx] != 0;
            if (op == OP_LOAD_CONST_STR && !is_str)
            {
                ver_fail("LOAD_CONST_STR idx=%u is not a string constant",
                         (unsigned)idx);
                return false;
            }
            if ((op == OP_LOAD_CONST_INT || op == OP_LOAD_CONST_FLOAT) && is_str)
            {
                ver_fail("LOAD_CONST numeric idx=%u is a string constant",
                         (unsigned)idx);
                return false;
            }
        }
        return true;
    }

    case OP_JUMP:
    case OP_JUMP_IF_FALSE:
    case OP_JUMP_IF_TRUE:
    case OP_NULL_COALESCE:
    case OP_NULL_ASSERT:   /* [uint16 line] — width matches jumps; not a target */
    case OP_TRY_BEGIN:
    {
        if (!ver_need(ch, *pos, 2)) return false;
        (void)ver_u16(ch, pos); /* jump targets checked in second pass */
        return true;
    }

    /* ── CALL family: u16 + u8 argc ───────────────────────────────── */
    case OP_CALL:
    {
        /* [u16 fn_idx][u8 argc] — non-virtual; post-merge arity is exact */
        if (!ver_need(ch, *pos, 3)) return false;
        uint16_t fn = ver_u16(ch, pos);
        uint8_t argc = ver_u8(ch, pos);
        if ((int)fn >= module->count) return false;
        if (strict)
        {
            const Chunk *callee = &module->chunks[fn];
            if (callee->count <= 0)
            {
                ver_fail("OP_CALL fn=%u has no code", (unsigned)fn);
                return false;
            }
            if ((int)argc != callee->param_count)
            {
                ver_fail("OP_CALL fn=%u argc=%u != param_count=%d",
                         (unsigned)fn, (unsigned)argc, callee->param_count);
                return false;
            }
        }
        return true;
    }
    case OP_CALL_METHOD:
    {
        /* [u16 method_slot][u8 argc] — ClassDef.methods[] slot, not fn table */
        if (!ver_need(ch, *pos, 3)) return false;
        uint16_t slot = ver_u16(ch, pos);
        (void)ver_u8(ch, pos);
        if (slot >= CLASS_MAX_METHODS) return false;
        return true;
    }
    case OP_CALL_SUPER:
    {
        /* [u16 parent_class_idx][u8 argc] */
        if (!ver_need(ch, *pos, 3)) return false;
        uint16_t par_ci = ver_u16(ch, pos);
        (void)ver_u8(ch, pos);
        if ((int)par_ci >= module->class_count) return false;
        return true;
    }
    case OP_CALL_IFACE:
    {
        /* [u16 name_const_idx][u8 argc] */
        if (!ver_need(ch, *pos, 3)) return false;
        uint16_t name_idx = ver_u16(ch, pos);
        (void)ver_u8(ch, pos);
        if ((int)name_idx >= ch->constants.count) return false;
        return true;
    }

    case OP_CALL_HOST:
    {
        if (!ver_need(ch, *pos, 3)) return false;
        uint16_t host_idx = ver_u16(ch, pos);
        uint8_t argc = ver_u8(ch, pos);
        if (host)
        {
            if ((int)host_idx >= host->host_fn_count)
            {
                ver_fail("OP_CALL_HOST index %u >= host_fn_count %d",
                         (unsigned)host_idx, host->host_fn_count);
                return false;
            }
            if (host->param_counts)
            {
                int pc = host->param_counts[host_idx];
                if (pc >= 0 && (int)argc != pc)
                {
                    ver_fail("OP_CALL_HOST idx=%u argc=%u != param_count=%d",
                             (unsigned)host_idx, (unsigned)argc, pc);
                    return false;
                }
            }
        }
        return true;
    }

    /* ── TO_STR: kind [+ optional class byte] ─────────────────────── */
    case OP_TO_STR:
    {
        if (!ver_need(ch, *pos, 1)) return false;
        uint8_t kind = ver_u8(ch, pos);
        if (kind == 4)
        {
            if (!ver_need(ch, *pos, 1)) return false;
            (void)ver_u8(ch, pos);
        }
        return true;
    }

    case OP_ARRAY_LIT:
    {
        if (!ver_need(ch, *pos, 2)) return false;
        (void)ver_u8(ch, pos); /* count */
        (void)ver_u8(ch, pos); /* elem_kind */
        return true;
    }

    case OP_LOAD_STATIC:
    case OP_STORE_STATIC:
    {
        /* [u8 class_idx][u8 field_idx]
         * PRE: hollow shells may have field_count == 0 — table caps only.
         * POST: class_idx in range and field_idx < real field_count. */
        if (!ver_need(ch, *pos, 2)) return false;
        uint8_t ci = ver_u8(ch, pos);
        uint8_t fi = ver_u8(ch, pos);
        if ((int)ci >= MODULE_MAX_CLASSES) return false;
        if ((int)fi >= CLASS_MAX_FIELDS) return false;
        if (strict)
        {
            if (!module || (int)ci >= module->class_count)
            {
                ver_fail("static access class_idx=%u out of range", (unsigned)ci);
                return false;
            }
            int fc = module->classes[ci].field_count;
            if ((int)fi >= fc)
            {
                ver_fail("static access class=%u field=%u >= field_count=%d",
                         (unsigned)ci, (unsigned)fi, fc);
                return false;
            }
        }
        else if (module && (int)ci < module->class_count)
        {
            int fc = module->classes[ci].field_count;
            if (fc > 0 && (int)fi >= fc)
                return false;
        }
        return true;
    }

    /* ── length-prefixed name encodings ───────────────────────────── */
    case OP_IS_TYPE:
    case OP_AS_TYPE:
    {
        if (!ver_need(ch, *pos, 2)) return false;
        (void)ver_u8(ch, pos); /* tag */
        uint8_t nlen = ver_u8(ch, pos);
        if (!ver_need(ch, *pos, nlen)) return false;
        *pos += nlen;
        return true;
    }

    case OP_TYPEOF:
    {
        if (!ver_need(ch, *pos, 3)) return false;
        (void)ver_u8(ch, pos); /* tag */
        (void)ver_u8(ch, pos); /* flags */
        uint8_t nlen = ver_u8(ch, pos);
        if (!ver_need(ch, *pos, nlen)) return false;
        *pos += nlen;
        return true;
    }

    case OP_EXCEPTION_IS_TYPE:
    {
        /* [u8 name_len][name bytes] */
        if (!ver_need(ch, *pos, 1)) return false;
        uint8_t nlen = ver_u8(ch, pos);
        if (!ver_need(ch, *pos, nlen)) return false;
        *pos += nlen;
        return true;
    }

    case OP_NEW:
    {
        /* [u8 nlen][name][u8 argc][u8 tac][tac kinds...]
         * POST: class name must resolve in the module. */
        if (!ver_need(ch, *pos, 1)) return false;
        uint8_t nlen = ver_u8(ch, pos);
        if (!ver_need(ch, *pos, nlen + 2)) return false;
        const uint8_t *name_bytes = &ch->code[*pos];
        *pos += nlen;
        (void)ver_u8(ch, pos); /* argc */
        uint8_t tac = ver_u8(ch, pos);
        if (tac > 8) tac = 8;
        if (!ver_need(ch, *pos, tac)) return false;
        *pos += tac;
        if (strict && module)
        {
            char cname[CLASS_NAME_MAX];
            if (nlen >= CLASS_NAME_MAX) nlen = CLASS_NAME_MAX - 1;
            memcpy(cname, name_bytes, nlen);
            cname[nlen] = '\0';
            if (module_find_class(module, cname) < 0)
            {
                ver_fail("OP_NEW class '%s' not found", cname);
                return false;
            }
        }
        return true;
    }

    case OP_CALL_STATIC:
    {
        /* [u8 nlen][name][u8 method_slot][u8 argc]
         * PRE: only reject impossible slots (hollow shells ok).
         * POST: class name must resolve and method_slot < method_count. */
        if (!ver_need(ch, *pos, 1)) return false;
        uint8_t nlen = ver_u8(ch, pos);
        if (!ver_need(ch, *pos, nlen + 2)) return false;
        const uint8_t *name_bytes = &ch->code[*pos];
        *pos += nlen;
        uint8_t method_slot = ver_u8(ch, pos);
        (void)ver_u8(ch, pos); /* argc */
        if (method_slot >= CLASS_MAX_METHODS) return false;
        if (strict && module)
        {
            char cname[CLASS_NAME_MAX];
            if (nlen >= CLASS_NAME_MAX) nlen = CLASS_NAME_MAX - 1;
            memcpy(cname, name_bytes, nlen);
            cname[nlen] = '\0';
            int ci = module_find_class(module, cname);
            if (ci < 0)
            {
                ver_fail("OP_CALL_STATIC class '%s' not found", cname);
                return false;
            }
            if ((int)method_slot >= module->classes[ci].method_count)
            {
                ver_fail("OP_CALL_STATIC '%s' method_slot=%u >= method_count=%d",
                         cname, (unsigned)method_slot,
                         module->classes[ci].method_count);
                return false;
            }
        }
        return true;
    }

    case OP_EVENT_SUBSCRIBE:
    case OP_EVENT_UNSUBSCRIBE:
    case OP_EVENT_SUBSCRIBE_BOUND:
    case OP_EVENT_UNSUBSCRIBE_BOUND:
    case OP_EVENT_SUBSCRIBE_MEMBER:
    case OP_EVENT_UNSUBSCRIBE_MEMBER:
    {
        /* [u8 ename_len][ename][u8 hname_len][hname] */
        if (!ver_need(ch, *pos, 1)) return false;
        uint8_t nlen = ver_u8(ch, pos);
        if (!ver_need(ch, *pos, nlen + 1)) return false;
        *pos += nlen;
        uint8_t hnlen = ver_u8(ch, pos);
        if (!ver_need(ch, *pos, hnlen)) return false;
        *pos += hnlen;
        return true;
    }

    case OP_EVENT_FIRE:
    case OP_EVENT_FIRE_MEMBER:
    {
        if (!ver_need(ch, *pos, 1)) return false;
        uint8_t nlen = ver_u8(ch, pos);
        if (!ver_need(ch, *pos, nlen + 1)) return false;
        *pos += nlen;
        (void)ver_u8(ch, pos); /* argc */
        return true;
    }

    default:
        ver_fail("unknown opcode %u", (unsigned)op);
        return false;
    }
}

static bool ver_chunk(const Module *module, const Chunk *ch, const char *name,
                      XbcVerifyMode mode, const XbcVerifyHostCtx *host)
{
    if (!ch || ch->count < 0)
    {
        ver_fail("chunk '%s': invalid header", name ? name : "?");
        return false;
    }
    /* Empty chunks are hollow external stubs pre-merge. Post-merge, emptiness
     * is only accepted for chunks that nothing references (checked later). */
    if (ch->count == 0)
        return true;

    if (ch->param_count < 0 || ch->local_count < 0)
    {
        ver_fail("chunk '%s': negative param/local count", name ? name : "?");
        return false;
    }
    if (ch->param_count > 64 || ch->local_count > 64)
    {
        ver_fail("chunk '%s': param/local count > 64", name ? name : "?");
        return false;
    }

    size_t bits_bytes = ((size_t)ch->count + 7) / 8;
    uint8_t *starts = calloc(1, bits_bytes ? bits_bytes : 1);
    if (!starts)
    {
        ver_fail("chunk '%s': out of memory", name ? name : "?");
        return false;
    }

    /* Pass 1: decode every instruction, record starts. */
    int pos = 0;
    while (pos < ch->count)
    {
        int at = pos;
        g_xbc_ver_err[0] = '\0';
        if (!ver_decode_one(module, ch, &pos, starts, mode, host))
        {
            if (!g_xbc_ver_err[0])
            {
                /* Dump a few preceding bytes so operand-size desyncs are obvious. */
                char hex[64] = {0};
                int hpos = 0;
                int from = at > 8 ? at - 8 : 0;
                for (int i = from; i < at && i < ch->count && hpos < 50; i++)
                    hpos += snprintf(hex + hpos, sizeof(hex) - (size_t)hpos,
                                     "%02x ", ch->code[i]);
                ver_fail("chunk '%s': decode failed at offset %d (op=%u) prev=[%s]",
                         name ? name : "?", at,
                         at < ch->count ? (unsigned)ch->code[at] : 0, hex);
            }
            else
            {
                /* prepend chunk context */
                char buf[256];
                snprintf(buf, sizeof(buf), "chunk '%s' @%d: %s",
                         name ? name : "?", at, g_xbc_ver_err);
                snprintf(g_xbc_ver_err, sizeof(g_xbc_ver_err), "%s", buf);
            }
            free(starts);
            return false;
        }
    }
    if (pos != ch->count)
    {
        ver_fail("chunk '%s': decode ended at %d, expected %d",
                 name ? name : "?", pos, ch->count);
        free(starts);
        return false;
    }

    /* Pass 2: jump / try / coalesce targets must land on instruction starts. */
    pos = 0;
    while (pos < ch->count)
    {
        int start = pos;
        uint8_t op = ch->code[pos++];
        switch ((OpCode)op)
        {
        case OP_JUMP:
        case OP_JUMP_IF_FALSE:
        case OP_JUMP_IF_TRUE:
        case OP_TRY_BEGIN:
        {
            /* Signed relative offset (matches VM READ_I16). */
            int16_t off = (int16_t)((uint16_t)ch->code[pos] << 8 | ch->code[pos + 1]);
            pos += 2;
            int target = pos + (int)off;
            /* target == count is a valid "jump past end" (fall off chunk). */
            if (target < 0 || target >= ch->count)
            {
                ver_fail("chunk '%s': bad jump target %d from %d",
                         name ? name : "?", target, start);
                free(starts);
                return false;
            }
            if (target < ch->count && !ver_bit_test(starts, target))
            {
                ver_fail("chunk '%s': jump from %d lands mid-instruction at %d",
                         name ? name : "?", start, target);
                free(starts);
                return false;
            }
            break;
        }
        case OP_NULL_COALESCE:
        {
            /* VM uses unsigned READ_U16 — do not sign-extend. */
            uint16_t uoff = (uint16_t)ch->code[pos] << 8 | ch->code[pos + 1];
            pos += 2;
            /* target = pos + uoff; reject wrap past end */
            if ((size_t)pos + (size_t)uoff >= (size_t)ch->count)
            {
                ver_fail("chunk '%s': NULL_COALESCE target out of range from %d",
                         name ? name : "?", start);
                free(starts);
                return false;
            }
            int target = pos + (int)uoff;
            if (target < ch->count && !ver_bit_test(starts, target))
            {
                ver_fail("chunk '%s': NULL_COALESCE lands mid-instruction at %d",
                         name ? name : "?", target);
                free(starts);
                return false;
            }
            break;
        }
        case OP_NULL_ASSERT:
            /* u16 line number — not a control-flow target */
            pos += 2;
            break;
        default:
            /* Re-decode to advance pos without re-checking (already valid). */
            pos = start;
            if (!ver_decode_one(module, ch, &pos, NULL, mode, host))
            {
                ver_fail("chunk '%s': re-decode failed at %d",
                         name ? name : "?", start);
                free(starts);
                return false;
            }
            break;
        }
    }

    /* ── Pass 3: fall-off-the-end + stack-depth simulation ─────────────
     * Walk instruction starts; compute net stack delta; propagate max
     * depth along fall-through and jump edges. Reject negative depth or
     * depth > VER_STACK_MAX. Require last instruction to be a terminator. */
    {
        int last_start = -1;
        uint8_t last_op = 0;
        pos = 0;
        while (pos < ch->count)
        {
            last_start = pos;
            last_op = ch->code[pos];
            if (!ver_decode_one(module, ch, &pos, NULL, mode, host))
            {
                ver_fail("chunk '%s': stack-pass re-decode failed at %d",
                         name ? name : "?", last_start);
                free(starts);
                return false;
            }
        }
        if (last_start >= 0)
        {
            switch ((OpCode)last_op)
            {
            case OP_RETURN:
            case OP_RETURN_VOID:
            case OP_THROW:
            case OP_JUMP:
            case OP_MATCH_FAIL:
                break; /* ok */
            default:
                ver_fail("chunk '%s': falls off end (last op=%u at %d)",
                         name ? name : "?", (unsigned)last_op, last_start);
                free(starts);
                return false;
            }
        }

        /* Stack-depth propagation with [min,max] per instruction.
         * Underflow checked against min; overflow against max.
         * -1 means unreachable. */
        int16_t *dmin = (int16_t *)malloc((size_t)ch->count * sizeof(int16_t));
        int16_t *dmax = (int16_t *)malloc((size_t)ch->count * sizeof(int16_t));
        if (!dmin || !dmax)
        {
            ver_fail("chunk '%s': out of memory (stack sim)", name ? name : "?");
            free(dmin); free(dmax);
            free(starts);
            return false;
        }
        for (int i = 0; i < ch->count; i++)
            dmin[i] = dmax[i] = -1;
        dmin[0] = dmax[0] = 0;

        bool changed = true;
        int guard = 0;
        const int guard_max = ch->count + 4;
        while (changed && guard < guard_max)
        {
            guard++;
            changed = false;
            pos = 0;
            while (pos < ch->count)
            {
                int start = pos;
                if (!ver_bit_test(starts, start) || dmin[start] < 0)
                {
                    /* Skip to next instruction start by decoding. */
                    if (!ver_decode_one(module, ch, &pos, NULL, mode, host))
                        break;
                    continue;
                }
                uint8_t op = ch->code[start];
                int next = start;
                if (!ver_decode_one(module, ch, &next, NULL, mode, host))
                    break;

                /* Net stack delta for this opcode (post-instruction depth
                 * relative to depth[start]). argc-bearing ops read operands. */
                int delta = 0;
                int argc = 0;
                switch ((OpCode)op)
                {
                case OP_LOAD_CONST_INT: case OP_LOAD_CONST_FLOAT:
                case OP_LOAD_CONST_BOOL: case OP_LOAD_CONST_STR:
                case OP_LOAD_LOCAL: case OP_LOAD_THIS: case OP_LOAD_EXCEPTION:
                case OP_PUSH_NULL: case OP_LOAD_STATIC:
                case OP_TYPEOF: /* -- Type */
                case OP_EXCEPTION_IS_TYPE: /* -- bool (reads caught_exception) */
                    delta = +1;
                    break;
                case OP_NEW_ARRAY: /* pop length, push array */
                    delta = 0;
                    break;
                case OP_ARRAY_LIT:
                {
                    uint8_t cnt = ch->code[start + 1];
                    delta = 1 - (int)cnt; /* pop count elems, push array */
                    break;
                }
                case OP_STORE_LOCAL: case OP_STORE_STATIC: case OP_POP:
                case OP_JUMP_IF_FALSE: case OP_JUMP_IF_TRUE:
                    delta = -1;
                    break;
                case OP_SET_FIELD: /* obj val -- */
                    delta = -2;
                    break;
                case OP_GET_FIELD: /* obj -- val */
                case OP_ARRAY_LEN: /* arr -- int */
                case OP_TYPE_FIELD: case OP_TYPE_NAMES:
                case OP_FIELD_PROP: case OP_METHOD_PROP: case OP_PARAM_PROP:
                case OP_IS_NULL: case OP_NOT_BOOL: case OP_NEGATE_INT:
                case OP_NEGATE_FLOAT: case OP_TO_STR: case OP_I2F: case OP_F2I:
                case OP_TRUNC_I8: case OP_TRUNC_U8: case OP_TRUNC_I16:
                case OP_TRUNC_U16: case OP_TRUNC_I32: case OP_TRUNC_U32:
                case OP_TRUNC_U64: case OP_TRUNC_CHAR:
                case OP_NULL_ASSERT: case OP_IS_TYPE: case OP_AS_TYPE:
                case OP_TYPE_GET_FIELD: case OP_TYPE_GET_METHOD:
                case OP_METHOD_PARAM_AT:
                    delta = 0; /* pop 1 push 1, or assert keeps value */
                    break;
                case OP_ADD_INT: case OP_SUB_INT: case OP_MUL_INT: case OP_DIV_INT:
                case OP_MOD_INT:
                case OP_ADD_FLOAT: case OP_SUB_FLOAT: case OP_MUL_FLOAT:
                case OP_DIV_FLOAT: case OP_MOD_FLOAT:
                case OP_CONCAT_STR:
                case OP_AND_BOOL: case OP_OR_BOOL:
                case OP_CMP_EQ_INT: case OP_CMP_NEQ_INT: case OP_CMP_LT_INT:
                case OP_CMP_LTE_INT: case OP_CMP_GT_INT: case OP_CMP_GTE_INT:
                case OP_CMP_EQ_FLOAT: case OP_CMP_NEQ_FLOAT: case OP_CMP_LT_FLOAT:
                case OP_CMP_LTE_FLOAT: case OP_CMP_GT_FLOAT: case OP_CMP_GTE_FLOAT:
                case OP_CMP_EQ_BOOL: case OP_CMP_NEQ_BOOL:
                case OP_CMP_EQ_STR: case OP_CMP_NEQ_STR:
                case OP_CMP_EQ_VAL: case OP_CMP_NEQ_VAL:
                case OP_ARRAY_GET: /* arr idx -- val */
                case OP_TYPE_IS_INSTANCE: /* val Type -- bool */
                case OP_TYPE_IS_ASSIGNABLE:
                case OP_TYPE_HAS_ATTR: /* name Type -- bool */
                    delta = -1; /* pop 2 push 1 */
                    break;
                case OP_TYPE_GET_ATTR_ARG: /* idx name Type -- str? */
                    delta = -2; /* pop 3 push 1 */
                    break;
                case OP_ARRAY_SET: /* arr idx val -- */
                    delta = -3;
                    break;
                case OP_JUMP: case OP_TRY_BEGIN: case OP_TRY_END:
                case OP_MATCH_FAIL:
                    delta = 0;
                    break;
                case OP_NULL_COALESCE:
                    /* Fall-through pops null (delta -1); jump keeps value.
                     * Use pops=1; per-edge join adjusts below. */
                    delta = 0;
                    break;
                case OP_CALL: /* [u16 fn][u8 argc] — pop argc, push 1 */
                    argc = ch->code[start + 3];
                    delta = -argc + 1;
                    break;
                case OP_CALL_METHOD: /* this + args */
                case OP_CALL_IFACE:
                    argc = ch->code[start + 3];
                    delta = -(argc + 1) + 1;
                    break;
                case OP_CALL_SUPER: /* args only; this is frame slot 0 */
                    argc = ch->code[start + 3];
                    delta = -argc; /* void constructor */
                    break;
                case OP_CALL_HOST:
                    argc = ch->code[start + 3];
                    delta = -argc + 1;
                    break;
                case OP_CALL_STATIC:
                {
                    /* [u8 nlen][name][u8 slot][u8 argc] */
                    uint8_t nlen = ch->code[start + 1];
                    argc = ch->code[start + 1 + 1 + nlen + 1];
                    delta = -argc + 1;
                    break;
                }
                case OP_NEW:
                {
                    uint8_t nlen = ch->code[start + 1];
                    argc = ch->code[start + 1 + 1 + nlen];
                    delta = -argc + 1;
                    break;
                }
                case OP_RETURN: /* pops return value */
                    delta = -1;
                    break;
                case OP_RETURN_VOID:
                    delta = 0;
                    break;
                case OP_THROW: /* pops exception object */
                    delta = -1;
                    break;
                case OP_EVENT_SUBSCRIBE: case OP_EVENT_UNSUBSCRIBE:
                    delta = 0; /* names in operands */
                    break;
                case OP_EVENT_SUBSCRIBE_BOUND: case OP_EVENT_UNSUBSCRIBE_BOUND:
                    delta = -1; /* pop receiver */
                    break;
                case OP_EVENT_SUBSCRIBE_MEMBER: case OP_EVENT_UNSUBSCRIBE_MEMBER:
                    delta = -1; /* pop receiver object */
                    break;
                case OP_EVENT_FIRE: case OP_EVENT_FIRE_MEMBER:
                {
                    /* [u8 nlen][name][u8 argc] — pop argc args (+ receiver for MEMBER) */
                    uint8_t nlen = ch->code[start + 1];
                    uint8_t fac = ch->code[start + 2 + nlen];
                    delta = (op == OP_EVENT_FIRE_MEMBER) ? -(int)fac - 1 : -(int)fac;
                    break;
                }
                default:
                    ver_fail("chunk '%s': unknown stack effect for op=%u at %d",
                             name ? name : "?", (unsigned)op, start);
                    free(dmin); free(dmax);
                    free(starts);
                    return false;
                }

                /* Minimum operands required on stack before this op. */
                int pops = 0;
                if (delta < 0)
                    pops = -delta; /* lower bound; refined below */
                switch ((OpCode)op)
                {
                case OP_ADD_INT: case OP_SUB_INT: case OP_MUL_INT: case OP_DIV_INT:
                case OP_MOD_INT: case OP_ADD_FLOAT: case OP_SUB_FLOAT:
                case OP_MUL_FLOAT: case OP_DIV_FLOAT: case OP_MOD_FLOAT:
                case OP_CONCAT_STR: case OP_AND_BOOL: case OP_OR_BOOL:
                case OP_CMP_EQ_INT: case OP_CMP_NEQ_INT: case OP_CMP_LT_INT:
                case OP_CMP_LTE_INT: case OP_CMP_GT_INT: case OP_CMP_GTE_INT:
                case OP_CMP_EQ_FLOAT: case OP_CMP_NEQ_FLOAT: case OP_CMP_LT_FLOAT:
                case OP_CMP_LTE_FLOAT: case OP_CMP_GT_FLOAT: case OP_CMP_GTE_FLOAT:
                case OP_CMP_EQ_BOOL: case OP_CMP_NEQ_BOOL:
                case OP_CMP_EQ_STR: case OP_CMP_NEQ_STR:
                case OP_CMP_EQ_VAL: case OP_CMP_NEQ_VAL:
                case OP_ARRAY_GET: case OP_TYPE_IS_INSTANCE:
                case OP_TYPE_IS_ASSIGNABLE: case OP_TYPE_HAS_ATTR:
                case OP_SET_FIELD:
                    pops = 2; break;
                case OP_ARRAY_SET: case OP_TYPE_GET_ATTR_ARG:
                    pops = 3; break;
                case OP_GET_FIELD: case OP_ARRAY_LEN: case OP_TYPE_FIELD:
                case OP_TYPE_NAMES: case OP_FIELD_PROP: case OP_METHOD_PROP:
                case OP_PARAM_PROP: case OP_IS_NULL: case OP_NOT_BOOL:
                case OP_NEGATE_INT: case OP_NEGATE_FLOAT: case OP_TO_STR:
                case OP_I2F: case OP_F2I: case OP_NULL_ASSERT:
                case OP_IS_TYPE: case OP_AS_TYPE: case OP_STORE_LOCAL:
                case OP_STORE_STATIC: case OP_POP: case OP_JUMP_IF_FALSE:
                case OP_JUMP_IF_TRUE: case OP_RETURN: case OP_THROW:
                case OP_NEW_ARRAY: case OP_NULL_COALESCE:
                case OP_TRUNC_I8: case OP_TRUNC_U8: case OP_TRUNC_I16:
                case OP_TRUNC_U16: case OP_TRUNC_I32: case OP_TRUNC_U32:
                case OP_TRUNC_U64: case OP_TRUNC_CHAR:
                case OP_TYPE_GET_FIELD: case OP_TYPE_GET_METHOD:
                case OP_METHOD_PARAM_AT:
                    pops = 1; break;
                case OP_ARRAY_LIT:
                    pops = (int)ch->code[start + 1]; break;
                case OP_CALL: case OP_CALL_HOST: case OP_CALL_STATIC:
                    pops = argc; break;
                case OP_CALL_METHOD: case OP_CALL_IFACE:
                    pops = argc + 1; break;
                case OP_CALL_SUPER:
                    pops = argc; break;
                case OP_NEW:
                    pops = argc; break;
                case OP_EVENT_SUBSCRIBE: case OP_EVENT_UNSUBSCRIBE:
                    pops = 0; break;
                case OP_EVENT_SUBSCRIBE_BOUND: case OP_EVENT_UNSUBSCRIBE_BOUND:
                case OP_EVENT_SUBSCRIBE_MEMBER: case OP_EVENT_UNSUBSCRIBE_MEMBER:
                    pops = 1; break;
                case OP_EVENT_FIRE: case OP_EVENT_FIRE_MEMBER:
                {
                    uint8_t nlen = ch->code[start + 1];
                    uint8_t fac = ch->code[start + 2 + nlen];
                    pops = (op == OP_EVENT_FIRE_MEMBER) ? (int)fac + 1 : (int)fac;
                    break;
                }
                default:
                    break;
                }
                if ((int)dmin[start] < pops)
                {
                    ver_fail("chunk '%s': stack underflow at %d (op=%u need %d have %d)",
                             name ? name : "?", start, (unsigned)op,
                             pops, (int)dmin[start]);
                    free(dmin); free(dmax);
                    free(starts);
                    return false;
                }

                int after_min = (int)dmin[start] + delta;
                int after_max = (int)dmax[start] + delta;
                /* NULL_COALESCE fall-through: pop null → after_min uses -1 */
                if (op == OP_NULL_COALESCE)
                    after_min = (int)dmin[start] - 1;
                if (after_min < 0)
                {
                    ver_fail("chunk '%s': stack underflow at %d (op=%u min_depth=%d delta=%d)",
                             name ? name : "?", start, (unsigned)op,
                             (int)dmin[start], delta);
                    free(dmin); free(dmax);
                    free(starts);
                    return false;
                }
                if (after_max > VER_STACK_MAX)
                {
                    ver_fail("chunk '%s': stack overflow at %d (max_depth %d > %d)",
                             name ? name : "?", start, after_max, VER_STACK_MAX);
                    free(dmin); free(dmax);
                    free(starts);
                    return false;
                }

                /* Propagate to successors. */
                int is_term = (op == OP_RETURN || op == OP_RETURN_VOID ||
                               op == OP_THROW || op == OP_MATCH_FAIL);
                int is_uncond_jump = (op == OP_JUMP);

                /* [min,max] join: min of mins, max of maxes. */
                #define VER_JOIN_RANGE(tgt, amin, amax)                           \
                    do {                                                          \
                        int _t = (tgt);                                           \
                        if (_t >= 0 && _t < ch->count)                            \
                        {                                                         \
                            if (dmin[_t] < 0)                                     \
                            {                                                     \
                                dmin[_t] = (int16_t)(amin);                       \
                                dmax[_t] = (int16_t)(amax);                       \
                                changed = true;                                   \
                            }                                                     \
                            else                                                  \
                            {                                                     \
                                if ((int)dmin[_t] > (amin))                       \
                                { dmin[_t] = (int16_t)(amin); changed = true; }   \
                                if ((int)dmax[_t] < (amax))                       \
                                { dmax[_t] = (int16_t)(amax); changed = true; }   \
                            }                                                     \
                        }                                                         \
                    } while (0)

                if (!is_term && !is_uncond_jump && next < ch->count)
                    VER_JOIN_RANGE(next, after_min, after_max);
                if (op == OP_JUMP || op == OP_JUMP_IF_FALSE ||
                    op == OP_JUMP_IF_TRUE || op == OP_TRY_BEGIN)
                {
                    int16_t off = (int16_t)((uint16_t)ch->code[start + 1] << 8 |
                                            ch->code[start + 2]);
                    int target = start + 3 + (int)off;
                    VER_JOIN_RANGE(target, after_min, after_max);
                }
                else if (op == OP_NULL_COALESCE)
                {
                    /* non-null: keep value (depth unchanged relative to start)
                     * null: pop then RHS pushes — still net 0 at join */
                    uint16_t uoff = (uint16_t)ch->code[start + 1] << 8 |
                                    ch->code[start + 2];
                    int target = start + 3 + (int)uoff;
                    VER_JOIN_RANGE(target, (int)dmin[start], (int)dmax[start]);
                }
                #undef VER_JOIN_RANGE

                pos = next;
            }
        }
        if (changed)
        {
            ver_fail("chunk '%s': stack-depth analysis did not converge",
                     name ? name : "?");
            free(dmin); free(dmax);
            free(starts);
            return false;
        }
        {
            int peak = 0;
            for (int i = 0; i < ch->count; i++)
                if (dmax[i] > peak) peak = dmax[i];
            /* Drop const to record peak — Module is not sealed yet. */
            ((Chunk *)ch)->max_stack = peak;
        }
        free(dmin); free(dmax);
    }

    free(starts);
    return true;
}

XbcResult xbc_verify_module(const Module *module, XbcVerifyMode mode)
{
    return xbc_verify_module_ex(module, mode, NULL);
}

XbcResult xbc_verify_module_ex(const Module *module, XbcVerifyMode mode,
                               const XbcVerifyHostCtx *host)
{
    g_xbc_ver_err[0] = '\0';
    if (!module)
    {
        ver_fail("null module");
        return XBC_ERR_CORRUPT;
    }

    bool strict = (mode == XBC_VERIFY_POST_MERGE);

    if (module->count < 0 || module->count > MODULE_MAX_FUNCTIONS)
    {
        ver_fail("bad function count %d", module->count);
        return XBC_ERR_CORRUPT;
    }
    if (module->class_count < 0 || module->class_count > MODULE_MAX_CLASSES)
    {
        ver_fail("bad class count %d", module->class_count);
        return XBC_ERR_CORRUPT;
    }
    if (module->sinit_index < -1 || module->sinit_index >= module->count)
    {
        ver_fail("bad sinit_index %d", module->sinit_index);
        return XBC_ERR_CORRUPT;
    }

    for (int ci = 0; ci < module->class_count; ci++)
    {
        const ClassDef *cls = &module->classes[ci];
        if (cls->field_count < 0 || cls->field_count > CLASS_MAX_FIELDS)
        {
            ver_fail("class '%s': bad field_count %d", cls->name, cls->field_count);
            return XBC_ERR_CORRUPT;
        }
        if (cls->method_count < 0 || cls->method_count > CLASS_MAX_METHODS)
        {
            ver_fail("class '%s': bad method_count %d", cls->name, cls->method_count);
            return XBC_ERR_CORRUPT;
        }
        if (cls->event_count < 0 || cls->event_count > CLASS_MAX_EVENTS)
        {
            ver_fail("class '%s': bad event_count %d", cls->name, cls->event_count);
            return XBC_ERR_CORRUPT;
        }

        /* parent_index: -1 = none; otherwise must be in range. */
        if (cls->parent_index < -1 ||
            (cls->parent_index >= module->class_count && cls->parent_index != -1))
        {
            ver_fail("class '%s': bad parent_index %d (class_count=%d)",
                     cls->name, cls->parent_index, module->class_count);
            return XBC_ERR_CORRUPT;
        }

        /* constructor_index: -1 ok for interfaces; concrete classes post-merge
         * must point at a chunk with code when a constructor is declared. */
        if (cls->constructor_index < -1 ||
            cls->constructor_index >= module->count)
        {
            ver_fail("class '%s': bad constructor_index %d",
                     cls->name, cls->constructor_index);
            return XBC_ERR_CORRUPT;
        }
        if (strict && !cls->is_interface && cls->constructor_index >= 0)
        {
            if (module->chunks[cls->constructor_index].count <= 0)
            {
                ver_fail("class '%s': constructor chunk has no code", cls->name);
                return XBC_ERR_CORRUPT;
            }
        }

        for (int mi = 0; mi < cls->method_count; mi++)
        {
            int fn = cls->methods[mi].fn_index;
            /* PRE: hollow external methods may use -1 until merge resolves them.
             * POST: concrete methods must resolve to a real chunk with code.
             *       Interface methods are intentionally abstract (fn_index=-1). */
            if (strict)
            {
                if (cls->is_interface)
                {
                    if (fn < -1 || fn >= module->count)
                    {
                        ver_fail("class '%s' method '%s': bad fn_index %d",
                                 cls->name, cls->methods[mi].name, fn);
                        return XBC_ERR_CORRUPT;
                    }
                }
                else
                {
                    if (fn < 0 || fn >= module->count)
                    {
                        ver_fail("class '%s' method '%s': unresolved fn_index %d",
                                 cls->name, cls->methods[mi].name, fn);
                        return XBC_ERR_CORRUPT;
                    }
                    if (module->chunks[fn].count <= 0)
                    {
                        ver_fail("class '%s' method '%s': chunk has no code",
                                 cls->name, cls->methods[mi].name);
                        return XBC_ERR_CORRUPT;
                    }
                }
            }
            else if (fn < -1 || fn >= module->count)
            {
                ver_fail("class '%s' method '%s': bad fn_index %d",
                         cls->name, cls->methods[mi].name, fn);
                return XBC_ERR_CORRUPT;
            }
            if (cls->methods[mi].param_count < 0 ||
                cls->methods[mi].param_count > METHOD_MAX_PARAMS)
            {
                ver_fail("class '%s' method '%s': bad param_count %d",
                         cls->name, cls->methods[mi].name,
                         cls->methods[mi].param_count);
                return XBC_ERR_CORRUPT;
            }
        }
    }

    /* Parent-chain cycle detection (post-merge meaningful; cheap always). */
    for (int ci = 0; ci < module->class_count; ci++)
    {
        int seen[MODULE_MAX_CLASSES];
        int nseen = 0;
        int cur = ci;
        while (cur >= 0)
        {
            for (int s = 0; s < nseen; s++)
            {
                if (seen[s] == cur)
                {
                    ver_fail("class '%s': parent_index cycle",
                             module->classes[ci].name);
                    return XBC_ERR_CORRUPT;
                }
            }
            if (nseen >= MODULE_MAX_CLASSES) break;
            seen[nseen++] = cur;
            cur = module->classes[cur].parent_index;
            if (cur < -1 || cur >= module->class_count)
                break; /* range already checked */
        }
    }

    for (int i = 0; i < module->count; i++)
    {
        const char *name = (module->names[i][0]) ? module->names[i] : "?";
        if (!ver_chunk(module, &module->chunks[i], name, mode, host))
            return XBC_ERR_CORRUPT;
    }

    return XBC_OK;
}

const char *xbc_result_str(XbcResult r) {
    switch (r) {
        case XBC_OK:              return "OK";
        case XBC_ERR_IO:          return "I/O error";
        case XBC_ERR_BAD_MAGIC:   return "Not a valid .xbc file";
        case XBC_ERR_BAD_VERSION: return "Unsupported .xbc version";
        case XBC_ERR_CORRUPT:
            if (g_xbc_ver_err[0])
                return g_xbc_ver_err;
            return "File appears corrupt or failed verification";
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
    XbcResult r = deserialize_module(module, &rb);
    if (r != XBC_OK)
        return r;
    /* Reject hostile / malformed bytecode before the Module escapes.
     * PRE_MERGE: hollow stubs (empty code, fn_index=-1) are still allowed. */
    r = xbc_verify_module(module, XBC_VERIFY_PRE_MERGE);
    if (r != XBC_OK) {
        module_free(module);
        memset(module, 0, sizeof(*module));
    } else {
        module->from_load = true;
    }
    return r;
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