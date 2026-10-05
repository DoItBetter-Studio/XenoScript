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
    if (version > XBC_VERSION)
        return XBC_ERR_BAD_VERSION;

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
            } else {
                int64_t raw = (int64_t)rb_u64(rb);
                if (rb->error) return XBC_ERR_IO;
				Value v; memset(&v, 0, sizeof(Value));
                v.i = (__int128_t)raw;    /* sign-extend into __int128 */
				chunk_add_constant(chunk, v);
            }
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