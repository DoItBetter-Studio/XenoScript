/*
 * vm.c — XenoScript VM implementation
 *
 * The heart of the system. Everything above this (lexer, parser, type checker,
 * compiler) exists to produce the bytecode that this file executes.
 *
 * The main loop in xeno_execute() is a classic fetch-decode-execute cycle:
 *   1. READ the opcode byte at ip, advance ip
 *   2. DECODE: switch on the opcode
 *   3. EXECUTE: perform the operation (pop operands, push result)
 *   4. Repeat
 *
 * Performance note: for a mod scripting VM, raw throughput is not critical.
 * Correctness and simplicity are. A computed goto dispatch table (used in
 * production VMs like CPython and LuaJIT) would be faster but is a GCC
 * extension. The switch statement is standard C17 and plenty fast for
 * scripting workloads.
 */

#include "vm.h"
#include "xar.h"
#include "xbc.h"
#include "stdlib_xar.h"
#include "lexer.h"
#include "parser.h"
#include "checker.h"
#include "../../source/compiler/compile_pipeline.h"

/* Forward declaration — xeno_execute is defined later in this file */
static XenoResult xeno_execute(XenoVM *vm);
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>

/* ─────────────────────────────────────────────────────────────────────────────
 * INTERNAL MACROS
 *
 * These make the main loop readable. They're macros rather than functions
 * so they have direct access to the local `frame` and `vm` variables in
 * xeno_execute(), which keeps the loop tight.
 * ───────────────────────────────────────────────────────────────────────────*/

/* Read one byte from the instruction stream and advance ip */
#define READ_BYTE() (*frame->ip++)

/* Read a uint16_t operand (big-endian, two bytes) */
#define READ_U16() (frame->ip += 2, \
                    (uint16_t)((frame->ip[-2] << 8) | frame->ip[-1]))

/* Read a signed int16_t (for jump offsets) */
#define READ_I16() ((int16_t)READ_U16())

/* Push a value onto the value stack */
#define PUSH(val) (*vm->sp++ = (val))

/* Pop a value from the value stack */
#define POP() (*--vm->sp)
#define DISCARD() (vm->sp--)

/* Peek at the top of the stack without popping */
#define PEEK() (*(vm->sp - 1))

/* Binary operation macro — pops two, pushes result.
 * `field` is the Value union field (i, f, b).
 * `op`    is the C operator to apply.
 * `rtype` is the result constructor (val_int, val_float, val_bool). */
#define BINARY_OP(field, op, rtype)      \
    do                                   \
    {                                    \
        Value b = POP();                 \
        Value a = POP();                 \
        PUSH(rtype(a.field op b.field)); \
    } while (0)

/* Runtime error — set message and return error code */
#define RUNTIME_ERROR(fmt, ...)                \
    do                                         \
    {                                          \
        xeno_vm_error(vm, fmt, ##__VA_ARGS__); \
        return XENO_RUNTIME_ERROR;             \
    } while (0)

/* ─────────────────────────────────────────────────────────────────────────────
 * VM LIFECYCLE
 * ───────────────────────────────────────────────────────────────────────────*/

void xeno_vm_init(XenoVM *vm)
{
    memset(vm, 0, sizeof(XenoVM));
    vm->sp = vm->stack; /* Stack pointer starts at bottom       */
    vm->frame_count = 0;
    vm->had_error = false;
    vm->has_source_module = false;
}

void xeno_vm_free(XenoVM *vm)
{
    if (vm->has_source_module)
    {
        module_free(&vm->source_module);
        vm->has_source_module = false;
    }
    for (int i = 0; i < vm->stdlib_module_count; i++)
    {
        module_free(vm->stdlib_modules[i]);
        free(vm->stdlib_modules[i]);
        vm->stdlib_modules[i] = NULL;
    }
    vm->stdlib_module_count = 0;
}

/* ─────────────────────────────────────────────────────────────────────────────
 * STDLIB XAR LOADING
 * ───────────────────────────────────────────────────────────────────────────*/

static bool stdlib_already_loaded(XenoVM *vm, const char *name)
{
    for (int i = 0; i < vm->stdlib_module_count; i++)
        if (strcmp(vm->stdlib_loaded_names[i], name) == 0)
            return true;
    return false;
}

static bool load_xar_into_pool(XenoVM *vm, const uint8_t *data, size_t size,
                               const char *name)
{
    if (vm->stdlib_module_count >= 64)
        return false;

    XarArchive ar;
    if (xar_read_mem(&ar, data, size) != XAR_OK)
    {
        fprintf(stderr, "vm: failed to read embedded .xar '%s'\n", name);
        return false;
    }

    /* Each chunk in the archive is a compiled .xbc — merge all into one module */
    Module *pool_mod = (Module *)calloc(1, sizeof(Module));
    vm->stdlib_modules[vm->stdlib_module_count] = pool_mod;
    module_init(pool_mod);

    for (int i = 0; i < ar.chunk_count; i++)
    {
        Module *chunk_mod = (Module *)calloc(1, sizeof(Module));
        module_init(chunk_mod);
        XbcResult xr = xbc_read_mem(chunk_mod,
                                    ar.chunks[i].data,
                                    ar.chunks[i].size);
        if (xr == XBC_OK)
        {
            module_merge(pool_mod, chunk_mod);
            /* Run sinit for this chunk so statics are initialised */
            if (chunk_mod->sinit_index >= 0)
            {
                vm->module = pool_mod;
                Chunk *sc = &chunk_mod->chunks[chunk_mod->sinit_index];
                if (sc->count > 0 && vm->frame_count < XENO_FRAME_MAX)
                {
                    CallFrame *frame = &vm->frames[vm->frame_count++];
                    frame->chunk = sc;
                    frame->ip = sc->code;
                    memset(frame->slots, 0, sizeof(frame->slots));
                    frame->type_arg_count = 0;
                    xeno_execute(vm);
                }
            }
            module_free(chunk_mod);
            free(chunk_mod);
        }
    }
    xar_archive_free(&ar);

    strncpy(vm->stdlib_loaded_names[vm->stdlib_module_count], name,
            XAR_MAX_NAME - 1);
    vm->stdlib_module_count++;
    return true;
}

bool xeno_vm_load_stdlib_module(XenoVM *vm, const char *name)
{
    if (stdlib_already_loaded(vm, name))
        return true;
    for (int i = 0; i < STDLIB_XAR_TOTAL_COUNT; i++)
    {
        if (strcmp(STDLIB_XAR_TABLE[i].name, name) == 0)
        {
            size_t size = (size_t)(STDLIB_XAR_TABLE[i].end -
                                   STDLIB_XAR_TABLE[i].start);
            return load_xar_into_pool(vm, STDLIB_XAR_TABLE[i].start,
                                      size, name);
        }
    }
    return false; /* not found in embedded table */
}

void xeno_vm_load_stdlib(XenoVM *vm)
{
    /* Load all embedded stdlib modules into the pool so the VM can fill in
     * external stubs at runtime.  This covers both the always-loaded modules
     * (core primitive extensions) and opt-in ones (math, collections, …).
     * The cost is a one-time XBC deserialise; the bytecode is small and the
     * pool is keyed by name so duplicates are skipped automatically. */
    for (int i = 0; i < STDLIB_XAR_TOTAL_COUNT; i++)
    {
        size_t size = (size_t)(STDLIB_XAR_TABLE[i].end -
                               STDLIB_XAR_TABLE[i].start);
        load_xar_into_pool(vm, STDLIB_XAR_TABLE[i].start,
                           size, STDLIB_XAR_TABLE[i].name);
    }
}

bool xeno_vm_load_xar(XenoVM *vm, const XarArchive *ar)
{
    const char *mod_name = ar->manifest.name[0]
                               ? ar->manifest.name
                               : "<unnamed>";

    /* Deduplicate — don't load the same mod twice */
    for (int i = 0; i < vm->stdlib_module_count; i++)
    {
        if (strcmp(vm->stdlib_loaded_names[i], mod_name) == 0)
            return true;
    }

    if (vm->stdlib_module_count >= 64)
    {
        fprintf(stderr, "vm: too many loaded modules\n");
        return false;
    }

    /* Merge all chunks from the archive into a single pool module */
    Module *pool_mod = (Module *)calloc(1, sizeof(Module));
    vm->stdlib_modules[vm->stdlib_module_count] = pool_mod;
    module_init(pool_mod);

    for (int i = 0; i < ar->chunk_count; i++)
    {
        Module *chunk_mod = (Module *)calloc(1, sizeof(Module));
        module_init(chunk_mod);
        XbcResult xr = xbc_read_mem(chunk_mod,
                                    ar->chunks[i].data,
                                    ar->chunks[i].size);
        if (xr == XBC_OK)
        {
            module_merge(pool_mod, chunk_mod);
            /* Run __sinit__ if present so static fields are initialised */
            if (chunk_mod->sinit_index >= 0)
            {
                vm->module = pool_mod;
                Chunk *sc = &chunk_mod->chunks[chunk_mod->sinit_index];
                if (sc->count > 0 && vm->frame_count < XENO_FRAME_MAX)
                {
                    CallFrame *frame = &vm->frames[vm->frame_count++];
                    frame->chunk = sc;
                    frame->ip = sc->code;
                    memset(frame->slots, 0, sizeof(frame->slots));
                    frame->type_arg_count = 0;
                    xeno_execute(vm);
                }
            }
            module_free(chunk_mod);
            free(chunk_mod);
        }
    }

    snprintf(vm->stdlib_loaded_names[vm->stdlib_module_count],
             XAR_MAX_NAME, "%s", mod_name);
    vm->stdlib_module_count++;
    return true;
}

XenoResult xeno_vm_run_mod(XenoVM *vm, const char *mod_name)
{
    /* Find the named mod in the stdlib pool (loaded via xeno_vm_load_xar) */
    Module *mod_module = NULL;
    for (int i = 0; i < vm->stdlib_module_count; i++)
    {
        if (strcmp(vm->stdlib_loaded_names[i], mod_name) == 0)
        {
            mod_module = vm->stdlib_modules[i];
            break;
        }
    }
    if (!mod_module)
    {
        xeno_vm_error(vm, "xeno_vm_run_mod: mod '%s' not loaded", mod_name);
        return XENO_RUNTIME_ERROR;
    }
    /* Run the mod's module directly — xeno_vm_run will merge stdlib into it */
    return xeno_vm_run(vm, mod_module);
}

void xeno_vm_set_mod_path(XenoVM *vm, const char *path)
{
    strncpy(vm->mod_path, path, sizeof(vm->mod_path) - 1);
}

/* ─────────────────────────────────────────────────────────────────────────────
 * ERROR HANDLING
 * ───────────────────────────────────────────────────────────────────────────*/

void xeno_vm_error(XenoVM *vm, const char *fmt, ...)
{
    va_list args;
    va_start(args, fmt);
    vsnprintf(vm->error, sizeof(vm->error), fmt, args);
    va_end(args);
    vm->had_error = true;
}

void xeno_vm_print_error(const XenoVM *vm)
{
    printf("[XenoScript Error] %s\n", vm->error);
}

/* ─────────────────────────────────────────────────────────────────────────────
 * HOST FUNCTION REGISTRY
 * ───────────────────────────────────────────────────────────────────────────*/

int xeno_register_fn_typed(XenoVM *vm, const char *name, XenoHostFn fn,
                           int return_kind, int param_count, int *param_kinds)
{
    if (vm->host_fn_count >= XENO_HOST_FN_MAX)
        return -1;

    HostFnEntry *entry = &vm->host_fns[vm->host_fn_count];
    entry->fn = fn;
    entry->param_count = param_count;
    entry->return_type_kind = return_kind;
    strncpy(entry->name, name, sizeof(entry->name) - 1);
    entry->name[sizeof(entry->name) - 1] = '\0';

    int pcount = param_count < 0 ? 0 : param_count;
    for (int i = 0; i < pcount && i < 16; i++)
        entry->param_type_kinds[i] = param_kinds ? param_kinds[i] : TYPE_INT;

    return vm->host_fn_count++;
}

int xeno_register_fn(XenoVM *vm, const char *name,
                     XenoHostFn fn, int param_count)
{
    return xeno_register_fn_typed(vm, name, fn, TYPE_VOID, param_count, NULL);
}

/* ─────────────────────────────────────────────────────────────────────────────
 * STACK HELPERS
 * ───────────────────────────────────────────────────────────────────────────*/

static bool stack_overflow(XenoVM *vm)
{
    return vm->sp >= vm->stack + XENO_STACK_MAX;
}

/* ─────────────────────────────────────────────────────────────────────────────
 * THE MAIN EXECUTION LOOP
 *
 * This function runs until the top-level function returns or an error occurs.
 * It is called once per vm_run() with the entry-point frame already set up.
 * ───────────────────────────────────────────────────────────────────────────*/

/* ─────────────────────────────────────────────────────────────────────────────
 * TYPE OPERATOR HELPERS
 *
 * xeno_make_type — allocate and populate a XenoType descriptor.
 * The type_tag byte matches the TypeKind enum from ast.h.
 * ───────────────────────────────────────────────────────────────────────────*/

/* type_tag values (must match TypeKind in ast.h) */
#define XTAG_UNKNOWN 0
#define XTAG_VOID 1
#define XTAG_BOOL 2
#define XTAG_INT 3
#define XTAG_FLOAT 4
#define XTAG_STRING 5
#define XTAG_OBJECT 6
#define XTAG_ENUM 7
#define XTAG_CLASS_REF 8
#define XTAG_SBYTE 9
#define XTAG_BYTE 10
#define XTAG_SHORT 11
#define XTAG_USHORT 12
#define XTAG_UINT 13
#define XTAG_LONG 14
#define XTAG_ULONG 15
#define XTAG_DOUBLE 16
#define XTAG_CHAR 17
#define XTAG_ANY 18
#define XTAG_ARRAY 19

static const char *xeno_type_name(uint8_t tag)
{
    switch (tag)
    {
    case XTAG_BOOL:
        return "bool";
    case XTAG_INT:
        return "int";
    case XTAG_FLOAT:
        return "float";
    case XTAG_STRING:
        return "string";
    case XTAG_SBYTE:
        return "sbyte";
    case XTAG_BYTE:
        return "byte";
    case XTAG_SHORT:
        return "short";
    case XTAG_USHORT:
        return "ushort";
    case XTAG_UINT:
        return "uint";
    case XTAG_LONG:
        return "long";
    case XTAG_ULONG:
        return "ulong";
    case XTAG_DOUBLE:
        return "double";
    case XTAG_CHAR:
        return "char";
    case XTAG_ENUM:
        return "enum";
    case XTAG_ARRAY:
        return "array";
    case XTAG_VOID:
        return "void";
    default:
        return "unknown";
    }
}

static bool xeno_type_is_primitive(uint8_t tag)
{
    switch (tag)
    {
    case XTAG_BOOL:
    case XTAG_INT:
    case XTAG_FLOAT:
    case XTAG_DOUBLE:
    case XTAG_SBYTE:
    case XTAG_BYTE:
    case XTAG_SHORT:
    case XTAG_USHORT:
    case XTAG_UINT:
    case XTAG_LONG:
    case XTAG_ULONG:
    case XTAG_CHAR:
    case XTAG_STRING:
        return true;
    default:
        return false;
    }
}

/* Build a XenoType object. Name is a static string or a malloc'd one.
 * For OBJECT/CLASS we pass a class_name; for ARRAY we pass element tag.
 * tag2 is used for arrays (element type tag) and objects (ignored). */
static XenoType *xeno_make_type(uint8_t tag, const char *override_name,
                                const Module *module)
{
    XenoType *t = malloc(sizeof(XenoType));
    if (!t)
        return NULL;
    t->name = override_name ? override_name : xeno_type_name(tag);
    t->is_array = (tag == XTAG_ARRAY);
    t->is_primitive = xeno_type_is_primitive(tag);
    t->is_enum = (tag == XTAG_ENUM);
    t->is_class = (tag == XTAG_OBJECT || tag == XTAG_CLASS_REF);
    t->class_def = NULL;
    /* For class/enum types, look up the ClassDef so attribute reflection works */
    if (t->is_class || t->is_enum)
    {
        if (module && t->name)
        {
            for (int _ci = 0; _ci < module->class_count; _ci++)
            {
                if (strcmp(module->classes[_ci].name, t->name) == 0)
                {
                    t->class_def = &module->classes[_ci];
                    break;
                }
            }
        }
    }
    return t;
}

/* Check whether a Value matches a given type tag at runtime.
 * For IS/AS we use the static type tag baked in by the compiler — this is
 * a compile-time type that we verify is sensible, not a full RTTI check.
 * For objects/arrays we do a quick structural check. */
static bool xeno_value_is_type(Value v, uint8_t tag)
{
    switch (tag)
    {
    case XTAG_BOOL:
        return true; /* bools are always bool */
    case XTAG_INT:
    case XTAG_SBYTE:
    case XTAG_BYTE:
    case XTAG_SHORT:
    case XTAG_USHORT:
    case XTAG_UINT:
    case XTAG_LONG:
    case XTAG_ULONG:
        return true; /* int-family */
    case XTAG_FLOAT:
    case XTAG_DOUBLE:
        return true; /* float-family */
    case XTAG_STRING:
        return v.s != NULL;
    case XTAG_ARRAY:
        return v.arr != NULL;
    case XTAG_OBJECT:
    case XTAG_CLASS_REF:
        return v.obj != NULL;
    default:
        return false;
    }
}

static char *int128_to_string(__int128 v) {
    char buf[64];
    int i = 63;
    buf[i] = '\0';

    int negative = v < 0;
    if (negative) v = -v;

    do {
        buf[--i] = '0' + (v % 10);
        v /= 10;
    } while (v);

    if (negative)
        buf[--i] = '-';

    char *s = malloc(64 - i);
    strcpy(s, &buf[i]);
    return s;
}

static char *uint128_to_string(__int128 v) {
    char buf[64];
    int i = 63;
    buf[i] = '\0';

    unsigned __int128 uv = (unsigned __int128)v;

    if (uv == 0) {
        buf[--i] = '0';
    } else {
        do {
            buf[--i] = '0' + (uv % 10);
            uv /= 10;
        } while (uv);
    }

    char *s = malloc(64 - i);
    strcpy(s, &buf[i]);
    return s;
}

static XenoResult xeno_execute(XenoVM *vm)
{

    /* The current call frame — updated on every call/return */
    CallFrame *frame = &vm->frames[vm->frame_count - 1];

    for (;;)
    {
    dispatch:;

        uint8_t instruction = READ_BYTE();

        /* ── DECODE + EXECUTE ─────────────────────────────────────────── */
        switch ((OpCode)instruction)
        {

        /* ── Constants ──────────────────────────────────────────── */
        case OP_LOAD_CONST_INT:
        {
            uint16_t idx = READ_U16();
            if (stack_overflow(vm))
                RUNTIME_ERROR("Stack overflow");
            PUSH(frame->chunk->constants.values[idx]);
            break;
        }

        case OP_LOAD_CONST_FLOAT:
        {
            uint16_t idx = READ_U16();
            if (stack_overflow(vm))
                RUNTIME_ERROR("Stack overflow");
            PUSH(frame->chunk->constants.values[idx]);
            break;
        }

        case OP_LOAD_CONST_BOOL:
        {
            uint8_t val = READ_BYTE();
            if (stack_overflow(vm))
                RUNTIME_ERROR("Stack overflow");
            PUSH(val_bool(val != 0));
            break;
        }

        case OP_LOAD_CONST_STR:
        {
            uint16_t idx = READ_U16();
            if (stack_overflow(vm))
                RUNTIME_ERROR("Stack overflow");
            PUSH(frame->chunk->constants.values[idx]);
            break;
        }

        /* ── Local variables ────────────────────────────────────── */
        case OP_LOAD_LOCAL:
        {
            uint8_t slot = READ_BYTE();
            if (stack_overflow(vm))
                RUNTIME_ERROR("Stack overflow");
            PUSH(frame->slots[slot]);
            break;
        }

        case OP_STORE_LOCAL:
        {
            uint8_t slot = READ_BYTE();
            frame->slots[slot] = POP();
            break;
        }

        /* ── Integer arithmetic ─────────────────────────────────── */
        case OP_ADD_INT:
            BINARY_OP(i, +, val_int);
            break;
        case OP_SUB_INT:
            BINARY_OP(i, -, val_int);
            break;
        case OP_MUL_INT:
            BINARY_OP(i, *, val_int);
            break;
        case OP_MOD_INT:
        {
            Value b = POP();
            Value a = POP();
            if (b.i == 0)
                RUNTIME_ERROR("Modulo by zero");
            PUSH(val_int(a.i % b.i));
            break;
        }
        case OP_DIV_INT:
        {
            Value b = POP();
            Value a = POP();
            if (b.i == 0)
                RUNTIME_ERROR("Division by zero");
            PUSH(val_int(a.i / b.i));
            break;
        }
        case OP_NEGATE_INT:
        {
            Value a = POP();
            PUSH(val_int(-a.i));
            break;
        }

        /* ── Float arithmetic ───────────────────────────────────── */
        case OP_ADD_FLOAT:
            BINARY_OP(f, +, val_float);
            break;
        case OP_SUB_FLOAT:
            BINARY_OP(f, -, val_float);
            break;
        case OP_MUL_FLOAT:
            BINARY_OP(f, *, val_float);
            break;
        case OP_DIV_FLOAT:
            BINARY_OP(f, /, val_float);
            break;
        case OP_MOD_FLOAT:
        {
            /* C doesn't have % for floats — use fmod */
            Value b = POP();
            Value a = POP();
            extern double fmod(double, double);
            PUSH(val_float(fmod(a.f, b.f)));
            break;
        }
        case OP_NEGATE_FLOAT:
        {
            Value a = POP();
            PUSH(val_float(-a.f));
            break;
        }

        /* ── String concatenation ───────────────────────────────── */
        case OP_CONCAT_STR:
        {
            Value b = POP();
            Value a = POP();
            /* Allocate a new string for the result.
             * In the full VM this would go through a string intern table.
             * For now: simple malloc. This leaks in long-running scripts
             * — marked as a known TODO for the string GC pass. */
            size_t la = a.s ? strlen(a.s) : 0;
            size_t lb = b.s ? strlen(b.s) : 0;
            char *result = malloc(la + lb + 1);
            if (a.s)
                memcpy(result, a.s, la);
            if (b.s)
                memcpy(result + la, b.s, lb);
            result[la + lb] = '\0';
            PUSH(val_str(result));
            break;
        }

        case OP_TO_STR:
        {
            /* Convert the top-of-stack value to its string representation.
             * This is the only opcode that inspects value type at runtime —
             * used exclusively for string concatenation with non-string types.
             * The value's type is encoded in the instruction by the compiler
             * via the operand byte: 0=int, 1=float, 2=bool, 3=string (no-op). */
            uint8_t kind = READ_BYTE();
            /* kind=4 (enum) has an extra class_index byte */
            uint8_t enum_class_idx = (kind == 4) ? READ_BYTE() : 0;
            Value v = POP();
            char buf[64];
            char *s;
            switch (kind)
            {
            case 4:
            { /* enum — look up member name by value */
                int idx = (int)v.i;
                ClassDef *ecls = NULL;
                if (enum_class_idx < 0xFF && (int)enum_class_idx < vm->module->class_count)
                {
                    ecls = &vm->module->classes[enum_class_idx];
                    if (!ecls->is_enum)
                        ecls = NULL;
                }
                /* Search by value (not position) to handle explicit values */
                const char *found_name = NULL;
                if (ecls)
                {
                    for (int mi = 0; mi < ecls->enum_member_count; mi++)
                    {
                        if (ecls->enum_member_values[mi] == idx)
                        {
                            found_name = ecls->enum_member_names[mi];
                            break;
                        }
                    }
                }
                if (found_name)
                {
                    s = malloc(strlen(found_name) + 1);
                    strcpy(s, found_name);
                }
                else
                {
                    snprintf(buf, sizeof(buf), "%lld", (long long)v.i);
                    s = malloc(strlen(buf) + 1);
                    strcpy(s, buf);
                }
                break;
            }
            case 0: /* int */
                s = int128_to_string(v.i);
                break;
            case 1: /* float */
                snprintf(buf, sizeof(buf), "%g", v.f);
                s = malloc(strlen(buf) + 1);
                strcpy(s, buf);
                break;
            case 2: /* bool */
                s = malloc(6);
                strcpy(s, v.b ? "true" : "false");
                break;
            case 5: /* unsigned int */
                s = uint128_to_string(v.i);
                break;
            case 3:
            { /* char -- encode codepoint as UTF-8 */
                uint32_t cp = (uint32_t)v.i;
                if (cp < 0x80)
                {
                    s = malloc(2);
                    s[0] = (char)cp;
                    s[1] = 0;
                }
                else if (cp < 0x800)
                {
                    s = malloc(3);
                    s[0] = (char)(0xC0 | (cp >> 6));
                    s[1] = (char)(0x80 | (cp & 0x3F));
                    s[2] = 0;
                }
                else if (cp < 0x10000)
                {
                    s = malloc(4);
                    s[0] = (char)(0xE0 | (cp >> 12));
                    s[1] = (char)(0x80 | ((cp >> 6) & 0x3F));
                    s[2] = (char)(0x80 | (cp & 0x3F));
                    s[3] = 0;
                }
                else
                {
                    s = malloc(5);
                    s[0] = (char)(0xF0 | (cp >> 18));
                    s[1] = (char)(0x80 | ((cp >> 12) & 0x3F));
                    s[2] = (char)(0x80 | ((cp >> 6) & 0x3F));
                    s[3] = (char)(0x80 | (cp & 0x3F));
                    s[4] = 0;
                }
                break;
            }
            default: /* string -- already a string, push back as-is */
                PUSH(v);
                goto to_str_done;
            }
            PUSH(val_str(s));
        to_str_done:;
            break;
        }

        /* ── Boolean operations ─────────────────────────────────── */
        case OP_NOT_BOOL:
        {
            Value a = POP();
            PUSH(val_bool(!a.b));
            break;
        }
        case OP_AND_BOOL:
            BINARY_OP(b, &&, val_bool);
            break;
        case OP_OR_BOOL:
            BINARY_OP(b, ||, val_bool);
            break;

        /* ── Integer comparisons ────────────────────────────────── */
        case OP_CMP_EQ_INT:
            BINARY_OP(i, ==, val_bool);
            break;
        case OP_CMP_NEQ_INT:
            BINARY_OP(i, !=, val_bool);
            break;
        case OP_CMP_LT_INT:
            BINARY_OP(i, <, val_bool);
            break;
        case OP_CMP_LTE_INT:
            BINARY_OP(i, <=, val_bool);
            break;
        case OP_CMP_GT_INT:
            BINARY_OP(i, >, val_bool);
            break;
        case OP_CMP_GTE_INT:
            BINARY_OP(i, >=, val_bool);
            break;

        /* ── Float comparisons ──────────────────────────────────── */
        case OP_CMP_EQ_FLOAT:
            BINARY_OP(f, ==, val_bool);
            break;
        case OP_CMP_NEQ_FLOAT:
            BINARY_OP(f, !=, val_bool);
            break;
        case OP_CMP_LT_FLOAT:
            BINARY_OP(f, <, val_bool);
            break;
        case OP_CMP_LTE_FLOAT:
            BINARY_OP(f, <=, val_bool);
            break;
        case OP_CMP_GT_FLOAT:
            BINARY_OP(f, >, val_bool);
            break;
        case OP_CMP_GTE_FLOAT:
            BINARY_OP(f, >=, val_bool);
            break;

        /* ── Bool/string equality ───────────────────────────────── */
        case OP_CMP_EQ_BOOL:
            BINARY_OP(b, ==, val_bool);
            break;
        case OP_CMP_NEQ_BOOL:
            BINARY_OP(b, !=, val_bool);
            break;
        case OP_CMP_EQ_STR:
        {
            Value b = POP();
            Value a = POP();
            bool eq = (a.s && b.s) ? strcmp(a.s, b.s) == 0
                                   : a.s == b.s;
            PUSH(val_bool(eq));
            break;
        }
        case OP_CMP_NEQ_STR:
        {
            Value b = POP();
            Value a = POP();
            bool eq = (a.s && b.s) ? strcmp(a.s, b.s) == 0
                                   : a.s == b.s;
            PUSH(val_bool(!eq));
            break;
        }
        /* Generic equality — uses elem_kind_reg set by the preceding OP_ARRAY_GET.
         * Allows T == T in generic classes to work correctly for any element type. */
        case OP_CMP_EQ_VAL:
        {
            Value b = POP();
            Value a = POP();
            bool eq;
            if (vm->elem_kind_reg == TYPE_STRING)
            {
                eq = (a.s && b.s) ? strcmp(a.s, b.s) == 0 : a.s == b.s;
            }
            else if (vm->elem_kind_reg == TYPE_FLOAT ||
                     vm->elem_kind_reg == TYPE_DOUBLE)
            {
                eq = (a.f == b.f);
            }
            else if (vm->elem_kind_reg == TYPE_BOOL)
            {
                eq = (a.b == b.b);
            }
            else
            {
                eq = (a.i == b.i); /* int, enum, object pointer, etc. */
            }
            PUSH(val_bool(eq));
            break;
        }
        case OP_CMP_NEQ_VAL:
        {
            Value b = POP();
            Value a = POP();
            bool eq;
            if (vm->elem_kind_reg == TYPE_STRING)
            {
                eq = (a.s && b.s) ? strcmp(a.s, b.s) == 0 : a.s == b.s;
            }
            else if (vm->elem_kind_reg == TYPE_FLOAT ||
                     vm->elem_kind_reg == TYPE_DOUBLE)
            {
                eq = (a.f == b.f);
            }
            else if (vm->elem_kind_reg == TYPE_BOOL)
            {
                eq = (a.b == b.b);
            }
            else
            {
                eq = (a.i == b.i);
            }
            PUSH(val_bool(!eq));
            break;
        }

        /* ── Stack management ───────────────────────────────────── */
        case OP_POP:
            --vm->sp;
            break;

        /* ── Control flow ───────────────────────────────────────── */
        case OP_JUMP:
        {
            int16_t offset = READ_I16();
            frame->ip += offset;
            break;
        }

        case OP_JUMP_IF_FALSE:
        {
            int16_t offset = READ_I16();
            Value cond = POP();
            if (!cond.b)
                frame->ip += offset;
            break;
        }

        case OP_JUMP_IF_TRUE:
        {
            int16_t offset = READ_I16();
            Value cond = POP();
            if (cond.b)
                frame->ip += offset;
            break;
        }

        /* ── Script function calls ──────────────────────────────── */
        case OP_CALL:
        {
            uint16_t fn_idx = READ_U16();
            uint8_t argc = READ_BYTE();

            if (vm->frame_count >= XENO_FRAME_MAX)
                RUNTIME_ERROR("Stack overflow: too many nested calls");

            if (fn_idx >= (uint16_t)vm->module->count)
                RUNTIME_ERROR("Invalid function index %d", fn_idx);

            Chunk *callee = &vm->module->chunks[fn_idx];

            if (argc != (uint8_t)callee->param_count)
                RUNTIME_ERROR("Function expects %d argument(s), got %d",
                              callee->param_count, argc);

            /* Set up a new call frame */
            CallFrame *new_frame = &vm->frames[vm->frame_count++];
            new_frame->chunk = callee;
            new_frame->ip = callee->code;
            memset(new_frame->slots, 0, sizeof(new_frame->slots));
            new_frame->type_arg_count = 0;

            /* Copy arguments from the value stack into the new frame's
             * parameter slots. Arguments were pushed left-to-right,
             * so argv[0] is deepest, argv[argc-1] is on top. */
            for (int i = argc - 1; i >= 0; i--)
            {
                new_frame->slots[i] = POP();
            }

            /* Switch execution to the new frame */
            frame = new_frame;
            break;
        }

        /* ── Return from script function ────────────────────────── */
        case OP_RETURN:
        {
            Value result = POP(); /* The return value */
            vm->frame_count--;

            if (vm->frame_count == 0)
            {
                /* Returned from the top-level entry point — done */
                PUSH(result);
                return XENO_OK;
            }

            /* Restore the caller's frame and push the return value */
            frame = &vm->frames[vm->frame_count - 1];
            PUSH(result);
            break;
        }

        case OP_RETURN_VOID:
        {
            bool was_constructor = frame->chunk->is_constructor;
            vm->frame_count--;

            if (vm->frame_count == 0)
            {
                /* Top-level void function returned — done */
                return XENO_OK;
            }

            frame = &vm->frames[vm->frame_count - 1];

            /* Check if we just returned from an event handler */
            if (vm->pending_fire_active &&
                vm->frame_count == vm->pending_fire_base_frame)
            {
                /* Advance to the next handler */
                vm->pending_handler++;
                if (vm->pending_handler < vm->pending_fire_handler_count)
                {
                    /* Push next handler's frame */
                    int fn_idx = vm->pending_fire_resolved[vm->pending_handler];
                    Value recv = vm->pending_fire_receivers[vm->pending_handler];
                    if (vm->frame_count < XENO_FRAME_MAX)
                    {
                        Chunk *hchunk = &vm->module->chunks[fn_idx];
                        CallFrame *hframe = &vm->frames[vm->frame_count++];
                        hframe->chunk = hchunk;
                        hframe->ip = hchunk->code;
                        memset(hframe->slots, 0, sizeof(hframe->slots));
                        hframe->type_arg_count = 0;
                        if (!is_val_null(recv))
                        {
                            hframe->slots[0] = recv;
                            for (int i = 0; i < vm->pending_fire_argc; i++)
                                hframe->slots[i + 1] = vm->pending_fire_args[i];
                        }
                        else
                        {
                            for (int i = 0; i < vm->pending_fire_argc; i++)
                                hframe->slots[i] = vm->pending_fire_args[i];
                        }
                        frame = hframe;
                        break; /* continue executing the next handler */
                    }
                }
                else
                {
                    /* All handlers fired — clear state, resume caller (no push) */
                    vm->pending_fire_active = false;
                    /* frame is already restored to caller above; just continue */
                    break;
                }
            }

            /* Constructors: the object was already pushed by OP_NEW before
             * the constructor frame was set up. Don't push a dummy — it would
             * sit on top of the object and confuse STORE_LOCAL. */
            if (!was_constructor)
            {
                PUSH(val_int(0));
            }
            break;
        }

        case OP_MATCH_FAIL:
            RUNTIME_ERROR("No matching case in match statement");

        /* Array operations */
        case OP_NEW_ARRAY:
        {
            uint8_t raw_kind = READ_BYTE(); /* TypeKind tag emitted by compiler */
            /* If high bit set, the lower 7 bits are a type_param index —
             * resolve to the concrete kind from the current frame's type_args. */
            uint8_t elem_kind;
            if (raw_kind & 0x80)
            {
                uint8_t param_idx = raw_kind & 0x7F;
                elem_kind = (param_idx < frame->type_arg_count)
                                ? frame->type_args[param_idx]
                                : (uint8_t)TYPE_ANY;
            }
            else
            {
                elem_kind = raw_kind;
            }
            int64_t len = POP().i;
            if (len < 0)
                RUNTIME_ERROR("Array length cannot be negative (%lld)", (long long)len);
            if (len > 1000000)
                RUNTIME_ERROR("Array length too large (%lld)", (long long)len);
            XenoArray *arr = malloc(sizeof(XenoArray) + (size_t)len * sizeof(Value));
            if (!arr)
                RUNTIME_ERROR("Out of memory allocating array of length %lld", (long long)len);
            arr->length = (int)len;
            arr->elem_kind = elem_kind;
            /* Zero-initialize all elements */
            for (int i = 0; i < (int)len; i++)
                arr->elements[i].i = 0;
            PUSH(val_arr(arr));
            break;
        }
        case OP_ARRAY_LIT:
        {
            uint8_t count = READ_BYTE();
            XenoArray *arr = malloc(sizeof(XenoArray) + count * sizeof(Value));
            if (!arr)
                RUNTIME_ERROR("Out of memory allocating array literal");
            arr->length = count;
            /* Elements are on stack in order (e0 pushed first, eN last).
             * We need to copy them out in reverse since stack is LIFO. */
            for (int i = count - 1; i >= 0; i--)
                arr->elements[i] = POP();
            PUSH(val_arr(arr));
            break;
        }
        case OP_ARRAY_GET:
        {
            int64_t idx = POP().i;
            XenoArray *arr = POP().arr;
            if (!arr)
                RUNTIME_ERROR("Null array dereference");
            if (idx < 0 || idx >= arr->length)
                RUNTIME_ERROR("Array index %lld out of bounds (length %d)",
                              (long long)idx, arr->length);
            vm->elem_kind_reg = arr->elem_kind; /* expose for OP_CMP_EQ_VAL */
            PUSH(arr->elements[(int)idx]);
            break;
        }
        case OP_ARRAY_SET:
        {
            Value val = POP();
            int64_t idx = POP().i;
            XenoArray *arr = POP().arr;
            if (!arr)
                RUNTIME_ERROR("Null array dereference");
            if (idx < 0 || idx >= arr->length)
                RUNTIME_ERROR("Array index %lld out of bounds (length %d)",
                              (long long)idx, arr->length);
            arr->elements[(int)idx] = val;
            break;
        }
        case OP_IS_TYPE:
        {
            /* [type_tag]  ( val -- bool ) */
            uint8_t tag = READ_BYTE();
            Value v = POP();
            PUSH(val_bool(xeno_value_is_type(v, tag)));
            break;
        }

        case OP_AS_TYPE:
        {
            /* [type_tag]  ( val -- val )  runtime error if incompatible */
            uint8_t tag = READ_BYTE();
            Value v = POP();
            if (!xeno_value_is_type(v, tag))
                RUNTIME_ERROR("Cast failed: value is not a %s", xeno_type_name(tag));
            PUSH(v);
            break;
        }

        case OP_TYPEOF:
        {
            /* [type_tag][name_len][name_bytes...]
             * name_len=0 means use built-in name for tag. */
            uint8_t tag = READ_BYTE();
            uint8_t name_len = READ_BYTE();
            char *tname = NULL;
            if (name_len > 0)
            {
                tname = malloc(name_len + 1);
                if (!tname)
                    RUNTIME_ERROR("Out of memory");
                for (int ni = 0; ni < name_len; ni++)
                    tname[ni] = (char)READ_BYTE();
                tname[name_len] = '\0';
            }
            Value v = POP();
            (void)v; /* value not needed for type construction */
            XenoType *t = xeno_make_type(tag, tname, vm->module);
            if (!t)
                RUNTIME_ERROR("Out of memory allocating Type object");
            PUSH(val_type(t));
            break;
        }
        case OP_TYPE_FIELD:
        {
            /* [field_id]  0=name 1=isArray 2=isPrimitive 3=isEnum 4=isClass */
            uint8_t fid = READ_BYTE();
            Value v = POP();
            XenoType *t = v.type;
            if (!t)
                RUNTIME_ERROR("Null Type dereference");
            switch (fid)
            {
            case 0:
            { /* .name */
                char *s = malloc(strlen(t->name) + 1);
                if (!s)
                    RUNTIME_ERROR("Out of memory");
                strcpy(s, t->name);
                PUSH(val_str(s));
                break;
            }
            case 1:
                PUSH(val_bool(t->is_array));
                break; /* .isArray */
            case 2:
                PUSH(val_bool(t->is_primitive));
                break; /* .isPrimitive */
            case 3:
                PUSH(val_bool(t->is_enum));
                break; /* .isEnum */
            case 4:
                PUSH(val_bool(t->is_class));
                break; /* .isClass */
            default:
                RUNTIME_ERROR("Unknown Type field %d", fid);
            }
            break;
        }

            /* ── Attribute reflection ──────────────────────────────────────────── */

        case OP_TYPE_HAS_ATTR:
        {
            /* ( string name, Type -- bool )
             * name was pushed first, Type on TOS. */
            Value type_val = POP();
            Value name_val = POP();
            XenoType *t = type_val.type;
            if (!t)
                RUNTIME_ERROR("Null Type in hasAttribute");
            const char *attr_name = name_val.s;
            if (!attr_name)
            {
                PUSH(val_bool(false));
                break;
            }
            bool found = false;
            if (t->class_def)
            {
                const ClassDef *def = t->class_def;
                for (int _ai = 0; _ai < def->attribute_count; _ai++)
                {
                    if (strcmp(def->attributes[_ai].class_name, attr_name) == 0)
                    {
                        found = true;
                        break;
                    }
                }
            }
            PUSH(val_bool(found));
            break;
        }

        case OP_TYPE_GET_ATTR_ARG:
        {
            /* ( int index, string name, Type -- string? )
             * index pushed first, name next, Type on TOS. */
            Value type_val = POP();
            Value name_val = POP();
            Value index_val = POP();
            XenoType *t = type_val.type;
            if (!t)
                RUNTIME_ERROR("Null Type in getAttributeArg");
            const char *attr_name = name_val.s;
            int arg_idx = (int)index_val.i;
            if (!attr_name || !t->class_def || arg_idx < 0)
            {
                PUSH(val_null());
                break;
            }
            const ClassDef *def = t->class_def;
            for (int _ai = 0; _ai < def->attribute_count; _ai++)
            {
                if (strcmp(def->attributes[_ai].class_name, attr_name) != 0)
                    continue;
                const AttributeInstance *inst = &def->attributes[_ai];
                if (arg_idx >= inst->arg_count)
                {
                    PUSH(val_null());
                    goto attr_done;
                }
                const AttrArg *arg = &inst->args[arg_idx];
                char buf[64];
                char *result = NULL;
                switch (arg->kind)
                {
                case ATTR_ARG_STRING:
                {
                    const char *_s = arg->s ? arg->s : "";
                    result = malloc(strlen(_s) + 1);
                    if (result)
                        strcpy(result, _s);
                    break;
                }
                case ATTR_ARG_INT:
                    snprintf(buf, sizeof(buf), "%lld", (long long)arg->i);
                    result = malloc(strlen(buf) + 1);
                    if (result)
                        strcpy(result, buf);
                    break;
                case ATTR_ARG_FLOAT:
                    snprintf(buf, sizeof(buf), "%g", arg->f);
                    result = malloc(strlen(buf) + 1);
                    if (result)
                        strcpy(result, buf);
                    break;
                case ATTR_ARG_BOOL:
                {
                    const char *_s = arg->b ? "true" : "false";
                    result = malloc(strlen(_s) + 1);
                    if (result)
                        strcpy(result, _s);
                    break;
                }
                default:
                    result = malloc(1);
                    if (result)
                        result[0] = '\0';
                    break;
                }
                if (!result)
                    RUNTIME_ERROR("Out of memory in getAttributeArg");
                PUSH(val_str(result));
                goto attr_done;
            }
            PUSH(val_null()); /* attribute not found */
        attr_done:;
            break;
        }

            /* ── Nullable operators ────────────────────────────────────────────── */

        case OP_PUSH_NULL:
            /* Push the null value (all bits zero) */
            PUSH(val_null());
            break;

        case OP_IS_NULL:
        {
            /* ( val -- bool )  true if val is null */
            Value v = POP();
            PUSH(val_bool(is_val_null(v)));
            break;
        }

        case OP_NULL_ASSERT:
        {
            /* [uint16 line]  ( val -- val )  assert val != null */
            uint16_t ln = READ_U16();
            Value v = *(vm->sp - 1); /* peek TOS */
            if (is_val_null(v))
                RUNTIME_ERROR("Null assertion failed at line %d — value was null", (int)ln);
            /* Leave val on stack */
            break;
        }

        case OP_NULL_COALESCE:
        {
            /* [uint16 jump_offset]  ( val -- val )
             * Peek TOS. If non-null, skip the right-side eval (jump).
             * If null, pop it and fall through to eval right side. */
            uint16_t offset = READ_U16();
            Value v = *(vm->sp - 1); /* peek TOS */
            if (!is_val_null(v))
            {
                /* Non-null: skip right side */
                frame->ip += offset;
            }
            else
            {
                /* Null: pop null value, fall through to eval right */
                vm->sp--;
            }
            break;
        }

        /* ── Exception handling opcodes ─────────────────────────────── */
        case OP_TRY_BEGIN:
        {
            /* Push an exception handler frame */
            uint16_t raw_offset = READ_U16();
            int16_t offset = (int16_t)raw_offset;
            if (vm->handler_count >= XENO_HANDLER_MAX)
                RUNTIME_ERROR("Too many nested try blocks");
            ExceptionHandler *h = &vm->handlers[vm->handler_count++];
            h->frame_count = vm->frame_count;
            h->sp = vm->sp;
            h->catch_ip = frame->ip + offset;
            h->catch_chunk = frame->chunk;
            h->class_name[0] = '\0'; /* catch-all by default */
            break;
        }

        case OP_TRY_END:
        {
            /* Normal exit from try block — pop handler */
            if (vm->handler_count > 0)
                vm->handler_count--;
            break;
        }

        case OP_THROW:
        {
            /* Pop exception object off value stack */
            Value ex = POP();

            /* Walk handler stack for a matching catch clause */
            while (vm->handler_count > 0)
            {
                ExceptionHandler *h = &vm->handlers[vm->handler_count - 1];

                /* Unwind call frames and value stack */
                vm->frame_count = h->frame_count;
                vm->sp = h->sp;

                /* Save caught exception */
                vm->caught_exception = ex;

                /* Pop this handler and jump to catch dispatch */
                vm->handler_count--;
                frame = &vm->frames[vm->frame_count - 1];
                frame->ip = h->catch_ip;
                frame->chunk = h->catch_chunk;
                goto dispatch; /* re-enter the main loop at catch site */
            }

            /* No handler found — runtime error */
            /* Try to extract message field from Exception object */
            const char *msg = "(no message)";
            if (!is_val_null(ex) && ex.obj)
            {
                ClassDef *cls = ex.obj->class_def;
                if (cls)
                {
                    for (int _fi = 0; _fi < cls->field_count; _fi++)
                    {
                        if (strcmp(cls->fields[_fi].name, "message") == 0 && cls->fields[_fi].instance_slot >= 0)
                        {
                            Value mv = ex.obj->fields[cls->fields[_fi].instance_slot];
                            if (mv.s)
                                msg = mv.s;
                            break;
                        }
                    }
                }
            }
            RUNTIME_ERROR("Unhandled exception: %s", msg);
        }

        case OP_LOAD_EXCEPTION:
        {
            /* Push the caught exception onto the value stack */
            PUSH(vm->caught_exception);
            break;
        }

        case OP_EXCEPTION_IS_TYPE:
        {
            /* [u8 name_len][name_bytes]
             * Check if vm->caught_exception is an instance of the named class
             * (or any subclass). Push bool result. */
            uint8_t nlen = READ_BYTE();
            char cname[128];
            int cn = nlen < 127 ? nlen : 127;
            for (int _i = 0; _i < nlen; _i++)
            {
                uint8_t ch = READ_BYTE();
                if (_i < cn)
                    cname[_i] = (char)ch;
            }
            cname[cn] = '\0';

            bool matches = false;
            Value ex = vm->caught_exception;
            if (!is_val_null(ex) && ex.obj && ex.obj->class_def)
            {
                /* Walk inheritance chain comparing class names */
                ClassDef *cls = ex.obj->class_def;
                while (cls)
                {
                    if (strcmp(cls->name, cname) == 0)
                    {
                        matches = true;
                        break;
                    }
                    int pidx = cls->parent_index;
                    if (pidx < 0 || !vm->module)
                        break;
                    cls = (pidx < vm->module->class_count)
                              ? &vm->module->classes[pidx]
                              : NULL;
                }
            }
            PUSH(val_bool(matches));
            break;
        }

        case OP_EVENT_SUBSCRIBE:
        case OP_EVENT_UNSUBSCRIBE:
        case OP_EVENT_SUBSCRIBE_BOUND:
        case OP_EVENT_UNSUBSCRIBE_BOUND:
        {
            bool is_subscribe = (instruction == (uint8_t)OP_EVENT_SUBSCRIBE ||
                                 instruction == (uint8_t)OP_EVENT_SUBSCRIBE_BOUND);
            bool is_bound = (instruction == (uint8_t)OP_EVENT_SUBSCRIBE_BOUND ||
                             instruction == (uint8_t)OP_EVENT_UNSUBSCRIBE_BOUND);

            /* Pop receiver BEFORE reading operands for bound delegates */
            Value receiver = is_bound ? POP() : val_null();

            /* Read event name */
            uint8_t nlen = READ_BYTE();
            char ename[65];
            int nl = nlen < 64 ? nlen : 64;
            for (int i = 0; i < nl; i++)
                ename[i] = (char)READ_BYTE();
            for (int i = nl; i < nlen; i++)
                (void)READ_BYTE();
            ename[nl] = '\0';
            /* Read handler function/method name */
            uint8_t hnlen = READ_BYTE();
            char hname[65];
            int hnl = hnlen < 64 ? hnlen : 64;
            for (int i = 0; i < hnl; i++)
                hname[i] = (char)READ_BYTE();
            for (int i = hnl; i < hnlen; i++)
                (void)READ_BYTE();
            hname[hnl] = '\0';

            /* Find or create event table entry */
            int ei = -1;
            for (int i = 0; i < vm->event_count; i++)
            {
                if (vm->event_table[i].active &&
                    strcmp(vm->event_table[i].name, ename) == 0)
                {
                    ei = i;
                    break;
                }
            }
            if (ei < 0)
            {
                if (vm->event_count >= XENO_MAX_EVENTS)
                    RUNTIME_ERROR("Too many events");
                ei = vm->event_count++;
                strncpy(vm->event_table[ei].name, ename, 63);
                vm->event_table[ei].name[63] = '\0';
                vm->event_table[ei].handler_count = 0;
                vm->event_table[ei].active = true;
            }

            if (is_subscribe)
            {
                /* Deduplicate: for bound delegates, same fn_name + same receiver obj ptr.
                 * For static, same fn_name + null receiver. */
                bool already = false;
                for (int hi = 0; hi < vm->event_table[ei].handler_count; hi++)
                {
                    bool name_match = strcmp(vm->event_table[ei].handlers[hi].fn_name, hname) == 0;
                    bool recv_match = is_bound
                                          ? (!is_val_null(vm->event_table[ei].handlers[hi].receiver) &&
                                             vm->event_table[ei].handlers[hi].receiver.obj == receiver.obj)
                                          : is_val_null(vm->event_table[ei].handlers[hi].receiver);
                    if (name_match && recv_match)
                    {
                        already = true;
                        break;
                    }
                }
                if (!already &&
                    vm->event_table[ei].handler_count < XENO_MAX_EVENT_HANDLERS)
                {
                    int hi = vm->event_table[ei].handler_count++;
                    strncpy(vm->event_table[ei].handlers[hi].fn_name, hname, 63);
                    vm->event_table[ei].handlers[hi].fn_name[63] = '\0';
                    vm->event_table[ei].handlers[hi].receiver = receiver;
                }
            }
            else
            {
                /* Remove matching handler */
                for (int hi = 0; hi < vm->event_table[ei].handler_count; hi++)
                {
                    bool name_match = strcmp(vm->event_table[ei].handlers[hi].fn_name, hname) == 0;
                    bool recv_match = is_bound
                                          ? (!is_val_null(vm->event_table[ei].handlers[hi].receiver) &&
                                             vm->event_table[ei].handlers[hi].receiver.obj == receiver.obj)
                                          : is_val_null(vm->event_table[ei].handlers[hi].receiver);
                    if (name_match && recv_match)
                    {
                        int last = vm->event_table[ei].handler_count - 1;
                        if (hi != last)
                            vm->event_table[ei].handlers[hi] =
                                vm->event_table[ei].handlers[last];
                        vm->event_table[ei].handler_count--;
                        break;
                    }
                }
            }
            break;
        }

        case OP_EVENT_FIRE:
        {
            /* Read event name and arg count */
            uint8_t nlen = READ_BYTE();
            char ename[65];
            int nl = nlen < 64 ? nlen : 64;
            for (int i = 0; i < nl; i++)
                ename[i] = (char)READ_BYTE();
            for (int i = nl; i < nlen; i++)
                (void)READ_BYTE();
            ename[nl] = '\0';
            uint8_t argc = READ_BYTE();

            /* Find event in table */
            int ei = -1;
            for (int i = 0; i < vm->event_count; i++)
            {
                if (vm->event_table[i].active &&
                    strcmp(vm->event_table[i].name, ename) == 0)
                {
                    ei = i;
                    break;
                }
            }
            /* If no handlers, just pop args and continue */
            if (ei < 0 || vm->event_table[ei].handler_count == 0)
            {
                for (int i = 0; i < argc; i++)
                    (void)POP();
                break;
            }

            /* Resolve handlers to fn indices + receivers */
            int resolved[XENO_MAX_EVENT_HANDLERS];
            Value receivers[XENO_MAX_EVENT_HANDLERS];
            int resolved_count = 0;
            for (int hi = 0; hi < vm->event_table[ei].handler_count; hi++)
            {
                const char *hname = vm->event_table[ei].handlers[hi].fn_name;
                Value recv = vm->event_table[ei].handlers[hi].receiver;
                int fn_idx = -1;

                if (!is_val_null(recv))
                {
                    /* Bound delegate — resolve method name on receiver's class */
                    XenoObject *obj = recv.obj;
                    if (obj && obj->class_def)
                    {
                        ClassDef *cd = obj->class_def;
                        for (int mi = 0; mi < cd->method_count; mi++)
                        {
                            if (strcmp(cd->methods[mi].name, hname) == 0)
                            {
                                fn_idx = cd->methods[mi].fn_index;
                                break;
                            }
                        }
                    }
                }
                else
                {
                    /* Static handler — resolve by chunk name */
                    for (int fi = 0; fi < vm->module->count; fi++)
                    {
                        if (strcmp(vm->module->names[fi], hname) == 0)
                        {
                            fn_idx = fi;
                            break;
                        }
                    }
                }
                if (fn_idx >= 0)
                {
                    resolved[resolved_count] = fn_idx;
                    receivers[resolved_count] = recv;
                    resolved_count++;
                }
            }
            if (resolved_count == 0)
            {
                for (int i = 0; i < argc; i++)
                    (void)POP();
                break;
            }

            /* Save args off the stack */
            int ac = argc < XENO_MAX_EVENT_ARGS ? argc : XENO_MAX_EVENT_ARGS;
            for (int i = ac - 1; i >= 0; i--)
                vm->pending_fire_args[i] = POP();

            /* Set up iterative firing state */
            vm->pending_fire_active = true;
            vm->pending_event_idx = ei;
            vm->pending_handler = 0;
            vm->pending_fire_argc = ac;
            vm->pending_fire_handler_count = resolved_count;
            vm->pending_fire_base_frame = vm->frame_count;
            for (int i = 0; i < resolved_count; i++)
            {
                vm->pending_fire_resolved[i] = resolved[i];
                vm->pending_fire_receivers[i] = receivers[i];
            }

            /* Push first handler's frame */
            if (vm->frame_count >= XENO_FRAME_MAX)
                RUNTIME_ERROR("Call stack overflow in event fire");
            {
                int fn_idx = vm->pending_fire_resolved[0];
                Value recv0 = vm->pending_fire_receivers[0];
                Chunk *hchunk = &vm->module->chunks[fn_idx];
                CallFrame *hframe = &vm->frames[vm->frame_count++];
                hframe->chunk = hchunk;
                hframe->ip = hchunk->code;
                memset(hframe->slots, 0, sizeof(hframe->slots));
                hframe->type_arg_count = 0;
                if (!is_val_null(recv0))
                {
                    /* Bound: slot 0 = receiver (this), args start at slot 1 */
                    hframe->slots[0] = recv0;
                    for (int i = 0; i < ac; i++)
                        hframe->slots[i + 1] = vm->pending_fire_args[i];
                }
                else
                {
                    for (int i = 0; i < ac; i++)
                        hframe->slots[i] = vm->pending_fire_args[i];
                }
                frame = hframe;
            }
            break;
        }

        case OP_ARRAY_LEN:
        {
            XenoArray *arr = POP().arr;
            if (!arr)
                RUNTIME_ERROR("Null array dereference");
            PUSH(val_int(arr->length));
            break;
        }

        /* Integer truncation: wrap to narrow type width */
        case OP_TRUNC_I8:
        {
            int64_t v = POP().i;
            PUSH(val_int((int64_t)(int8_t)(v & 0xFF)));
            break;
        }
        case OP_TRUNC_U8:
        {
            int64_t v = POP().i;
            PUSH(val_int((int64_t)(uint8_t)(v & 0xFF)));
            break;
        }
        case OP_TRUNC_I16:
        {
            int64_t v = POP().i;
            PUSH(val_int((int64_t)(int16_t)(v & 0xFFFF)));
            break;
        }
        case OP_TRUNC_U16:
        {
            int64_t v = POP().i;
            PUSH(val_int((int64_t)(uint16_t)(v & 0xFFFF)));
            break;
        }
        case OP_TRUNC_I32:
        {
            int64_t v = POP().i;
            PUSH(val_int((int64_t)(int32_t)(v & 0xFFFFFFFF)));
            break;
        }
        case OP_TRUNC_U32:
        {
            int64_t v = POP().i;
            PUSH(val_int((int64_t)(uint32_t)(v & 0xFFFFFFFF)));
            break;
        }
        case OP_TRUNC_U64:
        {
            uint64_t v = (uint64_t)POP().i;
            PUSH(val_int((__int128_t)v));
            break;
        }
        case OP_TRUNC_CHAR:
        {
            int64_t v = POP().i;
            PUSH(val_int((int64_t)((uint32_t)v & 0x10FFFF)));
            break;
        }

        /* ── Host function calls ────────────────────────────────── */
        case OP_CALL_HOST:
        {
            uint16_t host_idx = READ_U16();
            uint8_t argc = READ_BYTE();

            if (host_idx >= (uint16_t)vm->host_fn_count)
                RUNTIME_ERROR("Invalid host function index %d", host_idx);

            HostFnEntry *entry = &vm->host_fns[host_idx];

            /* Validate argument count (unless variadic) */
            if (entry->param_count >= 0 && argc != entry->param_count)
                RUNTIME_ERROR("Host function '%s' expects %d arg(s), got %d",
                              entry->name, entry->param_count, argc);

            /* Collect arguments from the stack into a temporary array.
             * Stack order: argv[0] deepest, argv[argc-1] on top. */
            Value argv[32]; /* Max 32 args to a host function */
            if (argc > 32)
                RUNTIME_ERROR("Too many arguments to host function");
            for (int i = argc - 1; i >= 0; i--)
                argv[i] = POP();

            /* Call the host function */
            Value result = val_int(0); /* Default return value */
            XenoResult hr = entry->fn(vm, argc, argv, &result);
            if (hr != XENO_OK)
                return hr;

            /* Push the return value (even for void — it'll be POP'd) */
            /* PUSH(result) expanded: */
            *vm->sp = result;
            vm->sp++;
            break;
        }

            /* ── Object opcodes ─────────────────────────────────────────── */

        case OP_NEW:
        {
            uint16_t class_idx = READ_U16();
            uint8_t argc = READ_BYTE();

            if (class_idx >= (uint16_t)vm->module->class_count)
                RUNTIME_ERROR("Invalid class index %d", class_idx);

            ClassDef *cls = &vm->module->classes[class_idx];

            /* Allocate the object — fixed header + one Value per field */
            XenoObject *obj = malloc(
                sizeof(XenoObject) + cls->field_count * sizeof(Value));
            if (!obj)
                RUNTIME_ERROR("Out of memory allocating '%s'", cls->name);

            obj->class_def = cls;

            /* Zero-initialize all fields */
            for (int i = 0; i < cls->field_count; i++)
                obj->fields[i] = val_int(0);

            /* If there's a constructor, call it.
             * Push 'this' as slot 0, then the args (already on stack). */
            if (cls->constructor_index >= 0)
            {
                /* Args are already on the stack in order.
                 * We need to insert 'this' before them in the new frame.
                 * Collect args first, then set up frame. */
                if (argc > 32)
                    RUNTIME_ERROR("Too many constructor arguments");
                Value args[32];
                for (int i = argc - 1; i >= 0; i--)
                    args[i] = POP();

                if (vm->frame_count >= XENO_FRAME_MAX)
                    RUNTIME_ERROR("Stack overflow in constructor");

                Chunk *ctor_chunk = &vm->module->chunks[cls->constructor_index];
                if (ctor_chunk->code == NULL || ctor_chunk->count == 0)
                {
                    RUNTIME_ERROR("Constructor '%s' has no bytecode (external stub not resolved)",
                                  vm->module->names[cls->constructor_index]);
                }
                CallFrame *ctor_frame = &vm->frames[vm->frame_count++];
                ctor_frame->chunk = ctor_chunk;
                ctor_frame->ip = ctor_chunk->code;

                /* Slot 0 = this, slots 1..argc = args */
                ctor_frame->slots[0] = val_obj(obj);
                for (int i = 0; i < argc; i++)
                    ctor_frame->slots[i + 1] = args[i];

                /* Read concrete type args */
                {
                    uint8_t tac = READ_BYTE();
                    if (tac > XENO_MAX_TYPE_ARGS)
                        tac = XENO_MAX_TYPE_ARGS;
                    ctor_frame->type_arg_count = tac;
                    for (int _i = 0; _i < tac; _i++)
                        ctor_frame->type_args[_i] = READ_BYTE();
                    for (int _i = tac; _i < XENO_MAX_TYPE_ARGS; _i++)
                        ctor_frame->type_args[_i] = TYPE_ANY;
                }

                frame = &vm->frames[vm->frame_count - 1];

                /* Constructor runs and will hit OP_RETURN_VOID.
                 * That will pop the frame and push a dummy — we'll POP it. */
            }
            else
            {
                /* No constructor — still need to consume the type_args bytes */
                uint8_t tac = READ_BYTE();
                if (tac > XENO_MAX_TYPE_ARGS)
                    tac = XENO_MAX_TYPE_ARGS;
                for (int _i = 0; _i < tac; _i++)
                    (void)READ_BYTE();
            }

            /* Push the new object */
            PUSH(val_obj(obj));
            break;
        }

        case OP_GET_FIELD:
        {
            uint8_t field_idx = READ_BYTE();
            Value v = POP();
            XenoObject *obj = v.obj;
            if (!obj)
                RUNTIME_ERROR("Null object dereference");
            if (field_idx >= obj->class_def->field_count)
                RUNTIME_ERROR("Field index %d out of range", field_idx);
            PUSH(obj->fields[field_idx]);
            break;
        }

        case OP_SET_FIELD:
        {
            uint8_t field_idx = READ_BYTE();
            Value val = POP();
            Value v = POP();
            XenoObject *obj = v.obj;
            if (!obj)
                RUNTIME_ERROR("Null object dereference");
            if (field_idx >= obj->class_def->field_count)
                RUNTIME_ERROR("Field index %d out of range", field_idx);
            obj->fields[field_idx] = val;
            break;
        }

        case OP_LOAD_THIS:
        {
            /* 'this' is always slot 0 in a method/constructor frame */
            PUSH(frame->slots[0]);
            break;
        }

        case OP_LOAD_STATIC:
        {
            uint8_t class_idx = READ_BYTE();
            uint8_t field_idx = READ_BYTE();
            if (class_idx >= (uint8_t)vm->module->class_count)
                RUNTIME_ERROR("LOAD_STATIC: invalid class index %d", class_idx);
            ClassDef *cls = &vm->module->classes[class_idx];
            if (field_idx >= (uint8_t)cls->field_count)
                RUNTIME_ERROR("LOAD_STATIC: invalid field index %d", field_idx);
            PUSH(cls->static_values[field_idx]);
            break;
        }

        case OP_STORE_STATIC:
        {
            uint8_t class_idx = READ_BYTE();
            uint8_t field_idx = READ_BYTE();
            if (class_idx >= (uint8_t)vm->module->class_count)
                RUNTIME_ERROR("STORE_STATIC: invalid class index %d", class_idx);
            ClassDef *cls = &vm->module->classes[class_idx];
            if (field_idx >= (uint8_t)cls->field_count)
                RUNTIME_ERROR("STORE_STATIC: invalid field index %d", field_idx);
            cls->static_values[field_idx] = POP();
            break;
        }

        case OP_CALL_IFACE:
        {
            /* Virtual dispatch through an interface-typed reference.
             * name_const_idx holds the method name as a string constant.
             * We look up the method on the ACTUAL runtime class_def. */
            uint16_t name_const_idx = READ_U16();
            uint8_t argc = READ_BYTE();

            if (argc > 32)
                RUNTIME_ERROR("Too many method arguments");
            Value args[32];
            for (int i = argc - 1; i >= 0; i--)
                args[i] = POP();
            Value obj_val = POP();

            if (!obj_val.obj)
                RUNTIME_ERROR("Null reference in interface method call");

            /* Fetch the method name from the constant pool */
            if (name_const_idx >= (uint16_t)frame->chunk->constants.count)
                RUNTIME_ERROR("Invalid interface method name constant index");
            Value name_val = frame->chunk->constants.values[name_const_idx];
            if (!name_val.s)
                RUNTIME_ERROR("Interface method name constant is not a string");
            const char *mname = name_val.s;

            /* Walk the object's class_def (and parent chain) for the method */
            ClassDef *search_cls = obj_val.obj ? obj_val.obj->class_def : NULL;
            if (!search_cls)
                RUNTIME_ERROR("Interface dispatch: object has no class_def");
            int iface_fn_idx = -1;
            int safety = 64;
            while (search_cls && safety-- > 0)
            {
                for (int i = 0; i < search_cls->method_count; i++)
                {
                    if (strcmp(search_cls->methods[i].name, mname) == 0 &&
                        !search_cls->methods[i].is_static)
                    {
                        iface_fn_idx = search_cls->methods[i].fn_index;
                        break;
                    }
                }
                if (iface_fn_idx >= 0)
                    break;
                /* Move to parent class */
                if (search_cls->parent_index >= 0 &&
                    search_cls->parent_index < vm->module->class_count)
                    search_cls = &vm->module->classes[search_cls->parent_index];
                else
                    break;
            }

            if (iface_fn_idx < 0)
                RUNTIME_ERROR("Interface dispatch: method '%s' not found on class '%s'",
                              mname, obj_val.obj->class_def->name);

            if (iface_fn_idx >= vm->module->count)
                RUNTIME_ERROR("Invalid method function index %d (interface dispatch)", iface_fn_idx);

            if (vm->frame_count >= XENO_FRAME_MAX)
                RUNTIME_ERROR("Stack overflow in interface method call");

            Chunk *method_chunk = &vm->module->chunks[iface_fn_idx];
            CallFrame *method_frame = &vm->frames[vm->frame_count++];
            method_frame->chunk = method_chunk;
            method_frame->ip = method_chunk->code;

            method_frame->type_arg_count = 0;
            method_frame->slots[0] = obj_val;
            for (int i = 0; i < argc; i++)
                method_frame->slots[i + 1] = args[i];

            frame = &vm->frames[vm->frame_count - 1];
            break;
        }

        case OP_CALL_METHOD:
        {
            uint16_t slot = READ_U16();
            uint8_t argc = READ_BYTE();
            if (argc > 32)
                RUNTIME_ERROR("Too many method arguments");
            Value args[32];
            for (int i = argc - 1; i >= 0; i--)
                args[i] = POP();
            Value obj_val = POP();

            if (!obj_val.obj)
                RUNTIME_ERROR("Null object dereference in method call");

            ClassDef *cls = obj_val.obj->class_def;
            if (!cls || slot >= (uint16_t)cls->method_count)
                RUNTIME_ERROR("Invalid method slot %d on class '%s'",
                              slot, cls ? cls->name : "?");

            int fn_idx = cls->methods[slot].fn_index;
            if (fn_idx < 0 || fn_idx >= vm->module->count)
                RUNTIME_ERROR("Method slot %d has no bound chunk (fn_idx=%d)",
                              slot, fn_idx);

            if (vm->frame_count >= XENO_FRAME_MAX)
                RUNTIME_ERROR("Stack overflow in method call");

            Chunk *method_chunk = &vm->module->chunks[fn_idx];
            if (!method_chunk->code)
                RUNTIME_ERROR("Method '%s' is an unresolved stub",
                              vm->module->names[fn_idx]);

            CallFrame *method_frame = &vm->frames[vm->frame_count++];
            method_frame->chunk = method_chunk;
            method_frame->ip = method_chunk->code;
            method_frame->type_arg_count = 0;
            method_frame->slots[0] = obj_val;
            for (int i = 0; i < argc; i++)
                method_frame->slots[i + 1] = args[i];

            frame = &vm->frames[vm->frame_count - 1];
            break;
        }

        case OP_CALL_SUPER:
        {
            uint16_t par_ci = READ_U16(); /* parent class index */
            uint8_t argc = READ_BYTE();

            /* super() — args are on the stack, 'this' is slot 0 of the
             * current frame. Resolve the constructor via the class def
             * (always correct after module_merge remaps fn_indices). */
            if (argc > 32)
                RUNTIME_ERROR("Too many super() arguments");
            Value args[32];
            for (int i = argc - 1; i >= 0; i--)
                args[i] = POP();

            if (par_ci >= (uint16_t)vm->module->class_count)
                RUNTIME_ERROR("Invalid super class index %d", par_ci);

            int fn_idx = vm->module->classes[par_ci].constructor_index;
            if (fn_idx < 0 || fn_idx >= vm->module->count)
                RUNTIME_ERROR("Parent class has no constructor");

            if (vm->frame_count >= XENO_FRAME_MAX)
                RUNTIME_ERROR("Stack overflow in super() call");

            Chunk *ctor_chunk = &vm->module->chunks[fn_idx];
            CallFrame *ctor_frame = &vm->frames[vm->frame_count++];
            ctor_frame->chunk = ctor_chunk;
            ctor_frame->ip = ctor_chunk->code;
            ctor_frame->type_arg_count = 0;
            ctor_frame->slots[0] = frame->slots[0];
            for (int i = 0; i < argc; i++)
                ctor_frame->slots[i + 1] = args[i];

            frame = &vm->frames[vm->frame_count - 1];
            break;
        }

        default:
            RUNTIME_ERROR("Unknown opcode 0x%02x", instruction);
        }
    }
}

/* ─────────────────────────────────────────────────────────────────────────────
 * PUBLIC RUN FUNCTIONS
 * ───────────────────────────────────────────────────────────────────────────*/

XenoResult xeno_vm_run(XenoVM *vm, Module *module)
{
    vm->module = module;
    vm->had_error = false;
    vm->sp = vm->stack;
    vm->frame_count = 0;

    /* ── Fill in any missing stdlib chunks ──────────────────────────────
     * Callers (run_xbc, run_xar, run_source) pre-seed the module with stdlib
     * class defs before loading user content so class indices are correct.
     * module_merge deduplicates by name so this is always safe to call. */
    for (int i = 0; i < vm->stdlib_module_count; i++)
        module_merge(module, vm->stdlib_modules[i]);

    /* ── Run static initializers, run it now to initialize all
     * static fields. This must happen before any entry point runs. */
    if (module->sinit_index >= 0 && module->sinit_index < module->count)
    {
        Chunk *sinit_chunk = &module->chunks[module->sinit_index];
        if (sinit_chunk->count > 0)
        {
            if (vm->frame_count >= XENO_FRAME_MAX)
            {
                xeno_vm_error(vm, "Stack overflow running static initializers");
                return XENO_RUNTIME_ERROR;
            }
            CallFrame *frame = &vm->frames[vm->frame_count++];
            frame->chunk = sinit_chunk;
            frame->ip = sinit_chunk->code;
            memset(frame->slots, 0, sizeof(Value) * (sinit_chunk->local_count > 0 ? sinit_chunk->local_count : 1));
            frame->type_arg_count = 0;
            XenoResult r = xeno_execute(vm);
            if (r != XENO_OK)
                return r;
        }
    }

    /* Determine entry point.
     * If the module has @Mod, instantiate the entry class, run constructor.
     * If no @Mod is present this is a library -- nothing to run. */
    if (module->metadata.has_mod)
    {
        const char *entry_class = module->metadata.entry_class;
        int ci = module_find_class(module, entry_class);
        if (ci < 0)
        {
            xeno_vm_error(vm, "@Mod entry class not found");
            return XENO_RUNTIME_ERROR;
        }
        ClassDef *cls = &module->classes[ci];

        /* Allocate the mod object */
        XenoObject *obj = malloc(sizeof(XenoObject) + cls->field_count * sizeof(Value));
        if (!obj)
        {
            xeno_vm_error(vm, "Out of memory allocating @Mod class '%s'", entry_class);
            return XENO_RUNTIME_ERROR;
        }
        obj->class_def = cls;
        for (int i = 0; i < cls->field_count; i++)
            obj->fields[i] = val_int(0);

        /* Run constructor if present */
        if (cls->constructor_index >= 0)
        {
            if (vm->frame_count >= XENO_FRAME_MAX)
            {
                free(obj);
                xeno_vm_error(vm, "Stack overflow starting @Mod constructor");
                return XENO_RUNTIME_ERROR;
            }
            Chunk *ctor_chunk = &module->chunks[cls->constructor_index];
            CallFrame *ctor_frame = &vm->frames[vm->frame_count++];
            ctor_frame->chunk = ctor_chunk;
            ctor_frame->ip = ctor_chunk->code;
            memset(ctor_frame->slots, 0, sizeof(ctor_frame->slots));
            ctor_frame->type_arg_count = 0;
            ctor_frame->slots[0] = val_obj(obj);
            XenoResult r = xeno_execute(vm);
            if (r != XENO_OK)
                return r;
        }

        /* Constructor-only entry point for mods */
        return XENO_OK;
    }

    /* No @Mod -- this is a library, nothing to run */
    return XENO_OK;
}

XenoResult xeno_vm_run_source(XenoVM *vm, const char *source)
{
    /* Free any previous source module (hot reload resets state) */
    if (vm->has_source_module)
    {
        module_free(&vm->source_module);
        vm->has_source_module = false;
    }

    /* Run the full compiler pipeline */
    Lexer lexer;
    Parser parser;
    Checker *checker = malloc(sizeof(Checker));
    Compiler compiler;
    CompilerHostTable host_table;

    if (!checker)
    {
        xeno_vm_error(vm, "Out of memory");
        return XENO_RUNTIME_ERROR;
    }

    lexer_init(&lexer, source);
    parser_init(&parser, &lexer);
    checker_init(checker, &parser.arena);
    compiler_host_table_init(&host_table);
    module_init(&vm->source_module);

    /* Resolve imports and load stdlib — same pipeline as xenoc */
    Module *staging = (Module *)calloc(1, sizeof(Module));
    module_init(staging);
    bool import_err = false;
    char *merged = pipeline_prepare(source, NULL, staging, &import_err);
    if (import_err || !merged)
    {
        free(merged);
        module_free(staging);
        free(staging);
        xeno_vm_error(vm, "Import error");
        free(checker);
        return XENO_COMPILE_ERROR;
    }

    /* Re-init lexer/parser on merged source */
    parser_free(&parser);
    lexer_init(&lexer, merged);
    parser_init(&parser, &lexer);
    checker_init(checker, &parser.arena);

    /* Pre-declare all registered host functions to the type checker
     * and compiler so they resolve correctly during compilation */
    for (int i = 0; i < vm->host_fn_count; i++)
    {
        HostFnEntry *e = &vm->host_fns[i];

        Type ret_type = {.kind = e->return_type_kind};
        Type params[MAX_PARAMS];
        int pcount = e->param_count < 0 ? 0 : e->param_count;
        if (pcount > MAX_PARAMS)
            pcount = MAX_PARAMS;

        for (int p = 0; p < pcount; p++)
            params[p].kind = e->param_type_kinds[p];

        checker_declare_host(checker, e->name, ret_type, params, pcount);

        /* Use add_any if any param is TYPE_ANY so the compiler emits OP_TO_STR */
        bool has_any = false;
        for (int p = 0; p < e->param_count; p++)
            if (e->param_type_kinds[p] == TYPE_ANY)
            {
                has_any = true;
                break;
            }
        if (has_any)
            compiler_host_table_add_any(&host_table, e->name, i, pcount);
        else
            compiler_host_table_add(&host_table, e->name, i, pcount);
    }

    /* Declare stdlib classes/functions to checker from staging */
    pipeline_declare_staging(checker, staging);

    /* Also declare any user dep modules pre-loaded into the VM pool
     * (e.g. via xeno_vm_load_xar before calling run_source). */
    for (int i = 0; i < vm->stdlib_module_count; i++)
        pipeline_declare_staging(checker, vm->stdlib_modules[i]);

    /* Parse */
    Program program = parser_parse(&parser);
    if (parser.had_error)
    {
        xeno_vm_error(vm, "Parse error at line %d: %s",
                      parser.errors[0].line,
                      parser.errors[0].message);
        parser_free(&parser);
        free(merged);
        free(checker);
        module_free(staging);
        free(staging);
        return XENO_COMPILE_ERROR;
    }

    /* Type check */
    bool checker_ok = checker_check(checker, &program);
    /* Print any warnings regardless of errors */
    for (int _wi = 0; _wi < checker->error_count; _wi++)
    {
        if (checker->errors[_wi].is_warning)
            fprintf(stderr, "[line %d] Warning: %s\n",
                    checker->errors[_wi].line, checker->errors[_wi].message);
    }
    if (!checker_ok)
    {
        xeno_vm_error(vm, "Type error at line %d: %s",
                      checker->errors[0].line,
                      checker->errors[0].message);
        parser_free(&parser);
        free(merged);
        free(checker);
        module_free(staging);
        free(staging);
        return XENO_COMPILE_ERROR;
    }

    /* Compile — pass staging so compiler can resolve stdlib classes (e.g. Exception)
     * via compiler_ensure_class fallback without inlining their source. */
    if (!compiler_compile_staged(&compiler, &program, &vm->source_module, &host_table, staging))
    {
        xeno_vm_error(vm, "Compile error at line %d: %s",
                      compiler.errors[0].line,
                      compiler.errors[0].message);
        parser_free(&parser);
        free(merged);
        free(checker);
        module_free(staging);
        free(staging);
        return XENO_COMPILE_ERROR;
    }

    /* staging no longer needed after compilation */
    module_free(staging);
    free(staging);

    /* Extract @Mod metadata so xeno_vm_run can find the entry point. */
    module_extract_mod_metadata(&vm->source_module, NULL);

    parser_free(&parser);
    free(merged);
    free(checker);
    vm->has_source_module = true;

    /* Execute */
    return xeno_vm_run(vm, &vm->source_module);
}