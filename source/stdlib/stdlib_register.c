/*
 * stdlib_register.c — Register stdlib host functions with the VM.
 *
 * Linked into xenovm (the standalone VM) and any game engine integration.
 * Requires vm.c — uses XenoVM, XenoResult, xeno_register_fn_typed.
 *
 * Call stdlib_register_host_fns() after xeno_vm_init() and after registering
 * "print" at index 0, before running any script that uses a stdlib module.
 */

#define _DEFAULT_SOURCE  /* log2, round etc. on glibc */
#include "stdlib_register.h"
#include "stdlib_math_table.h"
#include <math.h>
#include <string.h>

/* ── C implementations ───────────────────────────────────────────────── */

static XenoResult fn_abs(XenoVM *vm, int argc, Value *argv, Value *out)
    { (void)vm;(void)argc; int64_t n=argv[0].i; out->i=n<0?-n:n; return XENO_OK; }

static XenoResult fn_absf(XenoVM *vm, int argc, Value *argv, Value *out)
    { (void)vm;(void)argc; out->f=fabs(argv[0].f); return XENO_OK; }

static XenoResult fn_min(XenoVM *vm, int argc, Value *argv, Value *out)
    { (void)vm;(void)argc; int64_t a=argv[0].i,b=argv[1].i; out->i=a<b?a:b; return XENO_OK; }

static XenoResult fn_max(XenoVM *vm, int argc, Value *argv, Value *out)
    { (void)vm;(void)argc; int64_t a=argv[0].i,b=argv[1].i; out->i=a>b?a:b; return XENO_OK; }

static XenoResult fn_minf(XenoVM *vm, int argc, Value *argv, Value *out)
    { (void)vm;(void)argc; double a=argv[0].f,b=argv[1].f; out->f=a<b?a:b; return XENO_OK; }

static XenoResult fn_maxf(XenoVM *vm, int argc, Value *argv, Value *out)
    { (void)vm;(void)argc; double a=argv[0].f,b=argv[1].f; out->f=a>b?a:b; return XENO_OK; }

static XenoResult fn_clamp(XenoVM *vm, int argc, Value *argv, Value *out) {
    (void)vm;(void)argc;
    int64_t v=argv[0].i, lo=argv[1].i, hi=argv[2].i;
    out->i = v<lo ? lo : (v>hi ? hi : v);
    return XENO_OK;
}

static XenoResult fn_clampf(XenoVM *vm, int argc, Value *argv, Value *out) {
    (void)vm;(void)argc;
    double v=argv[0].f, lo=argv[1].f, hi=argv[2].f;
    out->f = v<lo ? lo : (v>hi ? hi : v);
    return XENO_OK;
}

static XenoResult fn_sign(XenoVM *vm, int argc, Value *argv, Value *out)
    { (void)vm;(void)argc; int64_t n=argv[0].i; out->i=n>0?1:(n<0?-1:0); return XENO_OK; }

static XenoResult fn_sqrt(XenoVM *vm, int argc, Value *argv, Value *out)
    { (void)vm;(void)argc; out->f=sqrt(argv[0].f); return XENO_OK; }

static XenoResult fn_pow(XenoVM *vm, int argc, Value *argv, Value *out)
    { (void)vm;(void)argc; out->f=pow(argv[0].f,argv[1].f); return XENO_OK; }

static XenoResult fn_floor(XenoVM *vm, int argc, Value *argv, Value *out)
    { (void)vm;(void)argc; out->f=floor(argv[0].f); return XENO_OK; }

static XenoResult fn_ceil(XenoVM *vm, int argc, Value *argv, Value *out)
    { (void)vm;(void)argc; out->f=ceil(argv[0].f); return XENO_OK; }

static XenoResult fn_round(XenoVM *vm, int argc, Value *argv, Value *out)
    { (void)vm;(void)argc; out->f=round(argv[0].f); return XENO_OK; }

static XenoResult fn_sin(XenoVM *vm, int argc, Value *argv, Value *out)
    { (void)vm;(void)argc; out->f=sin(argv[0].f); return XENO_OK; }

static XenoResult fn_cos(XenoVM *vm, int argc, Value *argv, Value *out)
    { (void)vm;(void)argc; out->f=cos(argv[0].f); return XENO_OK; }

static XenoResult fn_tan(XenoVM *vm, int argc, Value *argv, Value *out)
    { (void)vm;(void)argc; out->f=tan(argv[0].f); return XENO_OK; }

static XenoResult fn_log(XenoVM *vm, int argc, Value *argv, Value *out)
    { (void)vm;(void)argc; out->f=log(argv[0].f); return XENO_OK; }

static XenoResult fn_log2(XenoVM *vm, int argc, Value *argv, Value *out)
    { (void)vm;(void)argc; out->f=log2(argv[0].f); return XENO_OK; }

static XenoResult fn_log10(XenoVM *vm, int argc, Value *argv, Value *out)
    { (void)vm;(void)argc; out->f=log10(argv[0].f); return XENO_OK; }

static XenoResult fn_toFloat(XenoVM *vm, int argc, Value *argv, Value *out)
    { (void)vm;(void)argc; out->f=(double)argv[0].i; return XENO_OK; }

static XenoResult fn_toInt(XenoVM *vm, int argc, Value *argv, Value *out)
    { (void)vm;(void)argc; out->i=(int64_t)argv[0].f; return XENO_OK; }

/* ── Registration table — must be in the same order as STDLIB_MATH_TABLE ── */

static XenoHostFn const STDLIB_MATH_FNS[] = {
    fn_abs, fn_absf, fn_min, fn_max, fn_minf, fn_maxf,
    fn_clamp, fn_clampf, fn_sign,
    fn_sqrt, fn_pow, fn_floor, fn_ceil, fn_round,
    fn_sin, fn_cos, fn_tan, fn_log, fn_log2, fn_log10,
    fn_toFloat, fn_toInt,
};

/* ── Public API ─────────────────────────────────────────────────────────── */

/* ── Array host functions ─────────────────────────────────────────────── */

/* __array_grow(arr, new_cap) -> T[]
 * Reallocates arr to hold at least new_cap elements.
 * Zeros any newly added slots. Returns the (possibly moved) array pointer.
 * Called internally by List<T>, Stack<T>, Queue<T> — not for mod use. */
static XenoResult fn_array_grow(XenoVM *vm, int argc, Value *argv, Value *out) {
    (void)vm; (void)argc;
    XenoArray *arr     = argv[0].arr;
    int        new_cap = (int)argv[1].i;
    if (new_cap <= 0) new_cap = 8;
    if (arr && new_cap <= arr->length) { *out = argv[0]; return XENO_OK; }

    int old_len = arr ? arr->length : 0;
    XenoArray *grown = realloc(arr, sizeof(XenoArray) + (size_t)new_cap * sizeof(Value));
    if (!grown) return XENO_RUNTIME_ERROR;
    grown->length = new_cap;
    for (int i = old_len; i < new_cap; i++)
        grown->elements[i].i = 0;
    out->arr = grown;
    return XENO_OK;
}


void stdlib_register_host_fns(XenoVM *vm) {
    for (int i = 0; i < STDLIB_MATH_COUNT; i++) {
        const StdlibMathEntry *e = &STDLIB_MATH_TABLE[i];
        int kinds[4];
        memcpy(kinds, e->param_kinds, sizeof(kinds));
        xeno_register_fn_typed(vm, e->name, STDLIB_MATH_FNS[i],
                                e->return_kind, e->param_count,
                                e->param_count > 0 ? kinds : NULL);
    }

    /* Array growth — TYPE_ANY accepts any array value */
    int grow_params[2] = { TYPE_ANY, TYPE_INT };
    xeno_register_fn_typed(vm, "__array_grow", fn_array_grow,
                            TYPE_ANY, 2, grow_params);
}
