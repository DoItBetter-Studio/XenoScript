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

/*
 * Correct serialization — all in one pass, no double-writes.
 */
static bool serialize_module(WriteBuf *wb, const Module *module) {

    /* ── Header ──────────────────────────────────────────────────────── */
    if (!wb_write(wb, "XBC", 4)) return false;  /* includes null terminator */
    if (!wb_u8(wb, XBC_VERSION))  return false;
    if (!wb_u16(wb, (uint16_t)module->count))       return false;
    if (!wb_u16(wb, (uint16_t)module->class_count)) return false;

    /* ── Static init chunk index ──────────────────────────────────────── */
    if (!wb_u16(wb, (uint16_t)(module->sinit_index < 0 ? 0xFFFF : (uint16_t)module->sinit_index))) return false;

    /* ── Mod metadata ────────────────────────────────────────────────── */
    if (!wb_u8(wb, module->metadata.has_mod ? 1 : 0)) return false;
    if (module->metadata.has_mod) {
        if (!wb_str(wb, module->metadata.name))        return false;
        if (!wb_str(wb, module->metadata.version))     return false;
        if (!wb_str(wb, module->metadata.author))      return false;
        if (!wb_str(wb, module->metadata.description)) return false;
        if (!wb_str(wb, module->metadata.entry_class)) return false;
    }

    /* ── Class definitions ───────────────────────────────────────────── */
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
        }

        /* Methods */
        if (!wb_u16(wb, (uint16_t)cls->method_count)) return false;
        for (int mi = 0; mi < cls->method_count; mi++) {
            const MethodDef *md = &cls->methods[mi];
            if (!wb_str(wb, md->name))                    return false;
            if (!wb_u32(wb, (uint32_t)md->fn_index))      return false;
            if (!wb_u8(wb, md->is_static ? 1 : 0))       return false;
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

                /* OP_NEW: uint16 class_idx + uint8 argc = 3 bytes */
                case OP_NEW:
                    pc += 3; break;

                /* 2-byte operand opcodes (two uint8_t operands = class_idx + field_idx) */
                case OP_LOAD_STATIC:
                case OP_STORE_STATIC:
                    pc += 2; break;  /* opcode already consumed; 2 operand bytes follow */

                /* 1-byte operand opcodes */
                case OP_LOAD_LOCAL:
                case OP_STORE_LOCAL:
                case OP_LOAD_CONST_BOOL:
                case OP_GET_FIELD:
                case OP_SET_FIELD:
                case OP_TO_STR: {
                    uint8_t kind = (pc < chunk->count) ? chunk->code[pc] : 0;
                    pc += (kind == 4) ? 2 : 1; break;
                }
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
                case OP_NEW_ARRAY:
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
                    break;

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
                uint64_t raw;
                memcpy(&raw, &v, 8);
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

    /* ── Static init chunk index ──────────────────────────────────────── */
    uint16_t sinit_raw = rb_u16(rb);
    if (rb->error) return XBC_ERR_IO;
    module->sinit_index = (sinit_raw == 0xFFFF) ? -1 : (int)sinit_raw;

    /* ── Mod metadata ────────────────────────────────────────────────── */
    uint8_t has_mod = rb_u8(rb);
    if (rb->error) return XBC_ERR_IO;
    module->metadata.has_mod = has_mod != 0;
    if (module->metadata.has_mod) {
        char *s;
        s = rb_str(rb); if (rb->error || !s) return XBC_ERR_OOM;
        strncpy(module->metadata.name, s, MOD_STRING_MAX - 1);
        module->metadata.name[MOD_STRING_MAX - 1] = '\0'; free(s);

        s = rb_str(rb); if (rb->error || !s) return XBC_ERR_OOM;
        strncpy(module->metadata.version, s, MOD_STRING_MAX - 1);
        module->metadata.version[MOD_STRING_MAX - 1] = '\0'; free(s);

        s = rb_str(rb); if (rb->error || !s) return XBC_ERR_OOM;
        strncpy(module->metadata.author, s, MOD_STRING_MAX - 1);
        module->metadata.author[MOD_STRING_MAX - 1] = '\0'; free(s);

        s = rb_str(rb); if (rb->error || !s) return XBC_ERR_OOM;
        strncpy(module->metadata.description, s, MOD_STRING_MAX - 1);
        module->metadata.description[MOD_STRING_MAX - 1] = '\0'; free(s);

        s = rb_str(rb); if (rb->error || !s) return XBC_ERR_OOM;
        strncpy(module->metadata.entry_class, s, MOD_STRING_MAX - 1);
        module->metadata.entry_class[MOD_STRING_MAX - 1] = '\0'; free(s);
    }

    /* ── Class definitions ───────────────────────────────────────────── */
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

            md->fn_index  = (int)rb_u32(rb);
            md->is_static = rb_u8(rb) != 0;
            if (rb->error) return XBC_ERR_IO;
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
        if (rb->error) return XBC_ERR_IO;

        /* Constant pool */
        uint16_t const_count = rb_u16(rb);
        if (rb->error) return XBC_ERR_IO;

        for (int i = 0; i < const_count; i++) {
            uint8_t kind = rb_u8(rb);
            if (rb->error) return XBC_ERR_IO;

            Value v = {0};
            if (kind == CONST_STR) {
                char *s = rb_str(rb);
                if (rb->error || !s) return XBC_ERR_OOM;
                v.s = s;
            } else {
                uint64_t raw = rb_u64(rb);
                if (rb->error) return XBC_ERR_IO;
                memcpy(&v, &raw, 8);
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