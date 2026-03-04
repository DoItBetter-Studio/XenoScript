/*
 * vm.h — XenoScript Virtual Machine
 *
 * The VM is intentionally self-contained. Its only external dependencies are:
 *   bytecode.h  — Value, OpCode, Chunk, Module (the execution substrate)
 *   compiler.h  — only needed for vm_run_source (dev/hot-reload path)
 *
 * The VM knows nothing about lexers, parsers, or ASTs. In a release build
 * you could compile the VM without compiler.c/parser.c/etc. entirely.
 *
 * EXECUTION MODEL:
 *   - Stack-based: operands pushed, results popped
 *   - Call frames: one per active function, holds local variable slots
 *   - Host API: registered C functions callable from script via CALL_HOST
 *
 * SANDBOXING:
 *   Scripts can only do what opcodes allow. The only way out of the sandbox
 *   is OP_CALL_HOST, and you control exactly which host functions are registered.
 */

#ifndef VM_H
#define VM_H

#include "bytecode.h"
#include "compiler.h"
#include <stdbool.h>
#include <stdint.h>

/* ─────────────────────────────────────────────────────────────────────────────
 * CONFIGURATION
 * ───────────────────────────────────────────────────────────────────────────*/
#define XENO_STACK_MAX       256   /* Maximum values on the value stack      */
#define XENO_FRAME_MAX       64    /* Maximum call depth (recursion limit)   */
#define XENO_LOCALS_MAX      64    /* Maximum locals per frame               */
#define XENO_HOST_FN_MAX     256   /* Maximum registered host functions      */


/* ─────────────────────────────────────────────────────────────────────────────
 * RESULT CODES
 * ───────────────────────────────────────────────────────────────────────────*/
typedef enum {
    XENO_OK,              /* Execution completed successfully               */
    XENO_RUNTIME_ERROR,   /* Runtime error (div by zero, stack overflow...) */
    XENO_COMPILE_ERROR,   /* Source compilation failed (vm_run_source only) */
} XenoResult;


/* ─────────────────────────────────────────────────────────────────────────────
 * HOST FUNCTION API
 *
 * A host function is a C function registered by the game engine that scripts
 * can call via OP_CALL_HOST. This is the ONLY interface between the sandbox
 * and the outside world.
 *
 * Signature:
 *   XenoResult my_fn(struct XenoVM *vm, int argc, Value *argv, Value *out);
 *
 *   argc     — number of arguments passed by the script
 *   argv     — array of argument values (argv[0] is the first argument)
 *   out      — write the return value here (leave untouched for void)
 *
 * Return XENO_OK on success, XENO_RUNTIME_ERROR on failure.
 * On error, call xeno_vm_error() to set an error message before returning.
 * ───────────────────────────────────────────────────────────────────────────*/
typedef struct XenoVM XenoVM;
typedef XenoResult (*XenoHostFn)(XenoVM *vm, int argc, Value *argv, Value *out);

typedef struct {
    XenoHostFn  fn;
    char        name[64];
    int         param_count;      /* -1 means variadic                      */
    int         return_type_kind; /* TypeKind of return value               */
    int         param_type_kinds[16]; /* TypeKind of each parameter         */
} HostFnEntry;


/* ─────────────────────────────────────────────────────────────────────────────
 * CALL FRAME
 *
 * One frame per active function invocation.
 * Frames are stored in a fixed-size stack inside the VM.
 * ───────────────────────────────────────────────────────────────────────────*/
typedef struct {
    Chunk   *chunk;              /* The chunk being executed                */
    uint8_t *ip;                 /* Instruction pointer into chunk->code    */
    Value    slots[XENO_LOCALS_MAX]; /* Local variable slots for this call  */
} CallFrame;


/* ─────────────────────────────────────────────────────────────────────────────
 * THE VM
 * ───────────────────────────────────────────────────────────────────────────*/
struct XenoVM {
    /* Execution state */
    CallFrame   frames[XENO_FRAME_MAX];   /* Call frame stack               */
    int         frame_count;              /* Current frame depth            */

    Value       stack[XENO_STACK_MAX];    /* Value stack                    */
    Value      *sp;                       /* Stack pointer (next free slot) */

    /* The module currently executing */
    Module     *module;                   /* Owned externally, not freed     */

    /* Host function registry */
    HostFnEntry host_fns[XENO_HOST_FN_MAX];
    int         host_fn_count;

    /* Error state */
    char        error[256];               /* Last runtime error message      */
    bool        had_error;

    /* Dev/hot-reload state — compiler owned by VM for source mode */
    Module      source_module;            /* Module compiled from source     */
    bool        has_source_module;
};


/* ─────────────────────────────────────────────────────────────────────────────
 * PUBLIC API
 * ───────────────────────────────────────────────────────────────────────────*/

/* Initialize a VM. Must be called before any other vm function.
 * The VM is small enough to stack-allocate: sizeof(XenoVM) is checked
 * to be sane — see vm.c. */
void xeno_vm_init(XenoVM *vm);

/* Tear down the VM and free any internally owned resources. */
void xeno_vm_free(XenoVM *vm);

/*
 * Register a host (C) function that scripts can call.
 * name       — the function name as it appears in script (e.g. "print")
 * fn         — the C function pointer
 * param_count — expected argument count, or -1 for variadic
 *
 * Returns the host function index (used in OP_CALL_HOST operand),
 * or -1 if the registry is full.
 *
 * NOTE: Host functions must be registered BEFORE running any script that
 * calls them. The compiler resolves host function names at compile time.
 */
/*
 * Register a host function with full type information.
 * return_kind and param_kinds use the TypeKind enum values from ast.h.
 * Use TYPE_VOID for void return. Pass NULL for param_kinds if param_count <= 0.
 */
int xeno_register_fn_typed(XenoVM *vm, const char *name, XenoHostFn fn,
                           int return_kind, int param_count, int *param_kinds);

/* Shorthand: registers with all params as TYPE_INT (for backward compat) */
int xeno_register_fn(XenoVM *vm, const char *name,
                     XenoHostFn fn, int param_count);

/*
 * Execute a pre-compiled Module.
 * Calls the function named "main" as the entry point.
 * Returns XENO_OK on success.
 *
 * The module must remain valid for the lifetime of execution —
 * the VM does not copy it.
 */
XenoResult xeno_vm_run(XenoVM *vm, Module *module);

/*
 * Compile and execute a XenoScript source string.
 * Used for hot-reload (.xeno files) and dev tooling.
 *
 * On reload: call this again with new source — state resets completely.
 * The previous source_module is freed and replaced.
 *
 * Returns XENO_COMPILE_ERROR if compilation fails,
 *         XENO_RUNTIME_ERROR if execution fails,
 *         XENO_OK on success.
 */
XenoResult xeno_vm_run_source(XenoVM *vm, const char *source);

/*
 * Set a runtime error message. Call this from host functions before
 * returning XENO_RUNTIME_ERROR.
 */
void xeno_vm_error(XenoVM *vm, const char *fmt, ...);

/* Print the last error to stdout */
void xeno_vm_print_error(const XenoVM *vm);


/* ─────────────────────────────────────────────────────────────────────────────
 * CONVENIENCE VALUE CONSTRUCTORS (for host function authors)
 * ───────────────────────────────────────────────────────────────────────────*/
static inline Value xeno_int  (int64_t v) { return val_int(v);   }
static inline Value xeno_float(double  v) { return val_float(v); }
static inline Value xeno_bool (bool    v) { return val_bool(v);  }
static inline Value xeno_str  (char   *v) { return val_str(v);   }

#endif /* VM_H */