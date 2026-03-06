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
#include "../../source/stdlib/stdlib_register.h"
#include "../../source/compiler/compile_pipeline.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <dirent.h>

/* ── Host functions ───────────────────────────────────────────────────── */

static XenoResult std_print(XenoVM *vm, int argc, Value *argv, Value *out) {
    (void)vm; (void)argc; (void)out;
    printf("%s\n", argv[0].s ? argv[0].s : "");
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
    Module *module = malloc(sizeof(Module));
    if (!module) return 1;

    XbcResult xr = xbc_read(module, path);
    if (xr != XBC_OK) {
        fprintf(stderr, "xenovm: failed to load '%s': %s\n",
                path, xbc_result_str(xr));
        free(module);
        return 2;
    }

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

    /* Load each declared dependency from deps/<name>.xar
     * (relative to the .xar's own directory) */
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

    /* Load dep archives into the VM pool */
    for (int i = 0; i < ar.manifest.dep_count; i++) {
        char dep_path[512];
        snprintf(dep_path, sizeof(dep_path), "%sdeps/%s.xar",
                 base_dir, ar.manifest.dependencies[i]);

        XarArchive dep;
        XarResult dr = xar_read(&dep, dep_path);
        if (dr != XAR_OK) {
            fprintf(stderr, "xenovm: missing dependency '%s' (looked in '%s')\n",
                    ar.manifest.dependencies[i], dep_path);
            xar_archive_free(&ar);
            return 2;
        }
        if (!xeno_vm_load_xar(vm, &dep)) {
            fprintf(stderr, "xenovm: failed to load dep '%s'\n",
                    ar.manifest.dependencies[i]);
            xar_archive_free(&dep);
            xar_archive_free(&ar);
            return 1;
        }
        xar_archive_free(&dep);
    }

    /* Load the main archive as a standalone runnable module */
    Module *module = malloc(sizeof(Module));
    if (!module) { xar_archive_free(&ar); return 1; }
    module_init(module);

    for (int i = 0; i < ar.chunk_count; i++) {
        Module cm; module_init(&cm);
        if (xbc_read_mem(&cm, ar.chunks[i].data, ar.chunks[i].size) == XBC_OK) {
            /* Propagate @Mod metadata from the first chunk that has it */
            if (!module->metadata.has_mod && cm.metadata.has_mod)
                module->metadata = cm.metadata;
            module_merge(module, &cm);
            module_free(&cm);
        }
    }

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
    Module staging; module_init(&staging);
    bool any_err = false;

    for (int i = 0; i < source_count && !any_err; i++) {
        char *src = pipeline_read_file(source_paths[i]);
        if (!src) { any_err = true; break; }

        bool imp_err = false;
        char *file_merged = pipeline_prepare_project(src, source_paths[i],
                                                     deps_dir, &staging, &imp_err);
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
    module_free(&staging);

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

    if (exit_code != 0 && exit_code != 2) {
        xeno_vm_print_error(vm);
    }

    xeno_vm_free(vm);
    free(vm);
    return exit_code;
}
