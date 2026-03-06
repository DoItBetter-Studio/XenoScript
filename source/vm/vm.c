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
#define READ_BYTE()   (*frame->ip++)

/* Read a uint16_t operand (big-endian, two bytes) */
#define READ_U16()    (frame->ip += 2, \
                       (uint16_t)((frame->ip[-2] << 8) | frame->ip[-1]))

/* Read a signed int16_t (for jump offsets) */
#define READ_I16()    ((int16_t)READ_U16())

/* Push a value onto the value stack */
#define PUSH(val)     (*vm->sp++ = (val))

/* Pop a value from the value stack */
#define POP()         (*--vm->sp)
#define DISCARD()     (vm->sp--)

/* Peek at the top of the stack without popping */
#define PEEK()        (*(vm->sp - 1))

/* Binary operation macro — pops two, pushes result.
 * `field` is the Value union field (i, f, b).
 * `op`    is the C operator to apply.
 * `rtype` is the result constructor (val_int, val_float, val_bool). */
#define BINARY_OP(field, op, rtype) \
    do { \
        Value b = POP(); \
        Value a = POP(); \
        PUSH(rtype(a.field op b.field)); \
    } while (0)

/* Runtime error — set message and return error code */
#define RUNTIME_ERROR(fmt, ...) \
    do { \
        xeno_vm_error(vm, fmt, ##__VA_ARGS__); \
        return XENO_RUNTIME_ERROR; \
    } while (0)


/* ─────────────────────────────────────────────────────────────────────────────
 * VM LIFECYCLE
 * ───────────────────────────────────────────────────────────────────────────*/

void xeno_vm_init(XenoVM *vm) {
    memset(vm, 0, sizeof(XenoVM));
    vm->sp          = vm->stack;    /* Stack pointer starts at bottom       */
    vm->frame_count = 0;
    vm->had_error   = false;
    vm->has_source_module = false;
}

void xeno_vm_free(XenoVM *vm) {
    if (vm->has_source_module) {
        module_free(&vm->source_module);
        vm->has_source_module = false;
    }
    for (int i = 0; i < vm->stdlib_module_count; i++)
        module_free(&vm->stdlib_modules[i]);
    vm->stdlib_module_count = 0;
}

/* ─────────────────────────────────────────────────────────────────────────────
 * STDLIB XAR LOADING
 * ───────────────────────────────────────────────────────────────────────────*/

static bool stdlib_already_loaded(XenoVM *vm, const char *name) {
    for (int i = 0; i < vm->stdlib_module_count; i++)
        if (strcmp(vm->stdlib_loaded_names[i], name) == 0) return true;
    return false;
}

static bool load_xar_into_pool(XenoVM *vm, const uint8_t *data, size_t size,
                                const char *name) {
    if (vm->stdlib_module_count >= 64) return false;

    XarArchive ar;
    if (xar_read_mem(&ar, data, size) != XAR_OK) {
        fprintf(stderr, "vm: failed to read embedded .xar '%s'\n", name);
        return false;
    }

    /* Each chunk in the archive is a compiled .xbc — merge all into one module */
    Module *pool_mod = &vm->stdlib_modules[vm->stdlib_module_count];
    module_init(pool_mod);

    for (int i = 0; i < ar.chunk_count; i++) {
        Module chunk_mod;
        module_init(&chunk_mod);
        XbcResult xr = xbc_read_mem(&chunk_mod,
                                     ar.chunks[i].data,
                                     ar.chunks[i].size);
        if (xr == XBC_OK) {
            module_merge(pool_mod, &chunk_mod);
            /* Run sinit for this chunk so statics are initialised */
            if (chunk_mod.sinit_index >= 0) {
                vm->module = pool_mod;
                Chunk *sc  = &chunk_mod.chunks[chunk_mod.sinit_index];
                if (sc->count > 0 && vm->frame_count < XENO_FRAME_MAX) {
                    CallFrame *frame = &vm->frames[vm->frame_count++];
                    frame->chunk = sc;
                    frame->ip    = sc->code;
                    memset(frame->slots, 0, sizeof(frame->slots));
                    xeno_execute(vm);
                }
            }
            module_free(&chunk_mod);
        }
    }
    xar_archive_free(&ar);

    strncpy(vm->stdlib_loaded_names[vm->stdlib_module_count], name,
            XAR_MAX_NAME - 1);
    vm->stdlib_module_count++;
    return true;
}

bool xeno_vm_load_stdlib_module(XenoVM *vm, const char *name) {
    if (stdlib_already_loaded(vm, name)) return true;
    for (int i = 0; i < STDLIB_XAR_TOTAL_COUNT; i++) {
        if (strcmp(STDLIB_XAR_TABLE[i].name, name) == 0) {
            size_t size = (size_t)(STDLIB_XAR_TABLE[i].end -
                                   STDLIB_XAR_TABLE[i].start);
            return load_xar_into_pool(vm, STDLIB_XAR_TABLE[i].start,
                                       size, name);
        }
    }
    return false; /* not found in embedded table */
}

void xeno_vm_load_stdlib(XenoVM *vm) {
    /* Always load auto-loaded modules (core primitive extensions) */
    for (int i = 0; i < STDLIB_XAR_AUTO_COUNT; i++) {
        size_t size = (size_t)(STDLIB_XAR_TABLE[i].end -
                               STDLIB_XAR_TABLE[i].start);
        load_xar_into_pool(vm, STDLIB_XAR_TABLE[i].start,
                            size, STDLIB_XAR_TABLE[i].name);
    }
}

void xeno_vm_set_mod_path(XenoVM *vm, const char *path) {
    strncpy(vm->mod_path, path, sizeof(vm->mod_path) - 1);
}




/* ─────────────────────────────────────────────────────────────────────────────
 * ERROR HANDLING
 * ───────────────────────────────────────────────────────────────────────────*/

void xeno_vm_error(XenoVM *vm, const char *fmt, ...) {
    va_list args;
    va_start(args, fmt);
    vsnprintf(vm->error, sizeof(vm->error), fmt, args);
    va_end(args);
    vm->had_error = true;
}

void xeno_vm_print_error(const XenoVM *vm) {
    printf("[XenoScript Error] %s\n", vm->error);
}


/* ─────────────────────────────────────────────────────────────────────────────
 * HOST FUNCTION REGISTRY
 * ───────────────────────────────────────────────────────────────────────────*/

int xeno_register_fn_typed(XenoVM *vm, const char *name, XenoHostFn fn,
                           int return_kind, int param_count, int *param_kinds) {
    if (vm->host_fn_count >= XENO_HOST_FN_MAX) return -1;

    HostFnEntry *entry    = &vm->host_fns[vm->host_fn_count];
    entry->fn             = fn;
    entry->param_count    = param_count;
    entry->return_type_kind = return_kind;
    strncpy(entry->name, name, sizeof(entry->name) - 1);
    entry->name[sizeof(entry->name) - 1] = '\0';

    int pcount = param_count < 0 ? 0 : param_count;
    for (int i = 0; i < pcount && i < 16; i++)
        entry->param_type_kinds[i] = param_kinds ? param_kinds[i] : TYPE_INT;

    return vm->host_fn_count++;
}

int xeno_register_fn(XenoVM *vm, const char *name,
                     XenoHostFn fn, int param_count) {
    return xeno_register_fn_typed(vm, name, fn, TYPE_VOID, param_count, NULL);
}


/* ─────────────────────────────────────────────────────────────────────────────
 * STACK HELPERS
 * ───────────────────────────────────────────────────────────────────────────*/

static bool stack_overflow(XenoVM *vm) {
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
#define XTAG_UNKNOWN   0
#define XTAG_VOID      1
#define XTAG_BOOL      2
#define XTAG_INT       3
#define XTAG_FLOAT     4
#define XTAG_STRING    5
#define XTAG_OBJECT    6
#define XTAG_ENUM      7
#define XTAG_CLASS_REF 8
#define XTAG_SBYTE     9
#define XTAG_BYTE      10
#define XTAG_SHORT     11
#define XTAG_USHORT    12
#define XTAG_UINT      13
#define XTAG_LONG      14
#define XTAG_ULONG     15
#define XTAG_DOUBLE    16
#define XTAG_CHAR      17
#define XTAG_ANY       18
#define XTAG_ARRAY     19

static const char *xeno_type_name(uint8_t tag) {
    switch (tag) {
        case XTAG_BOOL:   return "bool";
        case XTAG_INT:    return "int";
        case XTAG_FLOAT:  return "float";
        case XTAG_STRING: return "string";
        case XTAG_SBYTE:  return "sbyte";
        case XTAG_BYTE:   return "byte";
        case XTAG_SHORT:  return "short";
        case XTAG_USHORT: return "ushort";
        case XTAG_UINT:   return "uint";
        case XTAG_LONG:   return "long";
        case XTAG_ULONG:  return "ulong";
        case XTAG_DOUBLE: return "double";
        case XTAG_CHAR:   return "char";
        case XTAG_ENUM:   return "enum";
        case XTAG_ARRAY:  return "array";
        case XTAG_VOID:   return "void";
        default:          return "unknown";
    }
}

static bool xeno_type_is_primitive(uint8_t tag) {
    switch (tag) {
        case XTAG_BOOL: case XTAG_INT:   case XTAG_FLOAT:  case XTAG_DOUBLE:
        case XTAG_SBYTE: case XTAG_BYTE: case XTAG_SHORT:  case XTAG_USHORT:
        case XTAG_UINT:  case XTAG_LONG: case XTAG_ULONG:  case XTAG_CHAR:
        case XTAG_STRING: return true;
        default: return false;
    }
}

/* Build a XenoType object. Name is a static string or a malloc'd one.
 * For OBJECT/CLASS we pass a class_name; for ARRAY we pass element tag.
 * tag2 is used for arrays (element type tag) and objects (ignored). */
static XenoType *xeno_make_type(uint8_t tag, const char *override_name) {
    XenoType *t = malloc(sizeof(XenoType));
    if (!t) return NULL;
    t->name         = override_name ? override_name : xeno_type_name(tag);
    t->is_array     = (tag == XTAG_ARRAY);
    t->is_primitive = xeno_type_is_primitive(tag);
    t->is_enum      = (tag == XTAG_ENUM);
    t->is_class     = (tag == XTAG_OBJECT || tag == XTAG_CLASS_REF);
    return t;
}

/* Check whether a Value matches a given type tag at runtime.
 * For IS/AS we use the static type tag baked in by the compiler — this is
 * a compile-time type that we verify is sensible, not a full RTTI check.
 * For objects/arrays we do a quick structural check. */
static bool xeno_value_is_type(Value v, uint8_t tag) {
    switch (tag) {
        case XTAG_BOOL:   return true; /* bools are always bool */
        case XTAG_INT: case XTAG_SBYTE: case XTAG_BYTE:
        case XTAG_SHORT:  case XTAG_USHORT: case XTAG_UINT:
        case XTAG_LONG:   case XTAG_ULONG:  return true; /* int-family */
        case XTAG_FLOAT:  case XTAG_DOUBLE: return true; /* float-family */
        case XTAG_STRING: return v.s != NULL;
        case XTAG_ARRAY:  return v.arr != NULL;
        case XTAG_OBJECT: case XTAG_CLASS_REF:
                          return v.obj != NULL;
        default:          return false;
    }
}

static XenoResult xeno_execute(XenoVM *vm) {

    /* The current call frame — updated on every call/return */
    CallFrame *frame = &vm->frames[vm->frame_count - 1];

    for (;;) {

        /* ── FETCH ────────────────────────────────────────────────────── */
        uint8_t instruction = READ_BYTE();

        /* ── DECODE + EXECUTE ─────────────────────────────────────────── */
        switch ((OpCode)instruction) {

            /* ── Constants ──────────────────────────────────────────── */
            case OP_LOAD_CONST_INT: {
                uint16_t idx = READ_U16();
                if (stack_overflow(vm)) RUNTIME_ERROR("Stack overflow");
                PUSH(frame->chunk->constants.values[idx]);
                break;
            }

            case OP_LOAD_CONST_FLOAT: {
                uint16_t idx = READ_U16();
                if (stack_overflow(vm)) RUNTIME_ERROR("Stack overflow");
                PUSH(frame->chunk->constants.values[idx]);
                break;
            }

            case OP_LOAD_CONST_BOOL: {
                uint8_t val = READ_BYTE();
                if (stack_overflow(vm)) RUNTIME_ERROR("Stack overflow");
                PUSH(val_bool(val != 0));
                break;
            }

            case OP_LOAD_CONST_STR: {
                uint16_t idx = READ_U16();
                if (stack_overflow(vm)) RUNTIME_ERROR("Stack overflow");
                PUSH(frame->chunk->constants.values[idx]);
                break;
            }

            /* ── Local variables ────────────────────────────────────── */
            case OP_LOAD_LOCAL: {
                uint8_t slot = READ_BYTE();
                if (stack_overflow(vm)) RUNTIME_ERROR("Stack overflow");
                PUSH(frame->slots[slot]);
                break;
            }

            case OP_STORE_LOCAL: {
                uint8_t slot = READ_BYTE();
                frame->slots[slot] = POP();
                break;
            }

            /* ── Integer arithmetic ─────────────────────────────────── */
            case OP_ADD_INT:    BINARY_OP(i, +,  val_int);   break;
            case OP_SUB_INT:    BINARY_OP(i, -,  val_int);   break;
            case OP_MUL_INT:    BINARY_OP(i, *,  val_int);   break;
            case OP_MOD_INT: {
                Value b = POP(); Value a = POP();
                if (b.i == 0) RUNTIME_ERROR("Modulo by zero");
                PUSH(val_int(a.i % b.i));
                break;
            }
            case OP_DIV_INT: {
                Value b = POP(); Value a = POP();
                if (b.i == 0) RUNTIME_ERROR("Division by zero");
                PUSH(val_int(a.i / b.i));
                break;
            }
            case OP_NEGATE_INT: {
                Value a = POP();
                PUSH(val_int(-a.i));
                break;
            }

            /* ── Float arithmetic ───────────────────────────────────── */
            case OP_ADD_FLOAT:   BINARY_OP(f, +, val_float); break;
            case OP_SUB_FLOAT:   BINARY_OP(f, -, val_float); break;
            case OP_MUL_FLOAT:   BINARY_OP(f, *, val_float); break;
            case OP_DIV_FLOAT:   BINARY_OP(f, /, val_float); break;
            case OP_MOD_FLOAT: {
                /* C doesn't have % for floats — use fmod */
                Value b = POP(); Value a = POP();
                extern double fmod(double, double);
                PUSH(val_float(fmod(a.f, b.f)));
                break;
            }
            case OP_NEGATE_FLOAT: {
                Value a = POP();
                PUSH(val_float(-a.f));
                break;
            }

            /* ── String concatenation ───────────────────────────────── */
            case OP_CONCAT_STR: {
                Value b = POP(); Value a = POP();
                /* Allocate a new string for the result.
                 * In the full VM this would go through a string intern table.
                 * For now: simple malloc. This leaks in long-running scripts
                 * — marked as a known TODO for the string GC pass. */
                size_t la = a.s ? strlen(a.s) : 0;
                size_t lb = b.s ? strlen(b.s) : 0;
                char *result = malloc(la + lb + 1);
                if (a.s) memcpy(result, a.s, la);
                if (b.s) memcpy(result + la, b.s, lb);
                result[la + lb] = '\0';
                PUSH(val_str(result));
                break;
            }

            case OP_TO_STR: {
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
                switch (kind) {
                    case 4: { /* enum — look up member name by value */
                        int idx = (int)v.i;
                        ClassDef *ecls = NULL;
                        if (enum_class_idx < 0xFF && (int)enum_class_idx < vm->module->class_count) {
                            ecls = &vm->module->classes[enum_class_idx];
                            if (!ecls->is_enum) ecls = NULL;
                        }
                        /* Search by value (not position) to handle explicit values */
                        const char *found_name = NULL;
                        if (ecls) {
                            for (int mi = 0; mi < ecls->enum_member_count; mi++) {
                                if (ecls->enum_member_values[mi] == idx) {
                                    found_name = ecls->enum_member_names[mi];
                                    break;
                                }
                            }
                        }
                        if (found_name) {
                            s = malloc(strlen(found_name) + 1);
                            strcpy(s, found_name);
                        } else {
                            snprintf(buf, sizeof(buf), "%lld", (long long)v.i);
                            s = malloc(strlen(buf) + 1); strcpy(s, buf);
                        }
                        break;
                    }
                    case 0: /* int */
                        snprintf(buf, sizeof(buf), "%lld", (long long)v.i);
                        s = malloc(strlen(buf) + 1);
                        strcpy(s, buf);
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
                    case 3: { /* char -- encode codepoint as UTF-8 */
                        uint32_t cp = (uint32_t)v.i;
                        if (cp < 0x80) {
                            s = malloc(2); s[0] = (char)cp; s[1] = 0;
                        } else if (cp < 0x800) {
                            s = malloc(3);
                            s[0] = (char)(0xC0 | (cp >> 6));
                            s[1] = (char)(0x80 | (cp & 0x3F)); s[2] = 0;
                        } else if (cp < 0x10000) {
                            s = malloc(4);
                            s[0] = (char)(0xE0 | (cp >> 12));
                            s[1] = (char)(0x80 | ((cp >> 6) & 0x3F));
                            s[2] = (char)(0x80 | (cp & 0x3F)); s[3] = 0;
                        } else {
                            s = malloc(5);
                            s[0] = (char)(0xF0 | (cp >> 18));
                            s[1] = (char)(0x80 | ((cp >> 12) & 0x3F));
                            s[2] = (char)(0x80 | ((cp >> 6) & 0x3F));
                            s[3] = (char)(0x80 | (cp & 0x3F)); s[4] = 0;
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
            case OP_NOT_BOOL: {
                Value a = POP();
                PUSH(val_bool(!a.b));
                break;
            }
            case OP_AND_BOOL: BINARY_OP(b, &&, val_bool); break;
            case OP_OR_BOOL:  BINARY_OP(b, ||, val_bool); break;

            /* ── Integer comparisons ────────────────────────────────── */
            case OP_CMP_EQ_INT:  BINARY_OP(i, ==, val_bool); break;
            case OP_CMP_NEQ_INT: BINARY_OP(i, !=, val_bool); break;
            case OP_CMP_LT_INT:  BINARY_OP(i, <,  val_bool); break;
            case OP_CMP_LTE_INT: BINARY_OP(i, <=, val_bool); break;
            case OP_CMP_GT_INT:  BINARY_OP(i, >,  val_bool); break;
            case OP_CMP_GTE_INT: BINARY_OP(i, >=, val_bool); break;

            /* ── Float comparisons ──────────────────────────────────── */
            case OP_CMP_EQ_FLOAT:  BINARY_OP(f, ==, val_bool); break;
            case OP_CMP_NEQ_FLOAT: BINARY_OP(f, !=, val_bool); break;
            case OP_CMP_LT_FLOAT:  BINARY_OP(f, <,  val_bool); break;
            case OP_CMP_LTE_FLOAT: BINARY_OP(f, <=, val_bool); break;
            case OP_CMP_GT_FLOAT:  BINARY_OP(f, >,  val_bool); break;
            case OP_CMP_GTE_FLOAT: BINARY_OP(f, >=, val_bool); break;

            /* ── Bool/string equality ───────────────────────────────── */
            case OP_CMP_EQ_BOOL:  BINARY_OP(b, ==, val_bool); break;
            case OP_CMP_NEQ_BOOL: BINARY_OP(b, !=, val_bool); break;
            case OP_CMP_EQ_STR: {
                Value b = POP(); Value a = POP();
                bool eq = (a.s && b.s) ? strcmp(a.s, b.s) == 0
                                       : a.s == b.s;
                PUSH(val_bool(eq));
                break;
            }
            case OP_CMP_NEQ_STR: {
                Value b = POP(); Value a = POP();
                bool eq = (a.s && b.s) ? strcmp(a.s, b.s) == 0
                                       : a.s == b.s;
                PUSH(val_bool(!eq));
                break;
            }

            /* ── Stack management ───────────────────────────────────── */
            case OP_POP:
                --vm->sp;
                break;

            /* ── Control flow ───────────────────────────────────────── */
            case OP_JUMP: {
                int16_t offset = READ_I16();
                frame->ip += offset;
                break;
            }

            case OP_JUMP_IF_FALSE: {
                int16_t offset = READ_I16();
                Value cond = POP();
                if (!cond.b) frame->ip += offset;
                break;
            }

            /* ── Script function calls ──────────────────────────────── */
            case OP_CALL: {
                uint16_t fn_idx = READ_U16();
                uint8_t  argc   = READ_BYTE();

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
                new_frame->ip    = callee->code;
                memset(new_frame->slots, 0, sizeof(new_frame->slots));

                /* Copy arguments from the value stack into the new frame's
                 * parameter slots. Arguments were pushed left-to-right,
                 * so argv[0] is deepest, argv[argc-1] is on top. */
                for (int i = argc - 1; i >= 0; i--) {
                    new_frame->slots[i] = POP();
                }

                /* Switch execution to the new frame */
                frame = new_frame;
                break;
            }

            /* ── Return from script function ────────────────────────── */
            case OP_RETURN: {
                Value result = POP();    /* The return value */
                vm->frame_count--;

                if (vm->frame_count == 0) {
                    /* Returned from the top-level entry point — done */
                    PUSH(result);
                    return XENO_OK;
                }

                /* Restore the caller's frame and push the return value */
                frame = &vm->frames[vm->frame_count - 1];
                PUSH(result);
                break;
            }

            case OP_RETURN_VOID: {
                bool was_constructor = frame->chunk->is_constructor;
                vm->frame_count--;

                if (vm->frame_count == 0) {
                    /* Top-level void function returned — done */
                    return XENO_OK;
                }

                frame = &vm->frames[vm->frame_count - 1];

                /* Constructors: the object was already pushed by OP_NEW before
                 * the constructor frame was set up. Don't push a dummy — it would
                 * sit on top of the object and confuse STORE_LOCAL. */
                if (!was_constructor) {
                    PUSH(val_int(0));
                }
                break;
            }

            case OP_MATCH_FAIL:
                RUNTIME_ERROR("No matching case in match statement");

            /* Array operations */
            case OP_NEW_ARRAY: {
                int64_t len = POP().i;
                if (len < 0) RUNTIME_ERROR("Array length cannot be negative (%lld)", (long long)len);
                if (len > 1000000) RUNTIME_ERROR("Array length too large (%lld)", (long long)len);
                XenoArray *arr = malloc(sizeof(XenoArray) + (size_t)len * sizeof(Value));
                if (!arr) RUNTIME_ERROR("Out of memory allocating array of length %lld", (long long)len);
                arr->length = (int)len;
                /* Zero-initialize all elements */
                for (int i = 0; i < (int)len; i++)
                    arr->elements[i].i = 0;
                PUSH(val_arr(arr));
                break;
            }
            case OP_ARRAY_LIT: {
                uint8_t count = READ_BYTE();
                XenoArray *arr = malloc(sizeof(XenoArray) + count * sizeof(Value));
                if (!arr) RUNTIME_ERROR("Out of memory allocating array literal");
                arr->length = count;
                /* Elements are on stack in order (e0 pushed first, eN last).
                 * We need to copy them out in reverse since stack is LIFO. */
                for (int i = count - 1; i >= 0; i--)
                    arr->elements[i] = POP();
                PUSH(val_arr(arr));
                break;
            }
            case OP_ARRAY_GET: {
                int64_t idx = POP().i;
                XenoArray *arr = POP().arr;
                if (!arr) RUNTIME_ERROR("Null array dereference");
                if (idx < 0 || idx >= arr->length)
                    RUNTIME_ERROR("Array index %lld out of bounds (length %d)",
                                  (long long)idx, arr->length);
                PUSH(arr->elements[(int)idx]);
                break;
            }
            case OP_ARRAY_SET: {
                Value val    = POP();
                int64_t idx  = POP().i;
                XenoArray *arr = POP().arr;
                if (!arr) RUNTIME_ERROR("Null array dereference");
                if (idx < 0 || idx >= arr->length)
                    RUNTIME_ERROR("Array index %lld out of bounds (length %d)",
                                  (long long)idx, arr->length);
                arr->elements[(int)idx] = val;
                break;
            }
            case OP_IS_TYPE: {
                /* [type_tag]  ( val -- bool ) */
                uint8_t tag = READ_BYTE();
                Value v = POP();
                PUSH(val_bool(xeno_value_is_type(v, tag)));
                break;
            }

            case OP_AS_TYPE: {
                /* [type_tag]  ( val -- val )  runtime error if incompatible */
                uint8_t tag = READ_BYTE();
                Value v = POP();
                if (!xeno_value_is_type(v, tag))
                    RUNTIME_ERROR("Cast failed: value is not a %s", xeno_type_name(tag));
                PUSH(v);
                break;
            }

            case OP_TYPEOF: {
                /* [type_tag][name_len][name_bytes...]
                 * name_len=0 means use built-in name for tag. */
                uint8_t tag     = READ_BYTE();
                uint8_t name_len = READ_BYTE();
                char *tname = NULL;
                if (name_len > 0) {
                    tname = malloc(name_len + 1);
                    if (!tname) RUNTIME_ERROR("Out of memory");
                    for (int ni = 0; ni < name_len; ni++)
                        tname[ni] = (char)READ_BYTE();
                    tname[name_len] = '\0';
                }
                Value v = POP();
                (void)v; /* value not needed for type construction */
                XenoType *t = xeno_make_type(tag, tname);
                if (!t) RUNTIME_ERROR("Out of memory allocating Type object");
                PUSH(val_type(t));
                break;
            }
            case OP_TYPE_FIELD: {
                /* [field_id]  0=name 1=isArray 2=isPrimitive 3=isEnum 4=isClass */
                uint8_t fid = READ_BYTE();
                Value v = POP();
                XenoType *t = v.type;
                if (!t) RUNTIME_ERROR("Null Type dereference");
                switch (fid) {
                    case 0: { /* .name */
                        char *s = malloc(strlen(t->name) + 1);
                        if (!s) RUNTIME_ERROR("Out of memory");
                        strcpy(s, t->name);
                        PUSH(val_str(s));
                        break;
                    }
                    case 1: PUSH(val_bool(t->is_array));     break; /* .isArray */
                    case 2: PUSH(val_bool(t->is_primitive)); break; /* .isPrimitive */
                    case 3: PUSH(val_bool(t->is_enum));      break; /* .isEnum */
                    case 4: PUSH(val_bool(t->is_class));     break; /* .isClass */
                    default: RUNTIME_ERROR("Unknown Type field %d", fid);
                }
                break;
            }

            case OP_ARRAY_LEN: {
                XenoArray *arr = POP().arr;
                if (!arr) RUNTIME_ERROR("Null array dereference");
                PUSH(val_int(arr->length));
                break;
            }

            /* Integer truncation: wrap to narrow type width */
            case OP_TRUNC_I8: {
                int64_t v = POP().i;
                PUSH(val_int((int64_t)(int8_t)(v & 0xFF)));
                break;
            }
            case OP_TRUNC_U8: {
                int64_t v = POP().i;
                PUSH(val_int((int64_t)(uint8_t)(v & 0xFF)));
                break;
            }
            case OP_TRUNC_I16: {
                int64_t v = POP().i;
                PUSH(val_int((int64_t)(int16_t)(v & 0xFFFF)));
                break;
            }
            case OP_TRUNC_U16: {
                int64_t v = POP().i;
                PUSH(val_int((int64_t)(uint16_t)(v & 0xFFFF)));
                break;
            }
            case OP_TRUNC_I32: {
                int64_t v = POP().i;
                PUSH(val_int((int64_t)(int32_t)(v & 0xFFFFFFFF)));
                break;
            }
            case OP_TRUNC_U32: {
                int64_t v = POP().i;
                PUSH(val_int((int64_t)(uint32_t)(v & 0xFFFFFFFF)));
                break;
            }
            case OP_TRUNC_U64: {
                int64_t v = POP().i;
                PUSH(val_int((int64_t)(uint64_t)v));
                break;
            }
            case OP_TRUNC_CHAR: {
                int64_t v = POP().i;
                PUSH(val_int((int64_t)((uint32_t)v & 0x10FFFF)));
                break;
            }

            /* ── Host function calls ────────────────────────────────── */
            case OP_CALL_HOST: {
                uint16_t host_idx = READ_U16();
                uint8_t  argc     = READ_BYTE();

                if (host_idx >= (uint16_t)vm->host_fn_count)
                    RUNTIME_ERROR("Invalid host function index %d", host_idx);

                HostFnEntry *entry = &vm->host_fns[host_idx];

                /* Validate argument count (unless variadic) */
                if (entry->param_count >= 0 && argc != entry->param_count)
                    RUNTIME_ERROR("Host function '%s' expects %d arg(s), got %d",
                                  entry->name, entry->param_count, argc);

                /* Collect arguments from the stack into a temporary array.
                 * Stack order: argv[0] deepest, argv[argc-1] on top. */
                Value argv[32];   /* Max 32 args to a host function */
                if (argc > 32) RUNTIME_ERROR("Too many arguments to host function");
                for (int i = argc - 1; i >= 0; i--)
                    argv[i] = POP();

                /* Call the host function */
                Value result = val_int(0);  /* Default return value */
                XenoResult hr = entry->fn(vm, argc, argv, &result);
                if (hr != XENO_OK) return hr;

                /* Push the return value (even for void — it'll be POP'd) */
                PUSH(result);
                break;
            }

            /* ── Object opcodes ─────────────────────────────────────────── */

            case OP_NEW: {
                uint16_t class_idx = READ_U16();
                uint8_t  argc      = READ_BYTE();

                if (class_idx >= (uint16_t)vm->module->class_count)
                    RUNTIME_ERROR("Invalid class index %d", class_idx);

                ClassDef *cls = &vm->module->classes[class_idx];

                /* Allocate the object — fixed header + one Value per field */
                XenoObject *obj = malloc(
                    sizeof(XenoObject) + cls->field_count * sizeof(Value));
                if (!obj) RUNTIME_ERROR("Out of memory allocating '%s'", cls->name);

                obj->class_def = cls;

                /* Zero-initialize all fields */
                for (int i = 0; i < cls->field_count; i++)
                    obj->fields[i] = val_int(0);

                /* If there's a constructor, call it.
                 * Push 'this' as slot 0, then the args (already on stack). */
                if (cls->constructor_index >= 0) {
                    /* Args are already on the stack in order.
                     * We need to insert 'this' before them in the new frame.
                     * Collect args first, then set up frame. */
                    if (argc > 32) RUNTIME_ERROR("Too many constructor arguments");
                    Value args[32];
                    for (int i = argc - 1; i >= 0; i--)
                        args[i] = POP();

                    if (vm->frame_count >= XENO_FRAME_MAX)
                        RUNTIME_ERROR("Stack overflow in constructor");

                    Chunk *ctor_chunk = &vm->module->chunks[cls->constructor_index];
                    CallFrame *ctor_frame = &vm->frames[vm->frame_count++];
                    ctor_frame->chunk = ctor_chunk;
                    ctor_frame->ip    = ctor_chunk->code;

                    /* Slot 0 = this, slots 1..argc = args */
                    ctor_frame->slots[0] = val_obj(obj);
                    for (int i = 0; i < argc; i++)
                        ctor_frame->slots[i + 1] = args[i];

                    frame = &vm->frames[vm->frame_count - 1];

                    /* Constructor runs and will hit OP_RETURN_VOID.
                     * That will pop the frame and push a dummy — we'll POP it. */
                }

                /* Push the new object */
                PUSH(val_obj(obj));
                break;
            }

            case OP_GET_FIELD: {
                uint8_t field_idx = READ_BYTE();
                Value v = POP();
                XenoObject *obj = v.obj;
                if (!obj) RUNTIME_ERROR("Null object dereference");
                if (field_idx >= obj->class_def->field_count)
                    RUNTIME_ERROR("Field index %d out of range", field_idx);
                PUSH(obj->fields[field_idx]);
                break;
            }

            case OP_SET_FIELD: {
                uint8_t field_idx = READ_BYTE();
                Value val = POP();
                Value v   = POP();
                XenoObject *obj = v.obj;
                if (!obj) RUNTIME_ERROR("Null object dereference");
                if (field_idx >= obj->class_def->field_count)
                    RUNTIME_ERROR("Field index %d out of range", field_idx);
                obj->fields[field_idx] = val;
                break;
            }

            case OP_LOAD_THIS: {
                /* 'this' is always slot 0 in a method/constructor frame */
                PUSH(frame->slots[0]);
                break;
            }

            case OP_LOAD_STATIC: {
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

            case OP_STORE_STATIC: {
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

            case OP_CALL_IFACE: {
                /* Virtual dispatch through an interface-typed reference.
                 * name_const_idx holds the method name as a string constant.
                 * We look up the method on the ACTUAL runtime class_def. */
                uint16_t name_const_idx = READ_U16();
                uint8_t  argc           = READ_BYTE();

                if (argc > 32) RUNTIME_ERROR("Too many method arguments");
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
                while (search_cls && safety-- > 0) {
                    for (int i = 0; i < search_cls->method_count; i++) {
                        if (strcmp(search_cls->methods[i].name, mname) == 0 &&
                            !search_cls->methods[i].is_static) {
                            iface_fn_idx = search_cls->methods[i].fn_index;
                            break;
                        }
                    }
                    if (iface_fn_idx >= 0) break;
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
                method_frame->ip    = method_chunk->code;

                method_frame->slots[0] = obj_val;
                for (int i = 0; i < argc; i++)
                    method_frame->slots[i + 1] = args[i];

                frame = &vm->frames[vm->frame_count - 1];
                break;
            }

            case OP_CALL_METHOD: {
                uint16_t fn_idx = READ_U16();
                uint8_t  argc   = READ_BYTE();

                /* The object ('this') sits below the args on the stack.
                 * Stack layout: ... obj arg0 arg1 ... argN   (argN on top)
                 * We collect args, then grab the object. */
                if (argc > 32) RUNTIME_ERROR("Too many method arguments");
                Value args[32];
                for (int i = argc - 1; i >= 0; i--)
                    args[i] = POP();
                Value obj_val = POP();

                if (fn_idx >= (uint16_t)vm->module->count)
                    RUNTIME_ERROR("Invalid method function index %d", fn_idx);

                if (vm->frame_count >= XENO_FRAME_MAX)
                    RUNTIME_ERROR("Stack overflow in method call");

                Chunk *method_chunk = &vm->module->chunks[fn_idx];
                CallFrame *method_frame = &vm->frames[vm->frame_count++];
                method_frame->chunk = method_chunk;
                method_frame->ip    = method_chunk->code;

                /* Slot 0 = this, slots 1..argc = args */
                method_frame->slots[0] = obj_val;
                for (int i = 0; i < argc; i++)
                    method_frame->slots[i + 1] = args[i];

                frame = &vm->frames[vm->frame_count - 1];
                break;
            }

            case OP_CALL_SUPER: {
                uint16_t fn_idx = READ_U16();
                uint8_t  argc   = READ_BYTE();

                /* super() — args are on the stack, 'this' is slot 0 of the
                 * CURRENT frame (not on the stack). We reuse the same object. */
                if (argc > 32) RUNTIME_ERROR("Too many super() arguments");
                Value args[32];
                for (int i = argc - 1; i >= 0; i--)
                    args[i] = POP();

                if (fn_idx >= (uint16_t)vm->module->count)
                    RUNTIME_ERROR("Invalid super constructor index %d", fn_idx);

                if (vm->frame_count >= XENO_FRAME_MAX)
                    RUNTIME_ERROR("Stack overflow in super() call");

                Chunk *ctor_chunk = &vm->module->chunks[fn_idx];
                CallFrame *ctor_frame = &vm->frames[vm->frame_count++];
                ctor_frame->chunk = ctor_chunk;
                ctor_frame->ip    = ctor_chunk->code;

                /* Slot 0 = same 'this' as the calling constructor's slot 0 */
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

XenoResult xeno_vm_run(XenoVM *vm, Module *module) {
    vm->module    = module;
    vm->had_error = false;
    vm->sp        = vm->stack;
    vm->frame_count = 0;

    /* ── Merge stdlib pool into this module ──────────────────────────────
     * Graft all loaded stdlib chunks into the running module so calls
     * to stdlib functions (e.g. int_toString, List.add) resolve correctly. */
    for (int i = 0; i < vm->stdlib_module_count; i++)
        module_merge(module, &vm->stdlib_modules[i]);

    /* ── Run static initializers ─────────────────────────────────────────
     * If the module has a __sinit__ chunk, run it now to initialize all
     * static fields. This must happen before any entry point runs. */
    if (module->sinit_index >= 0 && module->sinit_index < module->count) {
        Chunk *sinit_chunk = &module->chunks[module->sinit_index];
        if (sinit_chunk->count > 0) {
            if (vm->frame_count >= XENO_FRAME_MAX) {
                xeno_vm_error(vm, "Stack overflow running static initializers");
                return XENO_RUNTIME_ERROR;
            }
            CallFrame *frame = &vm->frames[vm->frame_count++];
            frame->chunk = sinit_chunk;
            frame->ip    = sinit_chunk->code;
            memset(frame->slots, 0, sizeof(Value) * (sinit_chunk->local_count > 0
                                                       ? sinit_chunk->local_count : 1));
            XenoResult r = xeno_execute(vm);
            if (r != XENO_OK) return r;
        }
    }

    /* Determine entry point.
     * If the module has @Mod, instantiate the entry class, run constructor,
     * then call main() on the instance.
     * If no @Mod is present this is a library -- nothing to run. */
    if (module->metadata.has_mod) {
        const char *entry_class = module->metadata.entry_class;
        int ci = module_find_class(module, entry_class);
        if (ci < 0) {
            xeno_vm_error(vm, "@Mod entry class '%s' not found in module", entry_class);
            return XENO_RUNTIME_ERROR;
        }
        ClassDef *cls = &module->classes[ci];

        /* Allocate the mod object */
        XenoObject *obj = malloc(sizeof(XenoObject) + cls->field_count * sizeof(Value));
        if (!obj) {
            xeno_vm_error(vm, "Out of memory allocating @Mod class '%s'", entry_class);
            return XENO_RUNTIME_ERROR;
        }
        obj->class_def = cls;
        for (int i = 0; i < cls->field_count; i++)
            obj->fields[i] = val_int(0);

        /* Run constructor if present */
        if (cls->constructor_index >= 0) {
            if (vm->frame_count >= XENO_FRAME_MAX) {
                free(obj);
                xeno_vm_error(vm, "Stack overflow starting @Mod constructor");
                return XENO_RUNTIME_ERROR;
            }
            Chunk *ctor_chunk = &module->chunks[cls->constructor_index];
            CallFrame *ctor_frame = &vm->frames[vm->frame_count++];
            ctor_frame->chunk = ctor_chunk;
            ctor_frame->ip    = ctor_chunk->code;
            memset(ctor_frame->slots, 0, sizeof(ctor_frame->slots));
            ctor_frame->slots[0] = val_obj(obj);
            XenoResult r = xeno_execute(vm);
            if (r != XENO_OK) return r;
        }

        /* Find main() method on the mod class */
        int main_mi = -1;
        for (int mi = 0; mi < cls->method_count; mi++) {
            if (strcmp(cls->methods[mi].name, "main") == 0) {
                main_mi = mi;
                break;
            }
        }
        if (main_mi < 0) {
            /* No main() — constructor-only entry point is valid.
             * The constructor already ran above; nothing more to do. */
            return XENO_OK;
        }

        int main_chunk_idx = cls->methods[main_mi].fn_index;
        if (main_chunk_idx < 0 || main_chunk_idx >= module->count) {
            xeno_vm_error(vm, "@Mod class '%s': main() has no bytecode", entry_class);
            return XENO_RUNTIME_ERROR;
        }
        if (vm->frame_count >= XENO_FRAME_MAX) {
            xeno_vm_error(vm, "Stack overflow calling @Mod main()");
            return XENO_RUNTIME_ERROR;
        }
        Chunk *main_chunk = &module->chunks[main_chunk_idx];
        CallFrame *frame  = &vm->frames[vm->frame_count++];
        frame->chunk      = main_chunk;
        frame->ip         = main_chunk->code;
        memset(frame->slots, 0, sizeof(frame->slots));
        frame->slots[0]   = val_obj(obj);
        return xeno_execute(vm);
    }

    /* No @Mod -- this is a library, nothing to run */
    return XENO_OK;
}

XenoResult xeno_vm_run_source(XenoVM *vm, const char *source) {
    /* Free any previous source module (hot reload resets state) */
    if (vm->has_source_module) {
        module_free(&vm->source_module);
        vm->has_source_module = false;
    }

    /* Run the full compiler pipeline */
    Lexer    lexer;
    Parser   parser;
    Checker *checker  = malloc(sizeof(Checker));
    Compiler compiler;
    CompilerHostTable host_table;

    if (!checker) {
        xeno_vm_error(vm, "Out of memory");
        return XENO_RUNTIME_ERROR;
    }

    lexer_init(&lexer, source);
    parser_init(&parser, &lexer);
    checker_init(checker, &parser.arena);
    compiler_host_table_init(&host_table);
    module_init(&vm->source_module);

    /* Resolve imports and load stdlib — same pipeline as xenoc */
    Module staging; module_init(&staging);
    bool import_err = false;
    char *merged = pipeline_prepare(source, NULL, &staging, &import_err);
    if (import_err || !merged) {
        free(merged);
        module_free(&staging);
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
    for (int i = 0; i < vm->host_fn_count; i++) {
        HostFnEntry *e = &vm->host_fns[i];

        Type ret_type  = { .kind = e->return_type_kind };
        Type params[MAX_PARAMS];
        int  pcount    = e->param_count < 0 ? 0 : e->param_count;
        if  (pcount > MAX_PARAMS) pcount = MAX_PARAMS;

        for (int p = 0; p < pcount; p++)
            params[p].kind = e->param_type_kinds[p];

        checker_declare_host(checker, e->name, ret_type, params, pcount);

        /* Use add_any if any param is TYPE_ANY so the compiler emits OP_TO_STR */
        bool has_any = false;
        for (int p = 0; p < e->param_count; p++)
            if (e->param_type_kinds[p] == TYPE_ANY) { has_any = true; break; }
        if (has_any)
            compiler_host_table_add_any(&host_table, e->name, i, pcount);
        else
            compiler_host_table_add(&host_table, e->name, i, pcount);
    }

    /* Declare stdlib classes/functions to checker from staging */
    pipeline_declare_staging(checker, &staging);

    /* Merge staging into source_module so compiler finds stdlib indices */
    module_merge(&vm->source_module, &staging);
    module_free(&staging);

    /* Parse */
    Program program = parser_parse(&parser);
    if (parser.had_error) {
        xeno_vm_error(vm, "Parse error at line %d: %s",
                      parser.errors[0].line,
                      parser.errors[0].message);
        parser_free(&parser);
        free(merged);
        free(checker);
        return XENO_COMPILE_ERROR;
    }

    /* Type check */
    bool checker_ok = checker_check(checker, &program);
    /* Print any warnings regardless of errors */
    for (int _wi = 0; _wi < checker->error_count; _wi++) {
        if (checker->errors[_wi].is_warning)
            fprintf(stderr, "[line %d] Warning: %s\n",
                    checker->errors[_wi].line, checker->errors[_wi].message);
    }
    if (!checker_ok) {
        xeno_vm_error(vm, "Type error at line %d: %s",
                      checker->errors[0].line,
                      checker->errors[0].message);
        parser_free(&parser);
        free(merged);
        free(checker);
        return XENO_COMPILE_ERROR;
    }

    /* Compile */
    if (!compiler_compile(&compiler, &program, &vm->source_module, &host_table)) {
        xeno_vm_error(vm, "Compile error at line %d: %s",
                      compiler.errors[0].line,
                      compiler.errors[0].message);
        parser_free(&parser);
        free(merged);
        free(checker);
        return XENO_COMPILE_ERROR;
    }

    parser_free(&parser);
    free(merged);
    free(checker);
    vm->has_source_module = true;

    /* Execute */
    return xeno_vm_run(vm, &vm->source_module);
}