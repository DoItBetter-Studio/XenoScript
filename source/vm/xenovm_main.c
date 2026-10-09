/*
 * xenovm_main.c — XenoScript standalone VM
 *
 * Usage:
 *   xenovm <script.xeno>     Compile and run from source
 *   xenovm <script.xbc>      Load and run pre-compiled bytecode
 *   xenovm <mod.xar>         Load and run a built mod archive
 *   xenovm <project-dir/>    Build-and-run a project directory
 *   xenovm --debug [--break N]... <path>
 *   xenovm --help
 *
 * Exit codes:
 *   0  Success
 *   1  Runtime or compile error
 *   2  I/O error
 *   3  Usage error
 */

#define _DEFAULT_SOURCE
#include "vm.h"
#include "xbc.h"
#include "xdbg.h"
#include "xar.h"
#include "toml.h"
#include "stdlib_xar.h"
#include "compiler.h"
#include "../../source/stdlib/stdlib_register.h"
#include "../../source/compiler/compile_pipeline.h"
#include "platform_time.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <dirent.h>
#include <time.h>

uint64_t ts_start_ns, ts_end_ns;

/* ── Host functions ───────────────────────────────────────────────────── */

static XenoResult std_print(XenoVM *vm, int argc, Value *argv, Value *out) {
    (void)vm; (void)argc; (void)out;
    if (argv[0].is_null) { printf("null\n"); return XENO_OK; }
    if (!argv[0].s) { printf("(null)\n"); return XENO_OK; }
    printf("%s\n", argv[0].s);
    return XENO_OK;
}

static void register_std_fns(XenoVM *vm) {
    int p_any[1] = { TYPE_ANY };
    xeno_register_fn_typed(vm, "print", std_print, TYPE_VOID, 1, p_any);
    stdlib_register_host_fns(vm);
}

/* ── Helpers ──────────────────────────────────────────────────────────── */

static void print_usage(void) {
    printf("Usage: xenovm [options] <script.xeno|script.xbc|mod.xar|project-dir/>\n");
    printf("       xenovm --help\n\n");
    printf("  .xeno  Compile and run from source\n");
    printf("  .xbc   Run pre-compiled bytecode\n");
    printf("  .xar   Run a built mod archive\n");
    printf("  dir/   Build-and-run a project directory (needs xeno.project)\n");
    printf("\nDebugger options:\n");
    printf("  --debug          Enable the interactive line debugger\n");
    printf("  --break <line>         Break on source line (any file)\n");
    printf("  --break <file>:<line>  Break only in that source file\n");
    printf("  --debug-sinit    Also stop inside __sinit__ static initializers\n");
    printf("\nWhen stopped, type a command and press Enter:\n");
    printf("  c / continue     Resume until the next breakpoint\n");
    printf("  s / step         Step over (next line at this depth)\n");
    printf("  i / stepin       Step in (enter calls)\n");
    printf("  o / stepout      Step out (leave current frame)\n");
    printf("  bt / stack       Print call stack\n");
    printf("  locals           Print locals in the current frame\n");
    printf("  fields           Print fields of `this` (slot 0)\n");
    printf("  q / quit         Abort execution\n");
    printf("\nNote: there is no 'build' subcommand. To run a built mod:\n");
    printf("  xenovm path/to/mod.xar\n");
    printf("To build a project, use xenoc:\n");
    printf("  xenoc build path/to/project/\n");
}

/* ── Interactive stdin debugger ───────────────────────────────────────── */

static int g_debug_abort = 0;

static void xenovm_debug_print_stack(XenoVM *vm)
{
    int n = xeno_vm_debug_frame_count(vm);
    fprintf(stderr, "[xeno debug] call stack (%d frame%s):\n", n, n == 1 ? "" : "s");
    for (int d = 0; d < n; d++) {
        XenoDebugFrame fr;
        if (!xeno_vm_debug_frame_at(vm, d, &fr))
            continue;
        const char *fn = fr.fn_name && fr.fn_name[0] ? fr.fn_name : "?";
        if (fr.source_file && fr.source_file[0])
            fprintf(stderr, "  #%d  %s (%s:%d)\n", d, fn, fr.source_file, fr.line);
        else
            fprintf(stderr, "  #%d  %s — line %d\n", d, fn, fr.line);
    }
}

static void xenovm_debug_print_locals(XenoVM *vm)
{
    int n = xeno_vm_debug_local_count(vm, 0);
    fprintf(stderr, "[xeno debug] locals (%d slot%s):\n", n, n == 1 ? "" : "s");
    for (int i = 0; i < n; i++) {
        XenoDebugLocal loc;
        if (!xeno_vm_debug_local_at(vm, 0, i, &loc))
            continue;
        if (loc.name)
            fprintf(stderr, "  [%d] %s = %s\n", loc.slot, loc.name, loc.summary);
        else
            fprintf(stderr, "  [%d] local_%d = %s\n", loc.slot, loc.slot, loc.summary);
    }
}

static void xenovm_debug_print_fields(XenoVM *vm)
{
    int n = xeno_vm_debug_field_count(vm, 0, 0);
    if (n == 0) {
        fprintf(stderr, "[xeno debug] fields: (no object in slot 0 / this)\n");
        return;
    }
    fprintf(stderr, "[xeno debug] fields of this (%d):\n", n);
    for (int i = 0; i < n; i++) {
        XenoDebugField f;
        if (!xeno_vm_debug_field_at(vm, 0, 0, i, &f))
            continue;
        const char *flags = "";
        if (f.is_static && f.is_final)
            flags = " (static final)";
        else if (f.is_static)
            flags = " (static)";
        else if (f.is_final)
            flags = " (final)";
        fprintf(stderr, "  %s%s = %s\n", f.name ? f.name : "?", flags, f.summary);
    }
}

static void xenovm_debug_on_break(XenoVM *vm)
{
    int line = xeno_vm_debug_hit_line(vm);
    int off  = xeno_vm_debug_hit_offset(vm);
    const char *fn = vm->debug_hit_fn[0] ? vm->debug_hit_fn : "?";
    const char *sf = vm->debug_hit_file[0] ? vm->debug_hit_file : NULL;
    if (sf)
        fprintf(stderr, "\n[xeno debug] %s (%s:%d)\n", fn, sf, line);
    else
        fprintf(stderr, "\n[xeno debug] %s — line %d\n", fn, line);
    (void)off;
    fprintf(stderr, "[xeno debug] (c)ontinue (s)tep (i)n (o)ut (bt) (locals) (fields) (q)uit > ");
    fflush(stderr);

    char buf[128];
    for (;;) {
        if (!fgets(buf, sizeof(buf), stdin)) {
            /* EOF — treat as continue so pipes don't hang forever */
            xeno_vm_debug_continue(vm);
            return;
        }
        /* trim */
        char *p = buf;
        while (*p == ' ' || *p == '\t') p++;
        if (*p == '\n' || *p == '\0') {
            fprintf(stderr, "[xeno debug] (c)ontinue (s)tep (i)n (o)ut (bt) (locals) (fields) (q)uit > ");
            fflush(stderr);
            continue;
        }
        if (p[0] == 'c' || strncmp(p, "continue", 8) == 0) {
            xeno_vm_debug_continue(vm);
            return;
        }
        if (p[0] == 's' || strncmp(p, "step", 4) == 0) {
            xeno_vm_debug_step_over(vm);
            return;
        }
        if (p[0] == 'i' || strncmp(p, "stepin", 6) == 0) {
            xeno_vm_debug_step_in(vm);
            return;
        }
        if (p[0] == 'o' || strncmp(p, "stepout", 7) == 0) {
            xeno_vm_debug_step_out(vm);
            return;
        }
        if (strncmp(p, "bt", 2) == 0 || strncmp(p, "stack", 5) == 0) {
            xenovm_debug_print_stack(vm);
            fprintf(stderr, "[xeno debug] (c)ontinue (s)tep (i)n (o)ut (bt) (locals) (fields) (q)uit > ");
            fflush(stderr);
            continue;
        }
        if (strncmp(p, "locals", 6) == 0) {
            xenovm_debug_print_locals(vm);
            fprintf(stderr, "[xeno debug] (c)ontinue (s)tep (i)n (o)ut (bt) (locals) (fields) (q)uit > ");
            fflush(stderr);
            continue;
        }
        if (strncmp(p, "fields", 6) == 0 || p[0] == 'f') {
            xenovm_debug_print_fields(vm);
            fprintf(stderr, "[xeno debug] (c)ontinue (s)tep (i)n (o)ut (bt) (locals) (fields) (q)uit > ");
            fflush(stderr);
            continue;
        }
        if (p[0] == 'q' || strncmp(p, "quit", 4) == 0) {
            g_debug_abort = 1;
            /* Leave paused so the VM aborts with a stop error */
            return;
        }
        fprintf(stderr, "[xeno debug] unknown command — use c, s, i, o, bt, locals, fields, or q\n");
        fprintf(stderr, "[xeno debug] (c)ontinue (s)tep (i)n (o)ut (bt) (locals) (fields) (q)uit > ");
        fflush(stderr);
    }
}

static bool has_ext(const char *path, const char *ext) {
    const char *dot = strrchr(path, '.');
    return dot && strcmp(dot, ext) == 0;
}

static bool path_is_dir(const char *path) {
    struct stat st;
    return stat(path, &st) == 0 && S_ISDIR(st.st_mode);
}

/* ── Run modes ────────────────────────────────────────────────────────── */

static void try_load_xdbg(XenoVM *vm, Module *module, const char *artifact)
{
    if (!vm || !module || !artifact || !vm->debug_enabled)
        return;
    char dbg_path[1024];
    if (!xdbg_path_from_artifact(artifact, dbg_path, sizeof(dbg_path)))
        return;
    if (xdbg_load(module, dbg_path))
        fprintf(stderr, "xenovm: loaded debug symbols '%s'\n", dbg_path);
}

static int run_xbc(XenoVM *vm, const char *path) {
    Module *user = (Module*)calloc(1, sizeof(Module)); module_init(user);
    XbcResult xr = xbc_read(user, path);
    if (xr != XBC_OK) {
        fprintf(stderr, "xenovm: failed to load '%s': %s\n", path, xbc_result_str(xr));
        module_free(user); free(user);
        return 2;
    }
    try_load_xdbg(vm, user, path);

    Module *module = malloc(sizeof(Module));
    if (!module) { module_free(user); free(user); return 1; }
    module_init(module);

    /* Always pre-seed stdlib in fixed order (core → collections → math → ...).
     * The compiler stages stdlib in this same order, so OP_CALL/OP_NEW indices
     * baked into the stripped XBC are always valid. Unconditional — no
     * uses_stdlib check — determinism is the guarantee, not savings. */
    for (int i = 0; i < vm->stdlib_module_count; i++)
        module_merge(module, vm->stdlib_modules[i]);

    /* Copy sinit chunk before merging user */
    if (user->sinit_index >= 0 && user->sinit_index < user->count) {
        Chunk *src = &user->chunks[user->sinit_index];
        int si = module_add_chunk(module);
        if (si >= 0) {
            Chunk *dst = &module->chunks[si];
            for (int bi = 0; bi < src->count; bi++) chunk_write(dst, src->code[bi], 0);
            for (int ci = 0; ci < src->constants.count; ci++)
                chunk_copy_constant(dst, src, ci);
            dst->local_count = src->local_count;
            dst->param_count = src->param_count;
            strncpy(module->names[si], "__sinit__", 63);
            module->sinit_index = si;
        }
    }
    module_merge(module, user);
    module_free(user); free(user);

    /* Extract ModMetadata now that user classes are fully merged in */
    module_extract_mod_metadata(module, NULL);

    ts_start_ns = xeno_time_ns();
    XenoResult r = xeno_vm_run(vm, module);
    ts_end_ns = xeno_time_ns();
    module_free(module);
    free(module);
    return (r == XENO_OK) ? 0 : 1;
}

static int run_source(XenoVM *vm, const char *path) {
    char *source = pipeline_read_file(path);
    if (!source) return 2;

    ts_start_ns = xeno_time_ns();
    xeno_vm_set_compile_source_path(vm, path);
    XenoResult r = xeno_vm_run_source(vm, source);
    ts_end_ns = xeno_time_ns();
    free(source);
    return (r == XENO_OK) ? 0 : 1;
}

static int run_xar(XenoVM *vm, const char *path) {
    XarArchive ar;
    XarResult xr = xar_read(&ar, path);
    if (xr != XAR_OK) {
        fprintf(stderr, "xenovm: failed to load '%s': %s\n",
                path, xar_result_str(xr));
        return 2;
    }

    /* Determine the directory containing this .xar, used as a fallback dep
     * search location when vm->mod_path is not set. */
    char base_dir[512] = "";
    const char *last_sep = strrchr(path, '/');
#ifdef _WIN32
    const char *last_bs = strrchr(path, '\\');
    if (last_bs && (!last_sep || last_bs > last_sep)) last_sep = last_bs;
#endif
    if (last_sep) {
        size_t dlen = (size_t)(last_sep - path + 1);
        if (dlen < sizeof(base_dir)) {
            memcpy(base_dir, path, dlen);
            base_dir[dlen] = '\0';
        }
    }

    /* Load declared dependencies into the VM pool.
     * Once compiled to .xar, a dependency is just another mod — it lives in
     * the same flat pool as every other .xar, not in a special deps/ subfolder.
     * Search order: 1) vm->mod_path  2) same directory as this .xar */
    for (int i = 0; i < ar.manifest.dep_count; i++) {
        const char *dep_name = ar.manifest.dependencies[i];

        /* Skip if already in the pool */
        bool already_loaded = false;
        for (int j = 0; j < vm->stdlib_module_count; j++) {
            if (strcmp(vm->stdlib_loaded_names[j], dep_name) == 0) {
                already_loaded = true; break;
            }
        }
        if (already_loaded) continue;

        char dep_path[1024];
        bool found = false;
        if (vm->mod_path[0]) {
            snprintf(dep_path, sizeof(dep_path), "%s%s.xar", vm->mod_path, dep_name);
            FILE *p = fopen(dep_path, "rb");
            if (p) { fclose(p); found = true; }
        }
        if (!found) {
            snprintf(dep_path, sizeof(dep_path), "%s%s.xar", base_dir, dep_name);
            FILE *p = fopen(dep_path, "rb");
            if (p) { fclose(p); found = true; }
        }
        if (!found) {
            /* Also check a deps/ subdirectory relative to the XAR location
             * (useful during development when running from a project folder) */
            snprintf(dep_path, sizeof(dep_path), "%sdeps/%s.xar", base_dir, dep_name);
            FILE *p = fopen(dep_path, "rb");
            if (p) { fclose(p); found = true; }
        }
        if (!found) {
            /* Check deps/ relative to parent of XAR's directory */
            char parent[512] = "";
            snprintf(parent, sizeof(parent), "%s", base_dir);
            size_t plen = strlen(parent);
            /* strip trailing slash, then go up one level */
            if (plen > 0 && (parent[plen-1]=='/'||parent[plen-1]=='\\')) parent[--plen]='\0';
            char *last = strrchr(parent, '/');
            if (!last) last = strrchr(parent, '\\');
            if (last) {
                *(last+1) = '\0';
                snprintf(dep_path, sizeof(dep_path), "%sdeps/%s.xar", parent, dep_name);
                FILE *p = fopen(dep_path, "rb");
                if (p) { fclose(p); found = true; }
            }
        }
        if (!found) {
            fprintf(stderr, "xenovm: missing dependency '%s'\n", dep_name);
            xar_archive_free(&ar);
            return 2;
        }

        XarArchive dep;
        if (xar_read(&dep, dep_path) != XAR_OK) {
            fprintf(stderr, "xenovm: failed to read dep '%s' from '%s'\n",
                    dep_name, dep_path);
            xar_archive_free(&ar);
            return 2;
        }
        if (!xeno_vm_load_xar(vm, &dep)) {
            fprintf(stderr, "xenovm: failed to load dep '%s'\n", dep_name);
            xar_archive_free(&dep);
            xar_archive_free(&ar);
            return 1;
        }
        fprintf(stderr, "xenovm: loaded dep '%s' from '%s'\n", dep_name, dep_path);
        xar_archive_free(&dep);
    }
    if (ar.manifest.dep_count == 0) {
        fprintf(stderr,
                "xenovm: note: '%s' declares no dependencies — "
                "stripped library classes will not be resolved\n",
                path);
    }

    /* Load the main archive as a standalone runnable module.
     * Build in load order: stdlib → deps → user mod, matching compile-time
     * staging so all class indices are naturally correct. */
    Module *module = malloc(sizeof(Module));
    if (!module) { xar_archive_free(&ar); return 1; }
    module_init(module);

    /* Always pre-seed stdlib in fixed order. Stdlib indices are baked into
     * every XBC at compile time in this same order, so module_merge
     * deduplicates by name and user indices stay stable. */
    for (int i = 0; i < vm->stdlib_module_count; i++)
        module_merge(module, vm->stdlib_modules[i]);

    /* Merge user XAR chunks on top */
    for (int i = 0; i < ar.chunk_count; i++) {
        Module *cm = (Module*)calloc(1, sizeof(Module)); module_init(cm);
        if (xbc_read_mem(cm, ar.chunks[i].data, ar.chunks[i].size) == XBC_OK) {
            try_load_xdbg(vm, cm, path);
            module_merge(module, cm);
            module_free(cm); free(cm);
        }
    }

    /* Extract ModMetadata from @Mod attribute on merged classes */
    module_extract_mod_metadata(module, NULL);

    ts_start_ns = xeno_time_ns();
    XenoResult r = xeno_vm_run(vm, module);
    ts_end_ns = xeno_time_ns();
    module_free(module);
    free(module);
    xar_archive_free(&ar);
    return (r == XENO_OK) ? 0 : 1;
}


/* Collect .xeno files recursively from a directory. Returns 0 on success. */
static int vm_collect_sources(const char *dir,
                               char *paths[], int *count, int cap) {
    DIR *d = opendir(dir);
    if (!d) return 1;
    struct dirent *ent;
    while ((ent = readdir(d)) != NULL) {
        if (ent->d_name[0] == '.') continue;
        char full[1024];
        snprintf(full, sizeof(full), "%s/%s", dir, ent->d_name);
        struct stat st2;
        if (stat(full, &st2) != 0) continue;
        if (S_ISDIR(st2.st_mode)) {
            if (vm_collect_sources(full, paths, count, cap) != 0) {
                closedir(d); return 1;
            }
        } else {
            const char *dot = strrchr(ent->d_name, '.');
            if (dot && strcmp(dot, ".xeno") == 0) {
                if (*count < cap) paths[(*count)++] = strdup(full);
            }
        }
    }
    closedir(d);
    return 0;
}

/* Build-and-run a project directory — merges all src/ files like xenoc build. */
static int run_project(XenoVM *vm, const char *project_dir) {
    /* Read xeno.project */
    char proj_path[512];
    snprintf(proj_path, sizeof(proj_path), "%s/xeno.project", project_dir);
    char *proj_toml = pipeline_read_file(proj_path);
    if (!proj_toml) {
        fprintf(stderr, "xenovm: cannot open '%s'\n", proj_path);
        return 2;
    }

    XarManifest manifest;
    char toml_err[256] = {0};
    if (!xar_manifest_from_toml(&manifest, proj_toml, toml_err, sizeof(toml_err))) {
        fprintf(stderr, "xenovm: %s\n", toml_err);
        free(proj_toml); return 1;
    }
    free(proj_toml);

    /* Load dependencies into VM pool */
    char deps_dir[512];
    snprintf(deps_dir, sizeof(deps_dir), "%s/deps", project_dir);
    for (int i = 0; i < manifest.dep_count; i++) {
        char dep_path[1024];
        snprintf(dep_path, sizeof(dep_path), "%s/%s.xar",
                 deps_dir, manifest.dependencies[i]);
        XarArchive dep;
        if (xar_read(&dep, dep_path) != XAR_OK) {
            fprintf(stderr, "xenovm: missing dependency '%s'\n",
                    manifest.dependencies[i]);
            return 2;
        }
        xeno_vm_load_xar(vm, &dep);
        xar_archive_free(&dep);
    }

    /* Collect all .xeno sources from src/ */
    char src_dir[512];
    snprintf(src_dir, sizeof(src_dir), "%s/src", project_dir);

    #define VM_MAX_SRCS 512
    char *source_paths[VM_MAX_SRCS];
    int   source_count = 0;
    if (vm_collect_sources(src_dir, source_paths, &source_count,
                           VM_MAX_SRCS) != 0 || source_count == 0) {
        fprintf(stderr, "xenovm: no .xeno files found in '%s'\n", src_dir);
        return 1;
    }

    /* Merge all sources into one string through the pipeline */
    size_t merged_cap = 131072, merged_len = 0;
    char  *merged_all = malloc(merged_cap);
    if (!merged_all) { return 1; }
    merged_all[0] = '\0';

    /* Use a shared staging module across all files */
    Module *staging = (Module*)calloc(1, sizeof(Module)); module_init(staging);
    bool any_err = false;

    for (int i = 0; i < source_count && !any_err; i++) {
        char *src = pipeline_read_file(source_paths[i]);
        if (!src) { any_err = true; break; }

        bool imp_err = false;
        char *file_merged = pipeline_prepare_project(src, source_paths[i],
                                                     deps_dir, staging, &imp_err);
        free(src);
        if (imp_err || !file_merged) { free(file_merged); any_err = true; break; }

        size_t flen = strlen(file_merged);
        while (merged_len + flen + 2 > merged_cap) {
            merged_cap *= 2;
            char *tmp = realloc(merged_all, merged_cap);
            if (!tmp) { free(file_merged); any_err = true; break; }
            merged_all = tmp;
        }
        memcpy(merged_all + merged_len, file_merged, flen);
        merged_len += flen;
        merged_all[merged_len++] = '\n';
        merged_all[merged_len]   = '\0';
        free(file_merged);
    }
    char primary_source[512] = "";
    if (source_count == 1 && source_paths[0])
        snprintf(primary_source, sizeof(primary_source), "%s", source_paths[0]);
    for (int i = 0; i < source_count; i++) free(source_paths[i]);
    module_free(staging); free(staging);

    if (any_err) { free(merged_all); return 1; }

    ts_start_ns = xeno_time_ns();
    if (primary_source[0])
        xeno_vm_set_compile_source_path(vm, primary_source);
    XenoResult r = xeno_vm_run_source(vm, merged_all);
    ts_end_ns = xeno_time_ns();
    free(merged_all);
    return (r == XENO_OK) ? 0 : 1;
}

/* ── Main ─────────────────────────────────────────────────────────────── */

int main(int argc, char **argv) {
    if (argc < 2) { print_usage(); return 3; }

    bool debug = false;
    int breaks[XENO_BREAKPOINT_MAX];
    char break_files[XENO_BREAKPOINT_MAX][128];
    int break_count = 0;
    const char *path = NULL;

    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--help") == 0 || strcmp(argv[i], "-h") == 0) {
            print_usage();
            return 0;
        }
        if (strcmp(argv[i], "build") == 0) {
            fprintf(stderr,
                    "xenovm: there is no 'build' subcommand.\n"
                    "  Build with:  xenoc build <project-dir>\n"
                    "  Run with:    xenovm <mod.xar>   or   xenovm <project-dir>\n");
            return 3;
        }
        if (strcmp(argv[i], "--debug") == 0) {
            debug = true;
            continue;
        }
        if (strcmp(argv[i], "--break") == 0 || strncmp(argv[i], "--break=", 8) == 0) {
            const char *arg;
            if (strncmp(argv[i], "--break=", 8) == 0)
                arg = argv[i] + 8;
            else {
                if (i + 1 >= argc) {
                    fprintf(stderr, "xenovm: --break requires <line> or <file>:<line>\n");
                    return 3;
                }
                arg = argv[++i];
            }
            /* Parse optional file:line */
            const char *colon = strrchr(arg, ':');
            int line = 0;
            char filebuf[128];
            filebuf[0] = '\0';
            if (colon && colon > arg && colon[1]) {
                /* Could be Windows drive C:\... or file:line — only split if after colon is digits */
                int all_digit = 1;
                for (const char *p = colon + 1; *p; p++) {
                    if (*p < '0' || *p > '9') { all_digit = 0; break; }
                }
                if (all_digit) {
                    size_t flen = (size_t)(colon - arg);
                    if (flen >= sizeof(filebuf)) flen = sizeof(filebuf) - 1;
                    memcpy(filebuf, arg, flen);
                    filebuf[flen] = '\0';
                    /* basename only */
                    const char *base = filebuf;
                    const char *sl = strrchr(filebuf, '/');
#ifdef _WIN32
                    const char *bs = strrchr(filebuf, '\\');
                    if (bs && (!sl || bs > sl)) sl = bs;
#endif
                    if (sl) base = sl + 1;
                    if (base != filebuf)
                        memmove(filebuf, base, strlen(base) + 1);
                    line = atoi(colon + 1);
                } else {
                    line = atoi(arg);
                }
            } else {
                line = atoi(arg);
            }
            if (line <= 0) {
                fprintf(stderr, "xenovm: invalid breakpoint '%s'\n", arg);
                return 3;
            }
            if (break_count < XENO_BREAKPOINT_MAX) {
                breaks[break_count] = line;
                snprintf(break_files[break_count], sizeof(break_files[break_count]),
                         "%s", filebuf);
                break_count++;
            }
            debug = true; /* --break implies --debug */
            continue;
        }
        if (argv[i][0] == '-') {
            fprintf(stderr, "xenovm: unknown option '%s'\n", argv[i]);
            return 3;
        }
        if (path) {
            fprintf(stderr, "xenovm: unexpected argument '%s'\n", argv[i]);
            return 3;
        }
        path = argv[i];
    }

    if (!path) {
        print_usage();
        return 3;
    }

    XenoVM *vm = malloc(sizeof(XenoVM));
    if (!vm) { fprintf(stderr, "xenovm: out of memory\n"); return 1; }

    xeno_vm_init(vm);
    register_std_fns(vm);
    xeno_vm_load_stdlib(vm);

    if (debug) {
        xeno_vm_debug_enable(vm, true);
        xeno_vm_debug_set_callback(vm, xenovm_debug_on_break);
        for (int i = 0; i < break_count; i++) {
            if (break_files[i][0])
                xeno_vm_debug_add_breakpoint_file(vm, breaks[i], break_files[i]);
            else
                xeno_vm_debug_add_breakpoint(vm, breaks[i]);
        }
        /* No breakpoints: stop on the first source line so the session starts usefully. */
        if (break_count == 0)
            xeno_vm_debug_step_over(vm);
        fprintf(stderr, "xenovm: debugger on");
        if (break_count > 0) {
            fprintf(stderr, " — breakpoints:");
            for (int i = 0; i < break_count; i++) {
                if (break_files[i][0])
                    fprintf(stderr, " %s:%d", break_files[i], breaks[i]);
                else
                    fprintf(stderr, " %d", breaks[i]);
            }
        } else {
            fprintf(stderr, " — stopping at first source line");
        }
        fprintf(stderr, "\n");
    }

    clock_t start = clock();        // start timing

    int exit_code;

    if (path_is_dir(path)) {
        exit_code = run_project(vm, path);
    } else if (has_ext(path, ".xar")) {
        exit_code = run_xar(vm, path);
    } else if (has_ext(path, ".xbc")) {
        exit_code = run_xbc(vm, path);
    } else if (has_ext(path, ".xeno")) {
        exit_code = run_source(vm, path);
    } else {
        fprintf(stderr,
                "xenovm: unknown file type '%s' (expected .xeno, .xbc, .xar, or dir)\n",
                path);
        xeno_vm_free(vm);
        free(vm);
        return 3;
    }

    clock_t end = clock();          // stop timing

    double ms = (double)(end - start) * 1000.0 / CLOCKS_PER_SEC;
    double exec_time = (ts_end_ns - ts_start_ns) / 1e6;
    fprintf(stdout, "VM time: %.6f ms\nExec time: %.16f ms\n", ms, exec_time);

    if (exit_code != 0 && exit_code != 2) {
        xeno_vm_print_error(vm);
    }

    xeno_vm_free(vm);
    free(vm);
    return exit_code;
}