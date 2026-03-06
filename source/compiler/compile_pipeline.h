/*
 * compile_pipeline.h — shared import resolution and staging for xenoc and xenovm.
 *
 * Both the standalone compiler (xenoc) and the VM's source-run path (xenovm)
 * need to resolve `import <x>` and `import "file.xeno"` statements, load the
 * appropriate stdlib XAR chunks or local sources, and pre-declare all resulting
 * classes/functions to the type checker.
 *
 * This header is intended to be #included exactly once in each translation unit
 * that needs it (xenoc_main.c and vm.c), since it defines static functions.
 */

#ifndef XENOSCRIPT_COMPILE_PIPELINE_H
#define XENOSCRIPT_COMPILE_PIPELINE_H

#define _DEFAULT_SOURCE
#include "lexer.h"
#include "parser.h"
#include "checker.h"
#include "compiler.h"
#include "xbc.h"
#include "xar.h"
#include "stdlib_xar.h"
#include "../../source/stdlib/stdlib_declare.h"
#include "../../source/stdlib/stdlib_sources.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdbool.h>

/* ── Path helpers ─────────────────────────────────────────────────────── */

static void pipeline_dir_of(const char *path, char *out, size_t sz) {
    if (sz == 0) return;

    size_t len = strlen(path);
    if (len >= sz) len = sz - 1;
    memcpy(out, path, len);
    out[len] = '\0';

    char *sl = strrchr(out, '/');
#ifdef _WIN32
    char *bs = strrchr(out, '\\');
    if (bs && (!sl || bs > sl)) sl = bs;
#endif
    if (sl) sl[1] = '\0'; else out[0] = '\0';
}

static char *pipeline_read_file(const char *path) {
    FILE *f = fopen(path, "rb");
    if (!f) return NULL;
    fseek(f, 0, SEEK_END); long sz = ftell(f); rewind(f);
    char *buf = malloc((size_t)sz + 1);
    if (!buf) { fclose(f); return NULL; }
    fread(buf, 1, (size_t)sz, f); buf[sz] = '\0';
    fclose(f); return buf;
}

/* ── Dedup tracking ───────────────────────────────────────────────────── */

#define PIPELINE_MAX_SYS    32
#define PIPELINE_MAX_LOCAL  64

typedef struct {
    char sys_loaded[PIPELINE_MAX_SYS][64];
    int  sys_loaded_count;
    char local_imported[PIPELINE_MAX_LOCAL][1024];
    int  local_import_count;
} PipelineState;

static void pipeline_state_init(PipelineState *s) {
    s->sys_loaded_count   = 0;
    s->local_import_count = 0;
}

static bool pipeline_sys_loaded(PipelineState *s, const char *name) {
    for (int i = 0; i < s->sys_loaded_count; i++)
        if (strcmp(s->sys_loaded[i], name) == 0) return true;
    return false;
}

static bool pipeline_local_seen(PipelineState *s, const char *key) {
    for (int i = 0; i < s->local_import_count; i++)
        if (strcmp(s->local_imported[i], key) == 0) return true;
    return false;
}

static void pipeline_local_mark(PipelineState *s, const char *key) {
    if (s->local_import_count < PIPELINE_MAX_LOCAL)
        snprintf(s->local_imported[s->local_import_count++], 1024, "%s", key);
}

/* ── Buffer helpers ───────────────────────────────────────────────────── */

static char *pipeline_buf_append(char *buf, size_t *len, size_t *cap,
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

/* ── System module loader ─────────────────────────────────────────────── */

static bool pipeline_load_sys_module(PipelineState *s, const char *name,
                                      Module *staging) {
    if (pipeline_sys_loaded(s, name)) return true;
    for (int i = 0; i < STDLIB_XAR_TOTAL_COUNT; i++) {
        if (strcmp(STDLIB_XAR_TABLE[i].name, name) != 0) continue;
        size_t sz = (size_t)(STDLIB_XAR_TABLE[i].end - STDLIB_XAR_TABLE[i].start);
        XarArchive ar;
        if (xar_read_mem(&ar, STDLIB_XAR_TABLE[i].start, sz) != XAR_OK) {
            fprintf(stderr, "xenoscript: failed to read embedded stdlib '%s'\n", name);
            return false;
        }
        for (int j = 0; j < ar.chunk_count; j++) {
            Module cm; module_init(&cm);
            if (xbc_read_mem(&cm, ar.chunks[j].data, ar.chunks[j].size) == XBC_OK) {
                module_merge(staging, &cm); module_free(&cm);
            }
        }
        xar_archive_free(&ar);
        if (s->sys_loaded_count < PIPELINE_MAX_SYS)
            strncpy(s->sys_loaded[s->sys_loaded_count++], name, 63);
        return true;
    }
    fprintf(stderr, "xenoscript: unknown system module '<%s>'\n", name);
    return false;
}

/* ── Import resolver (recursive) ─────────────────────────────────────── */

static char *pipeline_resolve_imports(PipelineState *s,
                                       const char *source,
                                       const char *base_dir,
                                       const char *label,
                                       char *out, size_t *len, size_t *cap,
                                       Module *staging, bool *err);

static char *pipeline_resolve_imports(PipelineState *s,
                                       const char *source,
                                       const char *base_dir,
                                       const char *label,
                                       char *out, size_t *len, size_t *cap,
                                       Module *staging, bool *err) {
    const char *p = source;
    while (*p) {
        while (*p == ' ' || *p == '\t' || *p == '\r' || *p == '\n') p++;
        if (!*p) break;
        if (p[0]=='/'&&p[1]=='/') { while (*p && *p!='\n') p++; continue; }
        if (p[0]=='/'&&p[1]=='*') {
            p += 2;
            while (*p && !(p[0]=='*' && p[1]=='/')) p++;
            if (*p) p += 2;
            continue;
        }
        if (strncmp(p, "import", 6) != 0 ||
            (p[6]!=' ' && p[6]!='\t' && p[6]!='<' && p[6]!='"')) break;
        p += 6;
        while (*p == ' ' || *p == '\t') p++;

        bool is_sys = false;
        char name[512]; int nlen = 0;
        if (*p == '<') {
            is_sys = true; p++;
            const char *s2 = p;
            while (*p && *p != '>' && *p != '\n') p++;
            nlen = (int)(p - s2); if (nlen > 511) nlen = 511;
            memcpy(name, s2, nlen); name[nlen] = '\0';
            if (*p == '>') p++;
        } else if (*p == '"') {
            p++;
            const char *s2 = p;
            while (*p && *p != '"' && *p != '\n') p++;
            nlen = (int)(p - s2); if (nlen > 511) nlen = 511;
            memcpy(name, s2, nlen); name[nlen] = '\0';
            if (*p == '"') p++;
        } else {
            while (*p && *p != ';' && *p != '\n') p++;
            if (*p == ';') p++;
            continue;
        }
        while (*p == ' ' || *p == '\t') p++;
        if (*p == ';') p++;

        if (is_sys) {
            /* Source-merge if stdlib source is available (needed for generic classes) */
            bool source_merged = false;
            for (int si = 0; si < XENOSCRIPT_STDLIB_COUNT; si++) {
                if (strcmp(XENOSCRIPT_STDLIB[si].name, name) != 0) continue;
                char skey[768];
                snprintf(skey, sizeof(skey), "<src:%s>", name);
                if (!pipeline_local_seen(s, skey)) {
                    pipeline_local_mark(s, skey);
                    out = pipeline_resolve_imports(s, XENOSCRIPT_STDLIB[si].source,
                                                   "", name, out, len, cap, staging, err);
                    if (*err || !out) return out;
                }
                source_merged = true;
                break;
            }
            if (!source_merged) {
                if (!pipeline_load_sys_module(s, name, staging)) {
                    *err = true; return out;
                }
            }
        } else {
            char key[768];
            snprintf(key, sizeof(key), "%s%s", base_dir, name);
            if (!pipeline_local_seen(s, key)) {
                pipeline_local_mark(s, key);
                char fpath[1024];
                snprintf(fpath, sizeof(fpath), "%s%s", base_dir, name);
                char *src = pipeline_read_file(fpath);
                if (!src) {
                    fprintf(stderr, "xenoscript: cannot open '%s' (from '%s')\n",
                            fpath, label);
                    *err = true; return out;
                }
                char sub[512] = "";
                pipeline_dir_of(fpath, sub, sizeof(sub));
                out = pipeline_resolve_imports(s, src, sub, fpath,
                                               out, len, cap, staging, err);
                free(src);
                if (*err || !out) return out;
            }
        }
    }
    out = pipeline_buf_append(out, len, cap, p, strlen(p));
    out = pipeline_buf_append(out, len, cap, "\n", 1);
    return out;
}

/* ── Declare staging to checker ──────────────────────────────────────── */

static Type pipeline_kind_to_type(int kind) {
    switch (kind) {
        case TYPE_INT:    return type_int();
        case TYPE_FLOAT:  return type_float();
        case TYPE_BOOL:   return type_bool();
        case TYPE_STRING: return type_string();
        case TYPE_VOID:   return type_void();
        default:          return type_any();
    }
}

static void pipeline_declare_staging(Checker *checker, const Module *staging) {
    for (int i = 0; i < staging->count; i++) {
        const char *nm = staging->names[i];
        if (strcmp(nm, "__sinit__") == 0) continue;
        if (strchr(nm, '.') != NULL) continue;
        Chunk *ch = &staging->chunks[i];
        Type ret = pipeline_kind_to_type(ch->return_type_kind);
        int pc = ch->param_count < 16 ? ch->param_count : 16;
        Type params[16];
        for (int j = 0; j < pc; j++)
            params[j] = pipeline_kind_to_type(ch->param_type_kinds[j]);
        checker_declare_host(checker, nm, ret, pc > 0 ? params : NULL, pc);
    }
    for (int i = 0; i < staging->class_count; i++)
        checker_declare_class_from_def(checker, &staging->classes[i]);
}

/* ── Top-level entry point ────────────────────────────────────────────── */

/*
 * pipeline_prepare — resolve all imports from `source` (at `source_path`),
 * populate `staging` with stdlib modules, and return the fully-merged source
 * string (heap-allocated, caller must free). Returns NULL on error.
 */
static char *pipeline_prepare(const char *source, const char *source_path,
                               Module *staging, bool *err) {
    PipelineState ps;
    pipeline_state_init(&ps);

    /* Always load core */
    pipeline_load_sys_module(&ps, "core", staging);

    char base_dir[512] = "";
    if (source_path && *source_path)
        pipeline_dir_of(source_path, base_dir, sizeof(base_dir));

    size_t len = 0, cap = 65536;
    char *merged = malloc(cap);
    if (!merged) { *err = true; return NULL; }
    merged[0] = '\0';

    *err = false;
    merged = pipeline_resolve_imports(&ps, source, base_dir,
                                       source_path ? source_path : "<source>",
                                       merged, &len, &cap, staging, err);
    if (*err || !merged) { free(merged); return NULL; }
    return merged;
}

#endif /* XENOSCRIPT_COMPILE_PIPELINE_H */
