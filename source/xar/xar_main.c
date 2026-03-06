/*
 * xar_main.c — XenoScript Archive tool
 *
 * Usage:
 *   xar pack <directory> [-o output.xar] [-n name] [-v version]
 *   xar info <file.xar>
 *   xar list <file.xar>
 *   xar --help
 *
 * PACK:
 *   Compiles every .xeno file in <directory> to .xbc (in memory),
 *   then packages all .xbc blobs + a manifest into a single .xar file.
 *   The chunk name for each file is its path relative to <directory>
 *   with the .xeno extension stripped, e.g.:
 *     player.xeno        → "player"
 *     ai/pathfind.xeno   → "ai/pathfind"
 *
 *   The manifest name defaults to the directory basename.
 *   The exports list is populated from every top-level function and
 *   class name found in the compiled module.
 *
 * INFO:
 *   Print the manifest of a .xar file.
 *
 * LIST:
 *   List all chunk names inside a .xar file.
 */

#define _DEFAULT_SOURCE
#include "xar.h"
#include "xbc.h"
#include "lexer.h"
#include "parser.h"
#include "checker.h"
#include "compiler.h"
#include "../../source/stdlib/stdlib_declare.h"
#include "../../source/stdlib/stdlib_sources.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <dirent.h>
#include <sys/stat.h>

/* ── Utility ──────────────────────────────────────────────────────────── */

static void print_usage(void) {
    printf("Usage:\n");
    printf("  xar pack <dir> [-o out.xar] [-n name] [-v version] [-a author] [-d desc]\n");
    printf("  xar info <file.xar>\n");
    printf("  xar list <file.xar>\n");
    printf("  xar --help\n");
}

static const char *basename_of(const char *path) {
    const char *s = strrchr(path, '/');
    return s ? s + 1 : path;
}

/* Strip trailing slash from path (in place). */
static void strip_trailing_slash(char *path) {
    size_t len = strlen(path);
    while (len > 1 && (path[len-1] == '/' || path[len-1] == '\\'))
        path[--len] = '\0';
}

static char *read_file(const char *path) {
    FILE *f = fopen(path, "rb");
    if (!f) return NULL;
    fseek(f, 0, SEEK_END);
    long sz = ftell(f);
    fseek(f, 0, SEEK_SET);
    char *buf = malloc(sz + 1);
    if (!buf) { fclose(f); return NULL; }
    size_t nr = fread(buf, 1, sz, f);
    (void)nr;
    fclose(f);
    buf[sz] = '\0';
    return buf;
}

/* ── Import resolver (reused from xenoc_main logic) ───────────────────── */

#define MAX_IMPORTS 64
static char g_imported[MAX_IMPORTS][1024];
static int  g_import_count = 0;

static void imports_reset(void) { g_import_count = 0; }

static bool already_seen(const char *key) {
    for (int i = 0; i < g_import_count; i++)
        if (strcmp(g_imported[i], key) == 0) return true;
    return false;
}
static void mark_seen(const char *key) {
    if (g_import_count < MAX_IMPORTS)
        snprintf(g_imported[g_import_count++], 1024, "%s", key);
}

static void dir_of(const char *path, char *out, size_t sz) {
    if (sz == 0) return;

    // Copy safely
    size_t len = strlen(path);
    if (len >= sz) len = sz - 1;
    memcpy(out, path, len);
    out[len] = '\0';

    // Strip filename
    char *sep = strrchr(out, '/');
    if (!sep) sep = strrchr(out, '\\');
    if (sep) *(sep + 1) = '\0';
    else out[0] = '\0';
}

static char *buf_append(char *buf, size_t *len, size_t *cap,
                         const char *s, size_t slen) {
    while (*len + slen + 1 > *cap) {
        *cap *= 2;
        buf = realloc(buf, *cap);
        if (!buf) return NULL;
    }
    memcpy(buf + *len, s, slen);
    *len += slen;
    buf[*len] = '\0';
    return buf;
}

static char *resolve_imports(const char *source, const char *base_dir,
                              const char *label,
                              char *out, size_t *len, size_t *cap,
                              bool *err);

static char *resolve_imports(const char *source, const char *base_dir,
                              const char *label,
                              char *out, size_t *len, size_t *cap,
                              bool *err) {
    const char *p = source;
    while (*p) {
        while (*p==' '||*p=='\t'||*p=='\r'||*p=='\n') p++;
        if (!*p) break;
        if (p[0]=='/'&&p[1]=='/') { while(*p&&*p!='\n') p++; continue; }
        if (p[0]=='/'&&p[1]=='*') {
            p += 2;
            while (*p && !(p[0]=='*' && p[1]=='/')) p++;
            if (*p) { p += 2; }
            continue;
        }
        if (strncmp(p,"import",6)!=0 ||
            (p[6]!=' '&&p[6]!='\t'&&p[6]!='<'&&p[6]!='"')) break;
        p+=6; while(*p==' '||*p=='\t') p++;

        bool is_sys = false;
        char name[512]; int nlen = 0;
        if (*p=='<') {
            is_sys=true; p++;
            const char *s=p;
            while(*p&&*p!='>'&&*p!='\n') p++;
            nlen=(int)(p-s); if(nlen>511)nlen=511;
            memcpy(name,s,nlen); name[nlen]='\0';
            if(*p=='>') p++;
        } else if (*p=='"') {
            p++;
            const char *s=p;
            while(*p&&*p!='"'&&*p!='\n') p++;
            nlen=(int)(p-s); if(nlen>511)nlen=511;
            memcpy(name,s,nlen); name[nlen]='\0';
            if(*p=='"') p++;
        } else {
            while(*p&&*p!=';'&&*p!='\n') p++;
            if(*p==';') p++;
            continue;
        }
        while(*p==' '||*p=='\t') p++;
        if(*p==';') p++;

        char key[768];
        if (is_sys) snprintf(key,sizeof(key),"<sys:%s>",name);
        else        snprintf(key,sizeof(key),"%s%s",base_dir,name);

        if (already_seen(key)) continue;
        mark_seen(key);

        char *src = NULL;
        if (is_sys) {
            for (int i=0; i<XENOSCRIPT_STDLIB_COUNT; i++) {
                if (strcmp(XENOSCRIPT_STDLIB[i].name, name)==0) {
                    src = strdup(XENOSCRIPT_STDLIB[i].source); break;
                }
            }
            if (!src) {
                fprintf(stderr,"xar: unknown system module '<%s>' (in '%s')\n",
                        name, label);
                *err=true; return out;
            }
        } else {
            char fpath[1024];
            snprintf(fpath,sizeof(fpath),"%s%s",base_dir,name);
            src = read_file(fpath);
            if (!src) {
                fprintf(stderr,"xar: cannot open import '%s' (from '%s')\n",
                        fpath, label);
                *err=true; return out;
            }
        }

        char sub_dir[512]="";
        if (!is_sys) {
            char fpath[1024];
            snprintf(fpath,sizeof(fpath),"%s%s",base_dir,name);
            dir_of(fpath,sub_dir,sizeof(sub_dir));
        }
        out = resolve_imports(src, sub_dir, is_sys?name:key,
                               out, len, cap, err);
        free(src);
        if (*err||!out) return out;
    }
    out = buf_append(out, len, cap, source, strlen(source));
    out = buf_append(out, len, cap, "\n", 1);
    return out;
}

/* ── Compile one .xeno file to an in-memory .xbc blob ─────────────────── */

typedef struct {
    char    chunk_name[512]; /* relative name within archive */
    uint8_t *xbc_data;
    size_t   xbc_size;
    bool     ok;
} CompileResult;

static CompileResult compile_xeno(const char *file_path,
                                   const char *chunk_name) {
    CompileResult res = {0};
    snprintf(res.chunk_name, sizeof(res.chunk_name), "%s", chunk_name);

    char *main_source = read_file(file_path);
    if (!main_source) {
        fprintf(stderr, "xar: cannot read '%s'\n", file_path);
        return res;
    }

    char base_dir[512]="";
    dir_of(file_path, base_dir, sizeof(base_dir));

    imports_reset();
    size_t cap=65536, len=0;
    char *merged = malloc(cap);
    if (!merged) { free(main_source); return res; }
    merged[0]='\0';

    bool import_err = false;
    merged = resolve_imports(main_source, base_dir, file_path,
                              merged, &len, &cap, &import_err);
    free(main_source);
    if (import_err || !merged) { free(merged); return res; }

    Lexer    lexer;
    Parser   parser;
    Checker *checker = malloc(sizeof(Checker));
    Compiler compiler;
    Module   module;

    lexer_init(&lexer, merged);
    parser_init(&parser, &lexer);
    checker_init(checker, &parser.arena);
    module_init(&module);

    Type void_t=type_void(), any_t=type_any();
    checker_declare_host(checker,"print",void_t,&any_t,1);
    CompilerHostTable host_table;
    compiler_host_table_init(&host_table);
    compiler_host_table_add_any(&host_table,"print",0,1);
    stdlib_declare_host_fns(checker, &host_table);

    Program program = parser_parse(&parser);
    if (parser.had_error) {
        fprintf(stderr,"xar: parse errors in '%s':\n", file_path);
        parser_print_errors(&parser);
        goto cleanup;
    }

    if (!checker_check(checker, &program)) {
        fprintf(stderr,"xar: type errors in '%s':\n", file_path);
        checker_print_errors(checker);
        goto cleanup;
    }

    if (!compiler_compile(&compiler, &program, &module, &host_table)) {
        fprintf(stderr,"xar: compile errors in '%s':\n", file_path);
        compiler_print_errors(&compiler);
        goto cleanup;
    }

    {
        XbcResult xr = xbc_write_mem(&module, &res.xbc_data, &res.xbc_size);
        if (xr != XBC_OK) {
            fprintf(stderr,"xar: bytecode serialization failed: %s\n",
                    xbc_result_str(xr));
            goto cleanup;
        }
        res.ok = true;
    }

cleanup:
    module_free(&module);
    parser_free(&parser);
    free(checker);
    free(merged);
    return res;
}

/* ── Collect .xeno files recursively ─────────────────────────────────── */

#define MAX_FILES 512

typedef struct { char path[4096]; char rel[1024]; } FileEntry;
static FileEntry g_files[MAX_FILES];
static int       g_file_count = 0;

static void collect_xeno(const char *base, const char *rel) {
    char full[2048];
    if (rel[0])
        snprintf(full, sizeof(full), "%s/%s", base, rel);
    else
        snprintf(full, sizeof(full), "%s", base);

    DIR *d = opendir(full);
    if (!d) return;

    struct dirent *ent;
    while ((ent = readdir(d))) {
        if (ent->d_name[0] == '.') continue;

        char child_rel[1024], child_full[4096];
        if (rel[0])
            snprintf(child_rel,  sizeof(child_rel),  "%s/%s", rel, ent->d_name);
        else
            snprintf(child_rel, sizeof(child_rel), "%s", ent->d_name);
        snprintf(child_full, sizeof(child_full), "%s/%s", full, ent->d_name);

        struct stat st;
        if (stat(child_full, &st) != 0) continue;

        if (S_ISDIR(st.st_mode)) {
            collect_xeno(base, child_rel);
        } else if (S_ISREG(st.st_mode)) {
            size_t nl = strlen(ent->d_name);
            if (nl > 5 && strcmp(ent->d_name + nl - 5, ".xeno") == 0) {
                if (g_file_count < MAX_FILES) {
                    snprintf(g_files[g_file_count].path, 4096, "%s", child_full);
                    snprintf(g_files[g_file_count].rel,  1024, "%s", child_rel);
                    g_file_count++;
                }
            }
        }
    }
    closedir(d);
}

/* Strip .xeno extension from a relative path for use as chunk name. */
static void strip_xeno_ext(char *s) {
    size_t len = strlen(s);
    if (len > 5 && strcmp(s + len - 5, ".xeno") == 0)
        s[len - 5] = '\0';
}

/* ── Commands ─────────────────────────────────────────────────────────── */

static int cmd_pack(int argc, char **argv) {
    const char *dir      = NULL;
    const char *out_path = NULL;
    const char *name     = NULL;
    const char *version  = "1.0.0";
    const char *author   = "";
    const char *desc     = "";

    for (int i = 0; i < argc; i++) {
        if (strcmp(argv[i], "-o") == 0 && i+1 < argc) { out_path = argv[++i]; }
        else if (strcmp(argv[i], "-n") == 0 && i+1 < argc) { name = argv[++i]; }
        else if (strcmp(argv[i], "-v") == 0 && i+1 < argc) { version = argv[++i]; }
        else if (strcmp(argv[i], "-a") == 0 && i+1 < argc) { author = argv[++i]; }
        else if (strcmp(argv[i], "-d") == 0 && i+1 < argc) { desc = argv[++i]; }
        else if (!dir && argv[i][0] != '-') { dir = argv[i]; }
    }

    if (!dir) {
        fprintf(stderr, "xar pack: no directory specified\n");
        print_usage(); return 1;
    }

    char dir_buf[1024];
    strncpy(dir_buf, dir, sizeof(dir_buf)-1);
    strip_trailing_slash(dir_buf);

    /* Default name = directory basename */
    if (!name) name = basename_of(dir_buf);

    /* Default output = name + .xar */
    char out_buf[1024];
    if (!out_path) {
        snprintf(out_buf, sizeof(out_buf), "%s.xar", name);
        out_path = out_buf;
    }

    /* Collect .xeno files */
    g_file_count = 0;
    collect_xeno(dir_buf, "");
    if (g_file_count == 0) {
        fprintf(stderr, "xar pack: no .xeno files found in '%s'\n", dir_buf);
        return 1;
    }
    printf("xar: packing %d file(s) from '%s'\n", g_file_count, dir_buf);

    /* Compile each file */
    XarChunk    chunks[MAX_FILES];
    int         n_chunks = 0;
    XarManifest manifest; memset(&manifest, 0, sizeof(manifest));
    snprintf(manifest.name,        XAR_MAX_NAME, "%s", name);
    snprintf(manifest.version,     64,            "%s", version);
    snprintf(manifest.author,      128,           "%s", author);
    snprintf(manifest.description, 256,           "%s", desc);

    bool any_error = false;
    for (int i = 0; i < g_file_count; i++) {
        char chunk_name[512];
        snprintf(chunk_name, sizeof(chunk_name), "%s", g_files[i].rel);
        strip_xeno_ext(chunk_name);

        printf("  compiling '%s' -> chunk '%s'\n", g_files[i].rel, chunk_name);
        CompileResult r = compile_xeno(g_files[i].path, chunk_name);
        if (!r.ok) { any_error = true; continue; }

        chunks[n_chunks].data = r.xbc_data;
        chunks[n_chunks].size = r.xbc_size;
        snprintf(chunks[n_chunks].name, XAR_MAX_NAME, "%s", chunk_name);
        n_chunks++;

        /* Harvest exports from the compiled module (read back from xbc) */
        Module mod; module_init(&mod);
        if (xbc_read_mem(&mod, r.xbc_data, r.xbc_size) == XBC_OK) {
            for (int j = 0; j < mod.count && manifest.export_count < XAR_MAX_EXPORTS; j++) {
                snprintf(manifest.exports[manifest.export_count++],
                         XAR_MAX_NAME, "%s", mod.names[j]);
            }
            for (int j = 0; j < mod.class_count && manifest.export_count < XAR_MAX_EXPORTS; j++) {
                snprintf(manifest.exports[manifest.export_count++],
                         XAR_MAX_NAME, "%s", mod.classes[j].name);
            }
            module_free(&mod);
        }
    }

    if (any_error) {
        fprintf(stderr, "xar: errors during compilation, aborting\n");
        for (int i = 0; i < n_chunks; i++) free(chunks[i].data);
        return 1;
    }

    XarResult xr = xar_write(out_path, &manifest, chunks, n_chunks);
    for (int i = 0; i < n_chunks; i++) free(chunks[i].data);

    if (xr != XAR_OK) {
        fprintf(stderr, "xar: failed to write '%s': %s\n",
                out_path, xar_result_str(xr));
        return 1;
    }

    printf("xar: wrote '%s' (%d chunk(s), %d export(s))\n",
           out_path, n_chunks, manifest.export_count);
    return 0;
}

static int cmd_info(const char *path) {
    XarArchive ar;
    XarResult r = xar_read(&ar, path);
    if (r != XAR_OK) {
        fprintf(stderr, "xar info: cannot read '%s': %s\n", path, xar_result_str(r));
        return 1;
    }
    XarManifest *m = &ar.manifest;
    printf("Name:        %s\n", m->name);
    printf("Version:     %s\n", m->version);
    if (m->author[0])      printf("Author:      %s\n", m->author);
    if (m->description[0]) printf("Description: %s\n", m->description);
    printf("Chunks:      %d\n", ar.chunk_count);
    printf("Exports:     %d\n", m->export_count);
    if (m->dep_count) {
        printf("Depends on:");
        for (int i = 0; i < m->dep_count; i++) printf(" %s", m->dependencies[i]);
        printf("\n");
    }
    xar_archive_free(&ar);
    return 0;
}

static int cmd_list(const char *path) {
    XarArchive ar;
    XarResult r = xar_read(&ar, path);
    if (r != XAR_OK) {
        fprintf(stderr, "xar list: cannot read '%s': %s\n", path, xar_result_str(r));
        return 1;
    }
    printf("Archive: %s v%s\n", ar.manifest.name, ar.manifest.version);
    printf("Chunks (%d):\n", ar.chunk_count);
    for (int i = 0; i < ar.chunk_count; i++)
        printf("  [%d] %s (%zu bytes)\n", i, ar.chunks[i].name, ar.chunks[i].size);
    printf("Exports (%d):\n", ar.manifest.export_count);
    for (int i = 0; i < ar.manifest.export_count; i++)
        printf("  %s\n", ar.manifest.exports[i]);
    xar_archive_free(&ar);
    return 0;
}

/* ── Entry point ──────────────────────────────────────────────────────── */

int main(int argc, char **argv) {
    if (argc < 2 || strcmp(argv[1], "--help") == 0) {
        print_usage(); return 0;
    }
    if (strcmp(argv[1], "pack") == 0) return cmd_pack(argc-2, argv+2);
    if (strcmp(argv[1], "info") == 0) {
        if (argc < 3) { fprintf(stderr,"xar info: missing file\n"); return 1; }
        return cmd_info(argv[2]);
    }
    if (strcmp(argv[1], "list") == 0) {
        if (argc < 3) { fprintf(stderr,"xar list: missing file\n"); return 1; }
        return cmd_list(argv[2]);
    }
    fprintf(stderr,"xar: unknown command '%s'\n", argv[1]);
    print_usage(); return 1;
}
