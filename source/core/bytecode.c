/*
 * bytecode.c — Chunk implementation and disassembler
 *
 * The disassembler is one of the most useful debugging tools in the whole
 * pipeline. When the compiler produces wrong output, you disassemble the
 * chunk and read it like source code — immediately seeing what went wrong.
 */

#include "bytecode.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ─────────────────────────────────────────────────────────────────────────────
 * CHUNK LIFECYCLE
 * ───────────────────────────────────────────────────────────────────────────*/

void chunk_init(Chunk *chunk)
{
    chunk->code = NULL;
    chunk->lines = NULL;
    chunk->count = 0;
    chunk->capacity = 0;
    chunk->local_count = 0;
    chunk->param_count = 0;
    chunk->is_constructor = false;
    chunk->return_type_kind = 0;
    memset(chunk->param_type_kinds, 0, sizeof(chunk->param_type_kinds));
    chunk->constants.values = NULL;
    chunk->constants.count = 0;
    chunk->constants.capacity = 0;
}

void chunk_free(Chunk *chunk)
{
    free(chunk->code);
    free(chunk->lines);
    free(chunk->constants.values);
    chunk_init(chunk); /* Reset to clean state */
}

/* ─────────────────────────────────────────────────────────────────────────────
 * WRITING BYTES
 * ───────────────────────────────────────────────────────────────────────────*/

int chunk_write(Chunk *chunk, uint8_t byte, int line)
{
    /* Grow if needed — double capacity each time (amortized O(1)) */
    if (chunk->count >= chunk->capacity)
    {
        int new_cap = chunk->capacity < CHUNK_INIT_CAPACITY
                          ? CHUNK_INIT_CAPACITY
                          : chunk->capacity * 2;
        chunk->code = realloc(chunk->code, new_cap * sizeof(uint8_t));
        chunk->lines = realloc(chunk->lines, new_cap * sizeof(int));
        chunk->capacity = new_cap;
    }

    int offset = chunk->count;
    chunk->code[chunk->count] = byte;
    chunk->lines[chunk->count] = line;
    chunk->count++;
    return offset;
}

int chunk_write_u16(Chunk *chunk, uint16_t value, int line)
{
    /* Big-endian: high byte first, then low byte.
     * Consistent byte order makes the disassembler and VM simpler. */
    int offset = chunk_write(chunk, (value >> 8) & 0xFF, line);
    chunk_write(chunk, value & 0xFF, line);
    return offset;
}

void chunk_patch_u16(Chunk *chunk, int offset, uint16_t value)
{
    /* Overwrite the two bytes at offset with the new value.
     * Called after a forward jump to fill in the real target offset. */
    chunk->code[offset] = (value >> 8) & 0xFF;
    chunk->code[offset + 1] = value & 0xFF;
}

/* ─────────────────────────────────────────────────────────────────────────────
 * CONSTANT POOL
 * ───────────────────────────────────────────────────────────────────────────*/

int chunk_add_constant(Chunk *chunk, Value value)
{
    ConstPool *pool = &chunk->constants;
    if (pool->count >= pool->capacity)
    {
        int new_cap = pool->capacity < 8 ? 8 : pool->capacity * 2;
        pool->values = realloc(pool->values, new_cap * sizeof(Value));
        pool->capacity = new_cap;
    }
    int index = pool->count;
    pool->values[index] = value;
    pool->count++;
    return index;
}

/* ─────────────────────────────────────────────────────────────────────────────
 * DISASSEMBLER
 *
 * Walks the instruction stream and prints each instruction.
 * This must stay in sync with the VM's fetch-decode loop.
 *
 * Returns the offset of the NEXT instruction (used to advance the loop).
 * ───────────────────────────────────────────────────────────────────────────*/

/* Print a simple instruction with no operands */
static int dis_simple(const char *name, int offset)
{
    printf("%-20s\n", name);
    return offset + 1; /* 1 byte: just the opcode */
}

/* Print an instruction with one uint8_t operand */
static int dis_byte(const char *name, const Chunk *chunk, int offset)
{
    uint8_t operand = chunk->code[offset + 1];
    printf("%-20s %4d\n", name, operand);
    return offset + 2; /* 1 opcode + 1 operand byte */
}

/* Print an instruction with one uint16_t operand */

/* Print a LOAD_CONST instruction — shows the constant's value too */
static int dis_const(const char *name, const Chunk *chunk,
                     int offset, bool is_str)
{
    uint16_t idx = (chunk->code[offset + 1] << 8) | chunk->code[offset + 2];
    Value v = chunk->constants.values[idx];

    printf("%-20s %4d  ", name, idx);

    if (is_str)
    {
        printf("(\"%s\")", v.s ? v.s : "<null>");
    }
    else
    {
        /* We don't know the type here without a tag, so print both
         * int and float interpretations — the opcode name tells you which */
        printf("(int: %lld  float: %g)", (long long)v.i, v.f);
    }
    printf("\n");
    return offset + 3;
}

/* Print a JUMP instruction — shows the absolute target offset */
static int dis_jump(const char *name, const Chunk *chunk, int offset)
{
    /* Read the signed 16-bit relative offset */
    int16_t jump = (int16_t)((chunk->code[offset + 1] << 8) | chunk->code[offset + 2]);
    /* Target is relative to the instruction AFTER this one (offset + 3) */
    int target = offset + 3 + jump;
    printf("%-20s %4d  -> %04d\n", name, jump, target);
    return offset + 3;
}

/* Print a CALL instruction — shows fn index and arg count */
static int dis_call(const char *name, const Chunk *chunk, int offset)
{
    uint16_t fn_idx = (chunk->code[offset + 1] << 8) | chunk->code[offset + 2];
    uint8_t argc = chunk->code[offset + 3];
    printf("%-20s fn[%d]  argc=%d\n", name, fn_idx, argc);
    return offset + 4; /* 1 opcode + 2 fn_idx + 1 argc */
}

/*
 * Disassemble a single instruction at `offset`.
 * Returns the offset of the next instruction.
 */
static int disassemble_instruction(const Chunk *chunk, int offset)
{
    /* Print byte offset and source line */
    printf("%04d  ", offset);

    /* Print line number, suppressing duplicates for readability */
    if (offset > 0 && chunk->lines[offset] == chunk->lines[offset - 1])
    {
        printf("       |   ");
    }
    else
    {
        printf("line %4d  ", chunk->lines[offset]);
    }

    OpCode op = (OpCode)chunk->code[offset];

    switch (op)
    {
    /* Constants */
    case OP_LOAD_CONST_INT:
        return dis_const("LOAD_CONST_INT", chunk, offset, false);
    case OP_LOAD_CONST_FLOAT:
        return dis_const("LOAD_CONST_FLOAT", chunk, offset, false);
    case OP_LOAD_CONST_BOOL:
        return dis_byte("LOAD_CONST_BOOL", chunk, offset);
    case OP_LOAD_CONST_STR:
        return dis_const("LOAD_CONST_STR", chunk, offset, true);

    /* Locals */
    case OP_LOAD_LOCAL:
        return dis_byte("LOAD_LOCAL", chunk, offset);
    case OP_STORE_LOCAL:
        return dis_byte("STORE_LOCAL", chunk, offset);

    /* Integer arithmetic */
    case OP_ADD_INT:
        return dis_simple("ADD_INT", offset);
    case OP_SUB_INT:
        return dis_simple("SUB_INT", offset);
    case OP_MUL_INT:
        return dis_simple("MUL_INT", offset);
    case OP_DIV_INT:
        return dis_simple("DIV_INT", offset);
    case OP_MOD_INT:
        return dis_simple("MOD_INT", offset);
    case OP_NEGATE_INT:
        return dis_simple("NEGATE_INT", offset);

    /* Float arithmetic */
    case OP_ADD_FLOAT:
        return dis_simple("ADD_FLOAT", offset);
    case OP_SUB_FLOAT:
        return dis_simple("SUB_FLOAT", offset);
    case OP_MUL_FLOAT:
        return dis_simple("MUL_FLOAT", offset);
    case OP_DIV_FLOAT:
        return dis_simple("DIV_FLOAT", offset);
    case OP_MOD_FLOAT:
        return dis_simple("MOD_FLOAT", offset);
    case OP_NEGATE_FLOAT:
        return dis_simple("NEGATE_FLOAT", offset);

    /* String */
    case OP_CONCAT_STR:
        return dis_simple("CONCAT_STR", offset);
    case OP_TO_STR:
    {
        uint8_t kind = chunk->code[offset + 1];
        if (kind == 4)
        {
            uint8_t class_idx = chunk->code[offset + 2];
            printf("%04d TO_STR kind=enum class[%d]\n", offset, class_idx);
            return offset + 3;
        }
        const char *kind_name[] = {"int", "float", "bool", "char", "enum"};
        printf("%04d TO_STR kind=%s\n", offset, kind < 4 ? kind_name[kind] : "?");
        return offset + 2;
    }

    /* Boolean */
    case OP_NOT_BOOL:
        return dis_simple("NOT_BOOL", offset);
    case OP_AND_BOOL:
        return dis_simple("AND_BOOL", offset);
    case OP_OR_BOOL:
        return dis_simple("OR_BOOL", offset);

    /* Integer comparisons */
    case OP_CMP_EQ_INT:
        return dis_simple("CMP_EQ_INT", offset);
    case OP_CMP_NEQ_INT:
        return dis_simple("CMP_NEQ_INT", offset);
    case OP_CMP_LT_INT:
        return dis_simple("CMP_LT_INT", offset);
    case OP_CMP_LTE_INT:
        return dis_simple("CMP_LTE_INT", offset);
    case OP_CMP_GT_INT:
        return dis_simple("CMP_GT_INT", offset);
    case OP_CMP_GTE_INT:
        return dis_simple("CMP_GTE_INT", offset);

    /* Float comparisons */
    case OP_CMP_EQ_FLOAT:
        return dis_simple("CMP_EQ_FLOAT", offset);
    case OP_CMP_NEQ_FLOAT:
        return dis_simple("CMP_NEQ_FLOAT", offset);
    case OP_CMP_LT_FLOAT:
        return dis_simple("CMP_LT_FLOAT", offset);
    case OP_CMP_LTE_FLOAT:
        return dis_simple("CMP_LTE_FLOAT", offset);
    case OP_CMP_GT_FLOAT:
        return dis_simple("CMP_GT_FLOAT", offset);
    case OP_CMP_GTE_FLOAT:
        return dis_simple("CMP_GTE_FLOAT", offset);

    /* Bool / string equality */
    case OP_CMP_EQ_BOOL:
        return dis_simple("CMP_EQ_BOOL", offset);
    case OP_CMP_NEQ_BOOL:
        return dis_simple("CMP_NEQ_BOOL", offset);
    case OP_CMP_EQ_STR:
        return dis_simple("CMP_EQ_STR", offset);
    case OP_CMP_NEQ_STR:
        return dis_simple("CMP_NEQ_STR", offset);
    case OP_CMP_EQ_VAL:
        return dis_simple("CMP_EQ_VAL", offset);
    case OP_CMP_NEQ_VAL:
        return dis_simple("CMP_NEQ_VAL", offset);

    /* Exception handling */
    case OP_TRY_BEGIN:
        return dis_jump("TRY_BEGIN", chunk, offset);
    case OP_TRY_END:
        return dis_simple("TRY_END", offset);
    case OP_THROW:
        return dis_simple("THROW", offset);
    case OP_LOAD_EXCEPTION:
        return dis_simple("LOAD_EXCEPTION", offset);
    case OP_EXCEPTION_IS_TYPE: {
        /* [u8 name_len][name_bytes] */
        uint8_t nlen = chunk->code[offset + 1];
        printf("%-20s '", "EXCEPTION_IS_TYPE");
        for (int _i = 0; _i < nlen && offset + 2 + _i < chunk->count; _i++)
            putchar(chunk->code[offset + 2 + _i]);
        printf("'\n");
        return offset + 2 + nlen;
    }

    case OP_EVENT_SUBSCRIBE:
    case OP_EVENT_UNSUBSCRIBE:
    case OP_EVENT_SUBSCRIBE_BOUND:
    case OP_EVENT_UNSUBSCRIBE_BOUND:
    case OP_EVENT_FIRE: {
        const char *label = (op == OP_EVENT_SUBSCRIBE)         ? "EVENT_SUBSCRIBE"
                          : (op == OP_EVENT_UNSUBSCRIBE)       ? "EVENT_UNSUBSCRIBE"
                          : (op == OP_EVENT_SUBSCRIBE_BOUND)   ? "EVENT_SUBSCRIBE_BOUND"
                          : (op == OP_EVENT_UNSUBSCRIBE_BOUND) ? "EVENT_UNSUBSCRIBE_BOUND"
                                                               : "EVENT_FIRE";
        uint8_t nlen = chunk->code[offset + 1];
        printf("%-24s '", label);
        for (int _i = 0; _i < nlen && offset + 2 + _i < chunk->count; _i++)
            putchar(chunk->code[offset + 2 + _i]);
        if (op == OP_EVENT_FIRE) {
            uint8_t argc = chunk->code[offset + 2 + nlen];
            printf("' argc=%d\n", argc);
            return offset + 3 + nlen;
        } else {
            /* handler name follows event name */
            uint8_t hnlen = chunk->code[offset + 2 + nlen];
            printf("' handler='");
            for (int _i = 0; _i < hnlen && offset + 3 + nlen + _i < chunk->count; _i++)
                putchar(chunk->code[offset + 3 + nlen + _i]);
            printf("'\n");
            return offset + 3 + nlen + hnlen;
        }
    }

    /* Stack */
    case OP_POP:
        return dis_simple("POP", offset);

    /* Control flow */
    case OP_JUMP:
        return dis_jump("JUMP", chunk, offset);
    case OP_JUMP_IF_FALSE:
        return dis_jump("JUMP_IF_FALSE", chunk, offset);
    case OP_JUMP_IF_TRUE:
        return dis_jump("JUMP_IF_TRUE", chunk, offset);

    /* Calls */
    case OP_CALL:
        return dis_call("CALL", chunk, offset);
    case OP_RETURN:
        return dis_simple("RETURN", offset);
    case OP_RETURN_VOID:
        return dis_simple("RETURN_VOID", offset);
    case OP_MATCH_FAIL:
        return dis_simple("MATCH_FAIL", offset);
    case OP_TRUNC_I8:
        return dis_simple("TRUNC_I8", offset);
    case OP_TRUNC_U8:
        return dis_simple("TRUNC_U8", offset);
    case OP_TRUNC_I16:
        return dis_simple("TRUNC_I16", offset);
    case OP_TRUNC_U16:
        return dis_simple("TRUNC_U16", offset);
    case OP_TRUNC_I32:
        return dis_simple("TRUNC_I32", offset);
    case OP_TRUNC_U32:
        return dis_simple("TRUNC_U32", offset);
    case OP_TRUNC_U64:
        return dis_simple("TRUNC_U64", offset);
    case OP_TRUNC_CHAR:
        return dis_simple("TRUNC_CHAR", offset);
    case OP_NEW_ARRAY:
        return dis_byte("NEW_ARRAY", chunk, offset);
    case OP_ARRAY_LIT:
        return dis_byte("ARRAY_LIT", chunk, offset);
    case OP_ARRAY_GET:
        return dis_simple("ARRAY_GET", offset);
    case OP_ARRAY_SET:
        return dis_simple("ARRAY_SET", offset);
    case OP_ARRAY_LEN:
        return dis_simple("ARRAY_LEN", offset);
    case OP_CALL_HOST:
        return dis_call("CALL_HOST", chunk, offset);

    /* Object opcodes */
    case OP_NEW:
    {
        uint16_t class_idx = (chunk->code[offset + 1] << 8) | chunk->code[offset + 2];
        uint8_t argc = chunk->code[offset + 3];
        printf("%-20s class[%d]  argc=%d\n", "NEW", class_idx, argc);
        /* skip: opcode(1) + class_idx(2) + argc(1) + tac(1) + tac×kind(N) */
        int base = offset + 4;
        if (base < chunk->count) {
            uint8_t tac = chunk->code[base];
            return base + 1 + tac;
        }
        return base;
    }
    case OP_GET_FIELD:
    {
        uint8_t field_idx = chunk->code[offset + 1];
        printf("%-20s field[%d]\n", "GET_FIELD", field_idx);
        return offset + 2;
    }
    case OP_SET_FIELD:
    {
        uint8_t field_idx = chunk->code[offset + 1];
        printf("%-20s field[%d]\n", "SET_FIELD", field_idx);
        return offset + 2;
    }
    case OP_LOAD_THIS:
        return dis_simple("LOAD_THIS", offset);
    case OP_CALL_METHOD:
        return dis_call("CALL_METHOD", chunk, offset);
    case OP_CALL_SUPER:
        return dis_call("CALL_SUPER", chunk, offset);
    case OP_CALL_IFACE:
        return dis_call("CALL_IFACE", chunk, offset);
    case OP_LOAD_STATIC:
    {
        uint8_t class_idx = chunk->code[offset + 1];
        uint8_t field_idx = chunk->code[offset + 2];
        printf("%-20s class[%d].field[%d]\n", "LOAD_STATIC", class_idx, field_idx);
        return offset + 3;
    }
    case OP_STORE_STATIC:
    {
        uint8_t class_idx = chunk->code[offset + 1];
        uint8_t field_idx = chunk->code[offset + 2];
        printf("%-20s class[%d].field[%d]\n", "STORE_STATIC", class_idx, field_idx);
        return offset + 3;
    }
    case OP_TYPEOF:
    {
        uint8_t tag = chunk->code[offset + 1];
        uint8_t len = chunk->code[offset + 2];
        printf("%04d TYPEOF tag=%d name_len=%d\n", offset, tag, len);
        return offset + 3 + len;
    }
    case OP_AS_TYPE:
        printf("%04d AS_TYPE %d\n", offset, chunk->code[offset + 1]);
        return offset + 2;
    case OP_IS_TYPE:
        printf("%04d IS_TYPE\n", offset);
        return offset + 1;
    case OP_TYPE_FIELD:
    {
        uint8_t field = chunk->code[offset + 1];
        const char *fname = NULL;
        switch (field)
        {
        case 0:
            fname = "name";
            break;
        case 1:
            fname = "isArray";
            break;
        case 2:
            fname = "isPrimitive";
            break;
        case 3:
            fname = "isEnum";
            break;
        case 4:
            fname = "isClass";
            break;
        default:
            fname = "unknown";
            break;
        }
        printf("%04d TYPE_FIELD %d (%s)\n", offset, field, fname);
        return offset + 2;
    }
    case OP_TYPE_HAS_ATTR:
        return dis_simple("TYPE_HAS_ATTR", offset);
    case OP_TYPE_GET_ATTR_ARG:
        return dis_simple("TYPE_GET_ATTR_ARG", offset);
    case OP_PUSH_NULL:
        return dis_simple("PUSH_NULL", offset);
    case OP_IS_NULL:
        return dis_simple("IS_NULL", offset);
    case OP_NULL_ASSERT:
        return dis_jump("NULL_ASSERT", chunk, offset);
    case OP_NULL_COALESCE:
        return dis_jump("NULL_COALESCE", chunk, offset);
    default:
        printf("UNKNOWN opcode %d\n", op);
        return offset + 1;
    }
}

void chunk_disassemble(const Chunk *chunk, const char *name)
{
    printf("\n=== disassembly: %s ===\n", name);
    printf("locals: %d  params: %d  constants: %d  bytes: %d\n\n",
           chunk->local_count, chunk->param_count,
           chunk->constants.count, chunk->count);

    int offset = 0;
    while (offset < chunk->count)
    {
        offset = disassemble_instruction(chunk, offset);
    }
    printf("=== end: %s ===\n", name);
}

/* ─────────────────────────────────────────────────────────────────────────────
 * OPCODE NAMES
 * ───────────────────────────────────────────────────────────────────────────*/

const char *opcode_name(OpCode op)
{
    switch (op)
    {
    case OP_LOAD_CONST_INT:
        return "LOAD_CONST_INT";
    case OP_LOAD_CONST_FLOAT:
        return "LOAD_CONST_FLOAT";
    case OP_LOAD_CONST_BOOL:
        return "LOAD_CONST_BOOL";
    case OP_LOAD_CONST_STR:
        return "LOAD_CONST_STR";
    case OP_LOAD_LOCAL:
        return "LOAD_LOCAL";
    case OP_STORE_LOCAL:
        return "STORE_LOCAL";
    case OP_ADD_INT:
        return "ADD_INT";
    case OP_SUB_INT:
        return "SUB_INT";
    case OP_MUL_INT:
        return "MUL_INT";
    case OP_DIV_INT:
        return "DIV_INT";
    case OP_MOD_INT:
        return "MOD_INT";
    case OP_NEGATE_INT:
        return "NEGATE_INT";
    case OP_ADD_FLOAT:
        return "ADD_FLOAT";
    case OP_SUB_FLOAT:
        return "SUB_FLOAT";
    case OP_MUL_FLOAT:
        return "MUL_FLOAT";
    case OP_DIV_FLOAT:
        return "DIV_FLOAT";
    case OP_MOD_FLOAT:
        return "MOD_FLOAT";
    case OP_NEGATE_FLOAT:
        return "NEGATE_FLOAT";
    case OP_CONCAT_STR:
        return "CONCAT_STR";
    case OP_TO_STR:
        return "TO_STR";
    case OP_NOT_BOOL:
        return "NOT_BOOL";
    case OP_AND_BOOL:
        return "AND_BOOL";
    case OP_OR_BOOL:
        return "OR_BOOL";
    case OP_CMP_EQ_INT:
        return "CMP_EQ_INT";
    case OP_CMP_NEQ_INT:
        return "CMP_NEQ_INT";
    case OP_CMP_LT_INT:
        return "CMP_LT_INT";
    case OP_CMP_LTE_INT:
        return "CMP_LTE_INT";
    case OP_CMP_GT_INT:
        return "CMP_GT_INT";
    case OP_CMP_GTE_INT:
        return "CMP_GTE_INT";
    case OP_CMP_EQ_FLOAT:
        return "CMP_EQ_FLOAT";
    case OP_CMP_NEQ_FLOAT:
        return "CMP_NEQ_FLOAT";
    case OP_CMP_LT_FLOAT:
        return "CMP_LT_FLOAT";
    case OP_CMP_LTE_FLOAT:
        return "CMP_LTE_FLOAT";
    case OP_CMP_GT_FLOAT:
        return "CMP_GT_FLOAT";
    case OP_CMP_GTE_FLOAT:
        return "CMP_GTE_FLOAT";
    case OP_CMP_EQ_BOOL:
        return "CMP_EQ_BOOL";
    case OP_CMP_NEQ_BOOL:
        return "CMP_NEQ_BOOL";
    case OP_CMP_EQ_STR:
        return "CMP_EQ_STR";
    case OP_CMP_NEQ_STR:
        return "CMP_NEQ_STR";
    case OP_POP:
        return "POP";
    case OP_JUMP:
        return "JUMP";
    case OP_JUMP_IF_FALSE:
        return "JUMP_IF_FALSE";
    case OP_JUMP_IF_TRUE:
        return "JUMP_IF_TRUE";
    case OP_CALL:
        return "CALL";
    case OP_RETURN:
        return "RETURN";
    case OP_RETURN_VOID:
        return "RETURN_VOID";
    case OP_MATCH_FAIL:
        return "MATCH_FAIL";
    case OP_CALL_HOST:
        return "CALL_HOST";
    case OP_NEW:
        return "NEW";
    case OP_GET_FIELD:
        return "GET_FIELD";
    case OP_SET_FIELD:
        return "SET_FIELD";
    case OP_LOAD_THIS:
        return "LOAD_THIS";
    case OP_CALL_METHOD:
        return "CALL_METHOD";
    case OP_CALL_SUPER:
        return "CALL_SUPER";
    case OP_CALL_IFACE:
        return "CALL_IFACE";
    case OP_LOAD_STATIC:
        return "LOAD_STATIC";
    case OP_STORE_STATIC:
        return "STORE_STATIC";
    case OP_PUSH_NULL:
        return "PUSH_NULL";
    case OP_IS_NULL:
        return "IS_NULL";
    case OP_NULL_ASSERT:
        return "NULL_ASSERT";
    case OP_NULL_COALESCE:
        return "NULL_COALESCE";
    case OP_EVENT_SUBSCRIBE:
        return "EVENT_SUBSCRIBE";
    case OP_EVENT_UNSUBSCRIBE:
        return "EVENT_UNSUBSCRIBE";
    case OP_EVENT_FIRE:
        return "EVENT_FIRE";
    case OP_EVENT_SUBSCRIBE_BOUND:
        return "EVENT_SUBSCRIBE_BOUND";
    case OP_EVENT_UNSUBSCRIBE_BOUND:
        return "EVENT_UNSUBSCRIBE_BOUND";
    case OP_EVENT_SUBSCRIBE_MEMBER:
        return "EVENT_SUBSCRIBE_MEMBER";
    case OP_EVENT_UNSUBSCRIBE_MEMBER:
        return "EVENT_UNSUBSCRIBE_MEMBER";
    case OP_EVENT_FIRE_MEMBER:
        return "EVENT_FIRE_MEMBER";
    default:
        return "UNKNOWN";
    }
}