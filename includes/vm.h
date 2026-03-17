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
#include "xar.h"
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
#define XENO_MAX_TYPE_ARGS 8   /* Maximum generic type params per instantiation */

typedef struct {
    Chunk   *chunk;              /* The chunk being executed                */
    uint8_t *ip;                 /* Instruction pointer into chunk->code    */
    Value    slots[XENO_LOCALS_MAX]; /* Local variable slots for this call  */
    /* Concrete type arguments for generic instantiation (e.g. T=string, K=int).
     * Populated by OP_NEW when constructing a generic class.
     * Indexed by type param position (T=0, K=0, V=1, etc.). */
    uint8_t  type_args[XENO_MAX_TYPE_ARGS];
    uint8_t  type_arg_count;
} CallFrame;


/* ─────────────────────────────────────────────────────────────────────────────
 * THE VM
 * ───────────────────────────────────────────────────────────────────────────*/
/* ── Exception handler frame ─────────────────────────────────────────────── */
#define XENO_HANDLER_MAX 64   /* Maximum nesting depth of try blocks */

typedef struct {
    int      frame_count;   /* call-frame depth when TRY_BEGIN was hit        */
    Value   *sp;            /* value-stack pointer when TRY_BEGIN was hit     */
    uint8_t *catch_ip;      /* instruction pointer to jump to on throw        */
    Chunk   *catch_chunk;   /* chunk the catch_ip lives in                    */
    char     class_name[64];/* exception class name filter ("" = catch all)   */
} ExceptionHandler;

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

    /* Stdlib module pool — merged into every compiled module before execution.
     * Populated by xeno_vm_load_stdlib() from embedded .xar blobs. */
    Module     *stdlib_modules[64];   /* heap-allocated stdlib modules         */
    int         stdlib_module_count;
    char        stdlib_loaded_names[64][XAR_MAX_NAME]; /* dedup by name */

    /* Mod search path — directory scanned for user .xar files at startup */
    char        mod_path[512];

    /* Generic element-kind register — set by OP_ARRAY_GET, read by OP_CMP_EQ_VAL */
    uint8_t     elem_kind_reg;

    /* Exception handler stack */
    ExceptionHandler handlers[XENO_HANDLER_MAX];
    int              handler_count;
    Value            caught_exception; /* the object caught by current catch block */

    /* Error state */
    char        error[256];               /* Last runtime error message      */
    bool        had_error;

    /* Event handler table — maps event names to lists of subscribed handlers.
     * Each handler is either a static/top-level function (receiver.obj == NULL /
     * is_null == true) or a bound delegate (receiver holds the instance). */
#define XENO_MAX_EVENTS        64
#define XENO_MAX_EVENT_HANDLERS 32
    struct {
        char  name[64];
        struct {
            char  fn_name[64];  /* function/method name to resolve at fire time */
            Value receiver;     /* val_null() for static, object for bound      */
        } handlers[XENO_MAX_EVENT_HANDLERS];
        int   handler_count;
        bool  active;
    } event_table[XENO_MAX_EVENTS];
    int event_count;

    /* Pending event fire state — used to fire handlers iteratively without
     * recursive calls to xeno_execute. When OP_EVENT_FIRE starts firing:
     *   pending_event_idx    = index into event_table
     *   pending_handler      = next handler to call (0-based)
     *   pending_fire_args       = saved argument values
     *   pending_fire_argc       = number of saved arguments
     *   pending_fire_resolved[i]  = resolved chunk index for handler i
     *   pending_fire_receivers[i] = receiver for handler i (val_null if static)
     *   pending_fire_active     = true while a fire is in progress */
#define XENO_MAX_EVENT_ARGS 16
    bool  pending_fire_active;
    int   pending_event_idx;
    int   pending_handler;
    int   pending_fire_argc;
    Value pending_fire_args[XENO_MAX_EVENT_ARGS];
    int   pending_fire_resolved[XENO_MAX_EVENT_HANDLERS];  /* chunk indices    */
    Value pending_fire_receivers[XENO_MAX_EVENT_HANDLERS]; /* bound receivers  */
    int   pending_fire_handler_count;
    /* frame count when the event fire was initiated — used to detect handler return */
    int   pending_fire_base_frame;

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
 * Load all auto-loaded stdlib .xar archives into the VM's stdlib module pool.
 * Also loads explicitly imported archives from STDLIB_XAR_TABLE by name.
 * Call this after xeno_vm_init() and after registering all host functions,
 * before running any script.
 *
 * `import_name` may be NULL to load only auto-loaded modules (core).
 * Pass a name like "math" or "collections" to also load that explicit module.
 * Call multiple times for multiple explicit imports — deduplication is handled.
 */
void xeno_vm_load_stdlib(XenoVM *vm);

/*
 * Load a single stdlib module by name into the VM's stdlib pool.
 * Used internally by xeno_vm_run_source when it encounters an import.
 * Returns false if the module name is not found in the embedded table.
 */
bool xeno_vm_load_stdlib_module(XenoVM *vm, const char *name);

/*
 * Load a user mod .xar archive into the VM's module pool.
 * All chunks from the archive are merged into the VM's stdlib pool
 * so that types and functions from the mod are available to other
 * mods that depend on it.
 *
 * Call this for each dependency BEFORE calling xeno_vm_run_source
 * or xeno_vm_run on the mod that depends on it.
 *
 * Returns false if the archive cannot be loaded.
 */
bool xeno_vm_load_xar(XenoVM *vm, const XarArchive *ar);

/*
 * Run a mod that was previously loaded via xeno_vm_load_xar.
 * mod_name must match the archive's manifest name / first export.
 */
XenoResult xeno_vm_run_mod(XenoVM *vm, const char *mod_name);

/*
 * Set a search path for user .xar mod files (e.g. "./mods/").
 * The VM will scan this directory at load time for .xar files.
 * Must be called before xeno_vm_run / xeno_vm_run_source.
 */
void xeno_vm_set_mod_path(XenoVM *vm, const char *path);

/*
 * Compile and execute a XenoScript source string.
 * Used for hot-reload (.xeno files) and dev tooling.
 *
 * On reload: call this again with new source -- state resets completely.
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