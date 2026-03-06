/*
 * xenovm_main.c — XenoScript standalone VM
 *
 * Usage:
 *   xenovm <script.xeno>     Compile and run from source (hot-reload dev mode)
 *   xenovm <script.xbc>      Load and run pre-compiled bytecode
 *   xenovm --help
 *
 * The VM registers a standard set of host functions for standalone use:
 *   print_int(int)
 *   print_float(float)
 *   print_bool(bool)
 *   print_str(string)
 *
 * When embedding in a game engine you would NOT use this main — you'd link
 * vm.c directly and register your own host functions.
 *
 * Exit codes:
 *   0  Success
 *   1  Runtime or compile error
 *   2  I/O error
 *   3  Usage error
 */

#include "vm.h"
#include "xbc.h"
#include "xar.h"
#include "stdlib_xar.h"
#include "../../source/stdlib/stdlib_register.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ─────────────────────────────────────────────────────────────────────────────
 * STANDARD HOST FUNCTIONS
 * Available in all scripts run via xenovm.
 * A game engine would replace these with its own functions.
 * ───────────────────────────────────────────────────────────────────────────*/

static XenoResult std_print(XenoVM *vm, int argc, Value *argv, Value *out) {
    (void)vm; (void)argc; (void)out;
    /* By the time we're called, the compiler has already converted the
     * argument to a string via OP_TO_STR, so argv[0].s is always valid. */
    printf("%s\n", argv[0].s ? argv[0].s : "");
    return XENO_OK;
}

static void register_std_fns(XenoVM *vm) {
    int p_any[1] = { TYPE_ANY };
    xeno_register_fn_typed(vm, "print", std_print, TYPE_VOID, 1, p_any);
    /* Register all stdlib host functions (math, etc.) at indices 1..N */
    stdlib_register_host_fns(vm);
}

static void print_usage(void) {
    printf("Usage: xenovm <script.xeno|script.xbc>\n");
    printf("       xenovm --help\n");
    printf("\n");
    printf("  .xeno files are compiled from source before execution.\n");
    printf("  .xbc  files are loaded directly (no compiler required).\n");
}

/* Read entire file into a heap-allocated string. Caller must free(). */
static char *read_file(const char *path) {
    FILE *f = fopen(path, "rb");
    if (!f) { fprintf(stderr, "xenovm: cannot open '%s'\n", path); return NULL; }

    fseek(f, 0, SEEK_END);
    long size = ftell(f);
    fseek(f, 0, SEEK_SET);

    char *buf = malloc(size + 1);
    if (!buf) { fclose(f); return NULL; }

    size_t nread = fread(buf, 1, size, f);
    if (nread != (size_t)size) { free(buf); fclose(f); return NULL; }
    buf[size] = '\0';
    fclose(f);
    return buf;
}

/* Determine file type by extension */
static bool has_extension(const char *path, const char *ext) {
    const char *dot = strrchr(path, '.');
    return dot && strcmp(dot, ext) == 0;
}

int main(int argc, char **argv) {
    if (argc < 2) { print_usage(); return 3; }
    if (strcmp(argv[1], "--help") == 0) { print_usage(); return 0; }

    const char *path = argv[1];

    XenoVM *vm = malloc(sizeof(XenoVM));
    if (!vm) { fprintf(stderr, "xenovm: out of memory\n"); return 1; }

    xeno_vm_init(vm);
    register_std_fns(vm);
    /* Load core stdlib (always) and pre-warm the stdlib pool */
    xeno_vm_load_stdlib(vm);

    XenoResult result;

    if (has_extension(path, ".xbc")) {
        /* Load pre-compiled bytecode */
        Module *module = malloc(sizeof(Module));
        if (!module) { free(vm); return 1; }

        XbcResult xr = xbc_read(module, path);
        if (xr != XBC_OK) {
            fprintf(stderr, "xenovm: failed to load '%s': %s\n",
                    path, xbc_result_str(xr));
            free(module);
            free(vm);
            return 2;
        }

        result = xeno_vm_run(vm, module);
        module_free(module);
        free(module);

    } else if (has_extension(path, ".xeno")) {
        /* Compile from source and run */
        char *source = read_file(path);
        if (!source) { free(vm); return 2; }

        result = xeno_vm_run_source(vm, source);
        free(source);

    } else {
        fprintf(stderr, "xenovm: unknown file type '%s' (expected .xeno or .xbc)\n", path);
        free(vm);
        return 3;
    }

    if (result != XENO_OK) {
        xeno_vm_print_error(vm);
        xeno_vm_free(vm);
        free(vm);
        return 1;
    }

    xeno_vm_free(vm);
    free(vm);
    return 0;
}