/*
 * xenovm_main.c — XenoScript standalone VM
 *
 * Usage:
 *   xenovm <script.xeno>     Compile and run from source
 *   xenovm <script.xbc>      Load and run pre-compiled bytecode
 *   xenovm <mod.xar>         Load and run a built mod archive
 *   xenovm <project-dir/>    Build-and-run a project directory
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
#include "xar.h"
#include "toml.h"
#include "stdlib_xar.h"
#include "compiler.h"
#include "../../source/stdlib/stdlib_register.h"
#include "../../source/compiler/compile_pipeline.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <dirent.h>
#include <time.h>

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
    printf("Usage: xenovm <script.xeno|script.xbc|mod.xar|project-dir/>\n");
    printf("       xenovm --help\n\n");
    printf("  .xeno  Compile and run from source\n");
    printf("  .xbc   Run pre-compiled bytecode\n");
    printf("  .xar   Run a built mod archive\n");
    printf("  dir/   Build-and-run a project directory (needs xeno.project)\n");
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

static int run_xbc(XenoVM *vm, const char *path) {
    Module *user = (Module*)calloc(1, sizeof(Module)); module_init(user);
    XbcResult xr = xbc_read(user, path);
    if (xr != XBC_OK) {
        fprintf(stderr, "xenovm: failed to load '%s': %s\n", path, xbc_result_str(xr));
        module_free(user); free(user);
        return 2;
    }

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
                chunk_add_constant(dst, src->constants.values[ci]);
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

    XenoResult r = xeno_vm_run(vm, module);
    module_free(module);
    free(module);
    return (r == XENO_OK) ? 0 : 1;
}

static int run_source(XenoVM *vm, const char *path) {
    char *source = pipeline_read_file(path);
    if (!source) return 2;

    XenoResult r = xeno_vm_run_source(vm, source);
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
        xar_archive_free(&dep);
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
            module_merge(module, cm);
            module_free(cm); free(cm);
        }
    }

    /* Extract ModMetadata from @Mod attribute on merged classes */
    module_extract_mod_metadata(module, NULL);

    XenoResult r = xeno_vm_run(vm, module);
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
    for (int i = 0; i < source_count; i++) free(source_paths[i]);
    module_free(staging); free(staging);

    if (any_err) { free(merged_all); return 1; }

    XenoResult r = xeno_vm_run_source(vm, merged_all);
    free(merged_all);
    return (r == XENO_OK) ? 0 : 1;
}

/* ── Main ─────────────────────────────────────────────────────────────── */

int main(int argc, char **argv) {
    if (argc < 2) { print_usage(); return 3; }
    if (strcmp(argv[1], "--help") == 0) { print_usage(); return 0; }

    const char *path = argv[1];

    XenoVM *vm = malloc(sizeof(XenoVM));
    if (!vm) { fprintf(stderr, "xenovm: out of memory\n"); return 1; }

    xeno_vm_init(vm);
    register_std_fns(vm);
    xeno_vm_load_stdlib(vm);

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
    fprintf(stdout, "Execution time: %.6f ms \n", ms);

    if (exit_code != 0 && exit_code != 2) {
        xeno_vm_print_error(vm);
    }

    xeno_vm_free(vm);
    free(vm);
    return exit_code;
}
