/*
 * xenoc_main.c — XenoScript standalone compiler
 *
 * Modes:
 *   xenoc <file.xeno> [-o out.xbc] [--dump]
 *       Single-file mode: compile one .xeno to .xbc (unchanged).
 *
 *   xenoc build [project-dir] [-o out.xar]
 *       Project mode: reads xeno.project, recursively compiles all
 *       .xeno files under src/, loads deps from deps\*.xar, validates
 *       @Mod id matches xeno.project id, writes a single .xar output.
 *
 * Project directory layout expected by `xenoc build`:
 *   <project>/
 *     xeno.project       -- TOML metadata (id, version, author, deps)
 *     src/               -- .xeno source files (recursive)
 *     deps/              -- dependency .xar files
 */
#define _DEFAULT_SOURCE
#include "compile_pipeline.h"
#include "xar.h"
#include "toml.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <dirent.h>
#include <sys/stat.h>
#ifdef _WIN32
#  include <direct.h>
#  define access _access
#else
#  include <unistd.h>
#endif

static void print_usage(void) {
    printf("Usage:\n");
    printf("  xenoc <source.xeno> [-o output.xbc] [--dump]\n");
    printf("  xenoc build [project-dir] [-o output.xar]\n");
    printf("  xenoc --help\n\n");
    printf("System modules:\n");
    for (int i = 0; i < STDLIB_XAR_TOTAL_COUNT; i++)
        printf("  <%s>\n", STDLIB_XAR_TABLE[i].name);
}

static void make_xbc_path(const char *in, char *out, size_t sz) {
    strncpy(out, in, sz-1); out[sz-1] = '\0';
    char *dot = strrchr(out, '.');
    if (dot) strncpy(dot, ".xbc", sz-(dot-out)-1);
    else      strncat(out, ".xbc", sz-strlen(out)-1);
}

static bool has_ext(const char *path, const char *ext) {
    const char *dot = strrchr(path, '.');
    return dot && strcmp(dot, ext) == 0;
}

static bool path_is_dir(const char *path) {
    struct stat st;
    return stat(path, &st) == 0 && S_ISDIR(st.st_mode);
}

/* ── Single-file compile (existing behaviour, extracted to function) ─── */

static int compile_single(const char *input_path,
                           const char *output_path,
                           bool dump_only) {
    char *main_source = pipeline_read_file(input_path);
    if (!main_source) {
        fprintf(stderr, "xenoc: cannot open '%s'\n", input_path);
        return 2;
    }

    Module staging; module_init(&staging);
    bool import_err = false;
    char *merged = pipeline_prepare(main_source, input_path, &staging, &import_err);
    free(main_source);
    if (import_err || !merged) { free(merged); module_free(&staging); return 1; }

    Lexer lexer; Parser parser; Checker *checker = malloc(sizeof(Checker));
    Compiler compiler; Module module;
    lexer_init(&lexer, merged); parser_init(&parser, &lexer);
    checker_init(checker, &parser.arena); module_init(&module);

    int exit_code = 0;

    Type void_t = type_void(), any_t = type_any();
    checker_declare_host(checker, "print", void_t, &any_t, 1);
    CompilerHostTable host_table;
    compiler_host_table_init(&host_table);
    compiler_host_table_add_any(&host_table, "print", 0, 1);
    stdlib_declare_host_fns(checker, &host_table);
    pipeline_declare_staging(checker, &staging);
    module_merge(&module, &staging);

    Program program = parser_parse(&parser);
    if (parser.had_error) {
        fprintf(stderr, "xenoc: parse errors in '%s':\n", input_path);
        parser_print_errors(&parser); exit_code = 1; goto cleanup_single;
    }

    {
        bool ok = checker_check(checker, &program);
        if (checker->error_count > 0) {
            if (!ok) fprintf(stderr, "xenoc: type errors in '%s':\n", input_path);
            checker_print_errors(checker);
        }
        if (!ok) { exit_code = 1; goto cleanup_single; }
    }

    if (!compiler_compile(&compiler, &program, &module, &host_table)) {
        fprintf(stderr, "xenoc: compile errors in '%s':\n", input_path);
        compiler_print_errors(&compiler); exit_code = 1; goto cleanup_single;
    }

    if (dump_only) {
        module_disassemble(&module);
    } else {
        char default_out[512];
        if (!output_path) {
            make_xbc_path(input_path, default_out, sizeof(default_out));
            output_path = default_out;
        }
        XbcResult r = xbc_write(&module, output_path);
        if (r != XBC_OK) {
            fprintf(stderr, "xenoc: failed to write '%s': %s\n",
                    output_path, xbc_result_str(r));
            exit_code = 2; goto cleanup_single;
        }
        printf("xenoc: compiled '%s' -> '%s'\n", input_path, output_path);
    }

cleanup_single:
    module_free(&module); module_free(&staging);
    parser_free(&parser); free(checker); free(merged);
    return exit_code;
}

/* ── Project build helpers ────────────────────────────────────────────── */

#define MAX_PROJECT_SOURCES 512

static int collect_sources(const char *dir,
                            char *paths[], int *count, int cap) {
    DIR *d = opendir(dir);
    if (!d) {
        fprintf(stderr, "xenoc: cannot open directory '%s'\n", dir);
        return 1;
    }
    struct dirent *ent;
    while ((ent = readdir(d)) != NULL) {
        if (ent->d_name[0] == '.') continue;

        char full[1024];
        snprintf(full, sizeof(full), "%s/%s", dir, ent->d_name);

        if (path_is_dir(full)) {
            if (collect_sources(full, paths, count, cap) != 0) {
                closedir(d); return 1;
            }
        } else if (has_ext(ent->d_name, ".xeno")) {
            if (*count >= cap) {
                fprintf(stderr, "xenoc: too many source files (max %d)\n", cap);
                closedir(d); return 1;
            }
            paths[(*count)++] = strdup(full);
        }
    }
    closedir(d);
    return 0;
}

static bool load_dep_into_staging(const char *xar_path, Module *staging) {
    XarArchive ar;
    XarResult xr = xar_read(&ar, xar_path);
    if (xr != XAR_OK) {
        fprintf(stderr, "xenoc: failed to load dep '%s': %s\n",
                xar_path, xar_result_str(xr));
        return false;
    }
    for (int i = 0; i < ar.chunk_count; i++) {
        Module cm; module_init(&cm);
        if (xbc_read_mem(&cm, ar.chunks[i].data, ar.chunks[i].size) == XBC_OK) {
            module_merge(staging, &cm);
            module_free(&cm);
        }
    }
    xar_archive_free(&ar);
    return true;
}

/* Lightweight scan of merged source for @Mod("id") or @Mod(name="id").
 * Returns false if not found. The checker does the authoritative validation. */
static bool find_mod_id(const char *src, char *out, size_t cap) {
    const char *p = src;
    while ((p = strstr(p, "@Mod(")) != NULL) {
        p += 5;
        while (*p == ' ' || *p == '\t') p++;
        /* Named: name="id" */
        if (strncmp(p, "name", 4) == 0) {
            p += 4;
            while (*p == ' ' || *p == '\t') p++;
            if (*p != '=') { p++; continue; }
            p++;
            while (*p == ' ' || *p == '\t') p++;
        }
        if (*p == '"') {
            p++;
            size_t i = 0;
            while (*p && *p != '"' && *p != '\n' && i < cap - 1)
                out[i++] = *p++;
            out[i] = '\0';
            return i > 0;
        }
        p++;
    }
    return false;
}

/* ── Project build ────────────────────────────────────────────────────── */

static int build_project(const char *project_dir, const char *output_path) {
    /* 1. Read and parse xeno.project */
    char proj_path[512];
    snprintf(proj_path, sizeof(proj_path), "%s/xeno.project", project_dir);

    char *proj_toml = pipeline_read_file(proj_path);
    if (!proj_toml) {
        fprintf(stderr, "xenoc: cannot open '%s'\n", proj_path);
        return 2;
    }

    XarManifest manifest;
    char toml_err[256] = {0};
    if (!xar_manifest_from_toml(&manifest, proj_toml, toml_err, sizeof(toml_err))) {
        fprintf(stderr, "xenoc: %s\n", toml_err);
        free(proj_toml);
        return 1;
    }
    free(proj_toml);

    printf("xenoc: building mod '%s' v%s\n", manifest.name, manifest.version);

    /* 2. Collect source files from src/ */
    char src_dir[512];
    snprintf(src_dir, sizeof(src_dir), "%s/src", project_dir);
    if (!path_is_dir(src_dir)) {
        fprintf(stderr, "xenoc: '%s' has no src/ directory\n", project_dir);
        return 1;
    }

    char  *source_paths[MAX_PROJECT_SOURCES];
    int    source_count = 0;
    if (collect_sources(src_dir, source_paths, &source_count,
                        MAX_PROJECT_SOURCES) != 0)
        return 1;

    if (source_count == 0) {
        fprintf(stderr, "xenoc: no .xeno files found in '%s'\n", src_dir);
        return 1;
    }
    printf("xenoc: found %d source file(s)\n", source_count);

    /* 3. Load deps from deps/<id>.xar into staging */
    Module staging;
    module_init(&staging);
    char deps_dir[512];
    snprintf(deps_dir, sizeof(deps_dir), "%s/deps", project_dir);

    for (int i = 0; i < manifest.dep_count; i++) {
        char dep_path[1024];
        snprintf(dep_path, sizeof(dep_path), "%s/%s.xar",
                 deps_dir, manifest.dependencies[i]);
        printf("xenoc: loading dep '%s'\n", manifest.dependencies[i]);
        if (!load_dep_into_staging(dep_path, &staging)) {
            for (int j = 0; j < source_count; j++) free(source_paths[j]);
            module_free(&staging);
            return 1;
        }
    }

    /* 4. Merge all source files through the pipeline into one string */
    size_t merged_cap = 131072;
    char  *merged_all = malloc(merged_cap);
    size_t merged_len = 0;
    if (!merged_all) { module_free(&staging); return 1; }
    merged_all[0] = '\0';

    for (int i = 0; i < source_count; i++) {
        char *src = pipeline_read_file(source_paths[i]);
        if (!src) {
            fprintf(stderr, "xenoc: cannot open '%s'\n", source_paths[i]);
            free(merged_all);
            for (int j = 0; j < source_count; j++) free(source_paths[j]);
            module_free(&staging);
            return 2;
        }
        printf("xenoc:   + %s\n", source_paths[i]);

        bool imp_err = false;
        char *file_merged = strdup(src);
        // char *file_merged = pipeline_prepare_project(
        //     src, 
        //     source_paths[i],
        //     deps_dir,
        //     &staging,
        //     &imp_err);
        free(src);
        if (imp_err || !file_merged) {
            free(file_merged); free(merged_all);
            for (int j = 0; j < source_count; j++) free(source_paths[j]);
            module_free(&staging);
            return 1;
        }

        size_t flen = strlen(file_merged);
        while (merged_len + flen + 2 > merged_cap) {
            merged_cap *= 2;
            char *tmp = realloc(merged_all, merged_cap);
            if (!tmp) {
                free(file_merged); free(merged_all);
                for (int j = 0; j < source_count; j++) free(source_paths[j]);
                module_free(&staging);
                return 1;
            }
            merged_all = tmp;
        }
        memcpy(merged_all + merged_len, file_merged, flen);
        merged_len += flen;
        merged_all[merged_len++] = '\n';
        merged_all[merged_len]   = '\0';
        free(file_merged);
    }
    for (int i = 0; i < source_count; i++) free(source_paths[i]);

    /* 5. Validate @Mod id matches xeno.project id */
    char found_id[XAR_MAX_NAME] = {0};
    if (find_mod_id(merged_all, found_id, sizeof(found_id))) {
        if (strcmp(found_id, manifest.name) != 0) {
            fprintf(stderr,
                    "xenoc: @Mod id '%s' does not match xeno.project id '%s'\n",
                    found_id, manifest.name);
            free(merged_all); module_free(&staging);
            return 1;
        }
        printf("xenoc: @Mod id '%s' verified\n", found_id);
    } else {
        fprintf(stderr, "xenoc: warning: no @Mod annotation found in project\n");
    }

    /* 6. Compile */
    Lexer    lexer;
    Parser   parser;
    Checker *checker = malloc(sizeof(Checker));
    Compiler compiler;
    Module   module;

    lexer_init(&lexer, merged_all);
    parser_init(&parser, &lexer);
    checker_init(checker, &parser.arena);
    module_init(&module);

    int exit_code = 0;

    Type void_t = type_void(), any_t = type_any();
    checker_declare_host(checker, "print", void_t, &any_t, 1);
    CompilerHostTable host_table;
    compiler_host_table_init(&host_table);
    compiler_host_table_add_any(&host_table, "print", 0, 1);
    stdlib_declare_host_fns(checker, &host_table);
    pipeline_declare_staging(checker, &staging);
    // module_merge(&module, &staging);

    Program program = parser_parse(&parser);
    if (parser.had_error) {
        fprintf(stderr, "xenoc: parse errors:\n");
        parser_print_errors(&parser); exit_code = 1; goto cleanup_build;
    }

    {
        bool ok = checker_check(checker, &program);
        if (checker->error_count > 0) {
            if (!ok) fprintf(stderr, "xenoc: type errors:\n");
            checker_print_errors(checker);
        }
        if (!ok) { exit_code = 1; goto cleanup_build; }
    }

    if (!compiler_compile(&compiler, &program, &module, &host_table)) {
        fprintf(stderr, "xenoc: compile errors:\n");
        compiler_print_errors(&compiler); exit_code = 1; goto cleanup_build;
    }

    /* 7. Serialise module to .xbc bytes and wrap in .xar */
    {
        uint8_t *xbc_data = NULL;
        size_t   xbc_size = 0;
        XbcResult xr = xbc_write_mem(&module, &xbc_data, &xbc_size);
        if (xr != XBC_OK) {
            fprintf(stderr, "xenoc: serialise failed: %s\n", xbc_result_str(xr));
            exit_code = 2; goto cleanup_build;
        }

        /* Export = the actual @Mod entry class name from the compiled module */
        const char *entry_class = module.metadata.entry_class[0]
                                  ? module.metadata.entry_class
                                  : manifest.name;
        snprintf(manifest.exports[0], XAR_MAX_NAME, "%s", entry_class);
        manifest.export_count = 1;

        /* Default output: dist/<id>.xar */
        char default_out[512];
        if (!output_path) {
            char dist_dir[512];
            snprintf(dist_dir, sizeof(dist_dir), "%s/dist", project_dir);
#ifdef _WIN32
            _mkdir(dist_dir);
#else
            mkdir(dist_dir, 0755);
#endif
            snprintf(default_out, sizeof(default_out),
                     "%s/dist/%s.xar", project_dir, manifest.name);
            output_path = default_out;
        }

        XarChunk chunk;
        snprintf(chunk.name, sizeof(chunk.name), "%s", manifest.name);
        chunk.data = xbc_data;
        chunk.size = xbc_size;

        XarResult wr = xar_write(output_path, &manifest, &chunk, 1);
        free(xbc_data);

        if (wr != XAR_OK) {
            fprintf(stderr, "xenoc: failed to write '%s': %s\n",
                    output_path, xar_result_str(wr));
            exit_code = 2; goto cleanup_build;
        }
        printf("xenoc: built '%s' -> '%s'\n", manifest.name, output_path);
    }

cleanup_build:
    module_free(&module); module_free(&staging);
    parser_free(&parser); free(checker); free(merged_all);
    return exit_code;
}

/* ── Main ─────────────────────────────────────────────────────────────── */

int main(int argc, char **argv) {
    if (argc < 2) { print_usage(); return 3; }
    if (strcmp(argv[1], "--help") == 0) { print_usage(); return 0; }

    /* xenoc build [dir] [-o out.xar] */
    if (strcmp(argv[1], "build") == 0) {
        const char *project_dir = ".";
        const char *output_path = NULL;
        for (int i = 2; i < argc; i++) {
            if (strcmp(argv[i], "-o") == 0) {
                if (++i >= argc) {
                    fprintf(stderr, "xenoc: -o requires a path\n"); return 3;
                }
                output_path = argv[i];
            } else if (argv[i][0] != '-') {
                project_dir = argv[i];
            } else {
                fprintf(stderr, "xenoc: unknown option '%s'\n", argv[i]);
                return 3;
            }
        }
        return build_project(project_dir, output_path);
    }

    /* xenoc <file.xeno> [-o out.xbc] [--dump] */
    const char *input_path  = NULL;
    const char *output_path = NULL;
    bool        dump_only   = false;

    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "-o") == 0) {
            if (++i >= argc) {
                fprintf(stderr, "xenoc: -o requires a path\n"); return 3;
            }
            output_path = argv[i];
        } else if (strcmp(argv[i], "--dump") == 0) {
            dump_only = true;
        } else if (argv[i][0] == '-') {
            fprintf(stderr, "xenoc: unknown option '%s'\n", argv[i]); return 3;
        } else {
            if (input_path) {
                fprintf(stderr, "xenoc: multiple inputs not supported\n");
                return 3;
            }
            input_path = argv[i];
        }
    }

    if (!input_path) {
        fprintf(stderr, "xenoc: no input file\n"); print_usage(); return 3;
    }
    return compile_single(input_path, output_path, dump_only);
}
