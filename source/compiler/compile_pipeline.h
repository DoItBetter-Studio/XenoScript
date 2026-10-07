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

#ifndef _POSIX_C_SOURCE
#define _POSIX_C_SOURCE 200809L
#endif
#include "lexer.h"
#include "parser.h"
#include "checker.h"
#include "compiler.h"
#include "xbc.h"
#include "xar.h"
#include "stdlib_xar.h"
#include "toml.h"
#include "../../source/stdlib/stdlib_declare.h"
#include "../../source/stdlib/stdlib_sources.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdbool.h>

/* ── Path helpers ─────────────────────────────────────────────────────── */

static void pipeline_dir_of(const char *path, char *out, size_t sz) {
    snprintf(out, sz, "%s", path);
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
    size_t nr = fread(buf, 1, (size_t)sz, f);
    buf[nr] = '\0';  /* use actual bytes read */
    fclose(f); return buf;
}

typedef struct {
    int major, minor, patch;
    bool valid;
} SemVer;

/* Parse "1.2.3" or "v1.2.3" → SemVer. Returns valid=false on failure. */
static SemVer semver_parse(const char *s)
{
    SemVer v = {0, 0, 0, false};
    if (!s || !*s) return v;

    /* skip leading v/V */
    if (s[0] == 'v' || s[0] == 'V') s++;

    int n = sscanf(s, "%d.%d.%d", &v.major, &v.minor, &v.patch);
    if (n < 1) return v;          /* need at least major */
    if (n == 1) { v.minor = 0; v.patch = 0; }
    if (n == 2) { v.patch = 0; }
    v.valid = true;
    return v;
}

/* -1 if a < b, 0 if equal, 1 if a > b */
static int semver_cmp(SemVer a, SemVer b)
{
    if (a.major != b.major) return a.major < b.major ? -1 : 1;
    if (a.minor != b.minor) return a.minor < b.minor ? -1 : 1;
    if (a.patch != b.patch) return a.patch < b.patch ? -1 : 1;
    return 0;
}

/*
 * Does `actual` satisfy `constraint`?
 *
 * Supported constraints:
 *   "1.2.3"   exact
 *   "^1.2.3"  >=1.2.3  &&  <2.0.0
 *   "~1.2.3"  >=1.2.3  &&  <1.3.0
 *   ">=1.2.3" minimum
 *   "*" or "" any
 */
static bool semver_satisfies(const char *constraint, const char *actual_str)
{
    if (!constraint || !*constraint || strcmp(constraint, "*") == 0)
        return true;

    SemVer actual = semver_parse(actual_str);
    if (!actual.valid) return false;

    const char *c = constraint;
    char op = 0;   /* '^', '~', or 0 for exact; 'g' for >= */

    if (c[0] == '^' || c[0] == '~') {
        op = c[0];
        c++;
    } else if (c[0] == '>' && c[1] == '=') {
        op = 'g';
        c += 2;
    }

    SemVer need = semver_parse(c);
    if (!need.valid) return false;

    int cmp = semver_cmp(actual, need);

    switch (op) {
    case 0:   /* exact */
        return cmp == 0;
    case 'g': /* >= */
        return cmp >= 0;
    case '^': /* compatible: >= need && < (need.major+1).0.0 */
        if (cmp < 0) return false;
        return actual.major == need.major;
    case '~': /* approx: >= need && < need.major.(need.minor+1).0 */
        if (cmp < 0) return false;
        return actual.major == need.major && actual.minor == need.minor;
    default:
        return false;
    }
}

/* ── Dedup tracking ───────────────────────────────────────────────────── */

#define PIPELINE_MAX_SYS    32
#define PIPELINE_MAX_LOCAL  64
#define PIPELINE_MAX_DEPS	32

typedef struct {
	char name[64];
	char version[32];		/* required version from xeno.project; "" = any */
} PipelineDep;

typedef struct {
    char sys_loaded[PIPELINE_MAX_SYS][64];
    int  sys_loaded_count;
    char local_imported[PIPELINE_MAX_LOCAL][1024];
    int  local_import_count;
    char deps_dir[512];   /* project deps/ directory, or "" if none */

	/* Declared project dependencies (from xeno.project [dependencies]) */
    PipelineDep deps[PIPELINE_MAX_DEPS];
    int         dep_count;
} PipelineState;

static void pipeline_state_init(PipelineState *s) {
    s->sys_loaded_count   = 0;
    s->local_import_count = 0;
    s->deps_dir[0]        = '\0';
	s->dep_count		  = 0;
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

/* ── Last-error reporting ─────────────────────────────────────────────── */
/* Import failures are recorded here (first failure wins) as well as echoed to
 * stderr, so front-ends such as the LSP can show a precise diagnostic. Clear
 * with pipeline_clear_error() before a run. */
#include <stdarg.h>
static char pipeline_last_error[320];
static char pipeline_last_error_import[512];  /* name of the import that failed */

static void pipeline_clear_error(void) {
    pipeline_last_error[0] = '\0';
    pipeline_last_error_import[0] = '\0';
}

static void pipeline_set_error(const char *import_name, const char *fmt, ...) {
    char tmp[320];
    va_list ap; va_start(ap, fmt);
    vsnprintf(tmp, sizeof(tmp), fmt, ap);
    va_end(ap);
    fprintf(stderr, "xenoscript: %s\n", tmp);
    if (pipeline_last_error[0]) return;               /* keep the first failure */
    snprintf(pipeline_last_error, sizeof(pipeline_last_error), "%s", tmp);
    snprintf(pipeline_last_error_import, sizeof(pipeline_last_error_import), "%s",
             import_name ? import_name : "");
}

/* ── System module loader ─────────────────────────────────────────────── */

/* Fill ps->deps_dir and ps->deps[] from xeno.project.
 * Returns true if the project file was found and parsed. */
static inline bool pipeline_load_project(PipelineState *ps, const char *project_root)
{
    char path[1100];
    snprintf(path, sizeof(path), "%s/xeno.project", project_root);
    char *text = pipeline_read_file(path);
    if (!text) return false;

    TomlDoc doc;
    if (!toml_parse(&doc, text)) {
        free(text);
        return false;
    }
    free(text);

    /* deps_dir */
    const char *configured = toml_get(&doc, "project", "deps_dir");
    if (configured && *configured) {
        int n = snprintf(ps->deps_dir, sizeof ps->deps_dir, "%s/%s", project_root, configured);
        if (n < 0 || (size_t)n >= sizeof ps->deps_dir) { toml_free(&doc); return false; }
    } else {
        static const char *candidates[] = { "deps", "dependencies", "libraries", "xars", NULL };
        ps->deps_dir[0] = '\0';
        for (int i = 0; candidates[i]; i++) {
            int n = snprintf(ps->deps_dir, sizeof ps->deps_dir, "%s/%s", project_root, candidates[i]);
            if (n < 0 || (size_t)n >= sizeof ps->deps_dir) { toml_free(&doc); return false; }
            break; /* for now just take the first candidate; refine with stat() later */
        }
        if (!ps->deps_dir[0])
            snprintf(ps->deps_dir, sizeof(ps->deps_dir), "%s/deps", project_root);
    }

    /* [dependencies] → ps->deps[] */
    ps->dep_count = 0;
    for (int i = 0; i < doc.count && ps->dep_count < PIPELINE_MAX_DEPS; i++) {
        if (strcmp(doc.entries[i].section, "dependencies") != 0) continue;
        PipelineDep *d = &ps->deps[ps->dep_count++];
        snprintf(d->name,    sizeof(d->name),    "%s", doc.entries[i].key);
        snprintf(d->version, sizeof(d->version), "%s", doc.entries[i].value);
    }

    toml_free(&doc);
    return true;
}

static const PipelineDep *pipeline_find_dep(const PipelineState *ps, const char *name)
{
    for (int i = 0; i < ps->dep_count; i++)
        if (strcmp(ps->deps[i].name, name) == 0)
            return &ps->deps[i];
    return NULL;
}

static bool pipeline_load_sys_module(PipelineState *s, const char *name,
                                      Module *staging) {
    if (pipeline_sys_loaded(s, name)) return true;

    /* ── 1. Project dependency (.xar in deps_dir) ─────────────────────── */
    if (s->deps_dir[0]) {
        char dep_path[1024];
        snprintf(dep_path, sizeof(dep_path), "%s/%s.xar", s->deps_dir, name);

        FILE *probe = fopen(dep_path, "rb");
        if (probe) {
            fclose(probe);

            /* File exists under deps_dir — load it. Version is checked only
             * when the dep was declared in xeno.project (PipelineState.deps).
             * Previously we required a declaration and ignored the file when
             * PipelineState had no deps list (e.g. xenovm project mode), which
             * produced "Unknown module '<libraries>'". */
            const PipelineDep *decl = pipeline_find_dep(s, name);
            bool ok = false;
            FILE *f = fopen(dep_path, "rb");
            if (f) {
                fseek(f, 0, SEEK_END);
                long sz = ftell(f);
                rewind(f);
                uint8_t *buf = (sz > 0) ? malloc((size_t)sz) : NULL;
                if (buf) {
                    size_t nr = fread(buf, 1, (size_t)sz, f);
                    XarArchive dep;
                    memset(&dep, 0, sizeof(dep));

                    if (nr != (size_t)sz) {
                        pipeline_set_error(name,
                            "Dependency '%s' could not be read (short read: %s)",
                            name, dep_path);
                    } else if (xar_read_mem(&dep, buf, (size_t)sz) != XAR_OK) {
                        pipeline_set_error(name,
                            "Dependency '%s' is not a valid .xar (%s)",
                            name, dep_path);
                    } else {
                        const char *actual = dep.manifest.version;
                        const char *need   = decl ? decl->version : "";

                        if (need[0] && actual[0] && !semver_satisfies(need, actual)) {
                            pipeline_set_error(name,
                                "Dependency '%s' version mismatch: need %s, found %s",
                                name, need, actual);
                            xar_archive_free(&dep);
                        } else {
                            ok = true;
                            for (int j = 0; j < dep.chunk_count; j++) {
                                Module *cm = calloc(1, sizeof(Module));
                                module_init(cm);
                                XbcResult br = xbc_read_mem(cm,
                                    dep.chunks[j].data, dep.chunks[j].size);
                                if (br == XBC_OK) {
                                    module_merge(staging, cm);
                                } else {
                                    if (br == XBC_ERR_BAD_VERSION)
                                        pipeline_set_error(name,
                                            "Dependency '%s' was built with a different "
                                            "bytecode version (tooling expects v%d) — "
                                            "rebuild %s.xar",
                                            name, XBC_VERSION, name);
                                    else
                                        pipeline_set_error(name,
                                            "Dependency '%s': bad bytecode chunk '%s': %s",
                                            name, dep.chunks[j].name,
                                            xbc_result_str(br));
                                    ok = false;
                                }
                                module_free(cm);
                                free(cm);
                            }
                            xar_archive_free(&dep);
                        }
                    }
                    free(buf);
                }
                fclose(f);
            }

            if (!ok) return false;

            if (s->sys_loaded_count < PIPELINE_MAX_SYS) {
                char *slot = s->sys_loaded[s->sys_loaded_count++];
                size_t nlen = strlen(name);
                if (nlen > 63) nlen = 63;
                memcpy(slot, name, nlen);
                slot[nlen] = '\0';
            }
            return true;
        }
    }

    /* ── 2. Embedded stdlib ───────────────────────────────────────────── */
    for (int i = 0; i < STDLIB_XAR_TOTAL_COUNT; i++) {
        if (strcmp(STDLIB_XAR_TABLE[i].name, name) != 0) continue;

        size_t sz = (size_t)(STDLIB_XAR_TABLE[i].end - STDLIB_XAR_TABLE[i].start);
        XarArchive ar;
        if (xar_read_mem(&ar, STDLIB_XAR_TABLE[i].start, sz) != XAR_OK) {
            pipeline_set_error(name,
                "Failed to read embedded standard library module '%s'", name);
            return false;
        }
        for (int j = 0; j < ar.chunk_count; j++) {
            Module *cm = calloc(1, sizeof(Module));
            module_init(cm);
            if (xbc_read_mem(cm, ar.chunks[j].data, ar.chunks[j].size) == XBC_OK) {
                module_merge(staging, cm);
                module_free(cm);
                free(cm);
            }
        }
        xar_archive_free(&ar);

        if (s->sys_loaded_count < PIPELINE_MAX_SYS) {
            char *slot = s->sys_loaded[s->sys_loaded_count++];
            size_t nlen = strlen(name);
            if (nlen > 63) nlen = 63;
            memcpy(slot, name, nlen);
            slot[nlen] = '\0';
        }
        return true;
    }

    /* ── 3. Not found anywhere ────────────────────────────────────────── */
    pipeline_set_error(name,
        "Unknown module '<%s>' — not part of the standard library, "
        "and no %s.xar was found in the project's deps folder "
        "(or it is not listed under [dependencies] in xeno.project)",
        name, name);
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
        /* Preserve line numbers: every newline we skip must appear in `out`
         * so checker/LSP diagnostics stay aligned with the user buffer. */
        while (*p == ' ' || *p == '\t' || *p == '\r' || *p == '\n') {
            if (*p == '\n') {
                out = pipeline_buf_append(out, len, cap, "\n", 1);
                if (!out) { *err = true; return out; }
            }
            p++;
        }
        if (!*p) break;
        if (p[0]=='/'&&p[1]=='/') {
            /* Line comment — skip text; trailing newline emitted above/next */
            while (*p && *p!='\n') p++;
            continue;
        }
        if (p[0]=='/'&&p[1]=='*') {
            p += 2;
            while (*p && !(p[0]=='*' && p[1]=='/')) {
                if (*p == '\n') {
                    out = pipeline_buf_append(out, len, cap, "\n", 1);
                    if (!out) { *err = true; return out; }
                }
                p++;
            }
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
                    /* Tag inlined stdlib so debugger does not attribute it to the user file */
                    char tag[160];
                    snprintf(tag, sizeof(tag), "// @xeno:file %s.xeno\n// @xeno:line 1\n", name);
                    out = pipeline_buf_append(out, len, cap, tag, strlen(tag));
                    if (!out) { *err = true; return out; }
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
                    pipeline_set_error(name, "Cannot open '%s' (imported from '%s')",
                                       fpath, label);
                    *err = true; return out;
                }
                char sub[1024] = "";
                pipeline_dir_of(fpath, sub, sizeof(sub));
                {
                    const char *base = fpath;
                    const char *sl = strrchr(fpath, '/');
#ifdef _WIN32
                    const char *bs = strrchr(fpath, '\\');
                    if (bs && (!sl || bs > sl)) sl = bs;
#endif
                    if (sl) base = sl + 1;
                    char tag[320];
                    snprintf(tag, sizeof(tag), "// @xeno:file %s\n// @xeno:line 1\n", base);
                    out = pipeline_buf_append(out, len, cap, tag, strlen(tag));
                    if (!out) { free(src); *err = true; return out; }
                }
                out = pipeline_resolve_imports(s, src, sub, fpath,
                                               out, len, cap, staging, err);
                free(src);
                if (*err || !out) return out;
            }
        }
        /* Do not emit an extra placeholder here: the newline that terminates
         * the import line is still at *p and the whitespace loop at the top
         * of the next iteration will copy it into `out`. Emitting both caused
         * every diagnostic after an import to shift down by one line. */
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

#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wunused-function"
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
    for (int i = 0; i < staging->class_count; i++) {
        const ClassDef *def = &staging->classes[i];
        const char *parent = NULL;
        if (def->parent_index >= 0 && def->parent_index < staging->class_count)
            parent = staging->classes[def->parent_index].name;
        checker_declare_class_from_def(checker, def, parent);
    }
}

/* Like pipeline_declare_staging but only declares classes at indices >= skip_first.
 * Used in project builds to skip stdlib classes (which will be declared from
 * source inlining) and only declare dep classes to the checker. */
static void pipeline_declare_staging_deps_only(Checker *checker,
                                                const Module *staging,
                                                int skip_first) {
    /* Functions: always declare all (stdlib fns are host-declared separately) */
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
    /* Classes: only declare those from deps (after the stdlib classes) */
    for (int i = skip_first; i < staging->class_count; i++) {
        const ClassDef *def = &staging->classes[i];
        const char *parent = NULL;
        if (def->parent_index >= 0 && def->parent_index < staging->class_count)
            parent = staging->classes[def->parent_index].name;
        checker_declare_class_from_def(checker, def, parent);
    }
}

/* ── Top-level entry point ────────────────────────────────────────────── */

static char *pipeline_prepare_with_state(const char *source, const char *source_path,
                                          PipelineState *ps,
                                          Module *staging, bool *err)
{
    pipeline_clear_error();

    /* Load stdlib first with deps_dir disabled so disk packages can't shadow it */
    char saved_deps[512];
    snprintf(saved_deps, sizeof(saved_deps), "%s", ps->deps_dir);
    ps->deps_dir[0] = '\0';

    pipeline_load_sys_module(ps, "core",        staging);
    pipeline_load_sys_module(ps, "math",        staging);
    pipeline_load_sys_module(ps, "collections", staging);

    /* Restore project deps_dir for user imports */
    snprintf(ps->deps_dir, sizeof(ps->deps_dir), "%s", saved_deps);

    char base_dir[1024] = "";
    if (source_path && *source_path)
        pipeline_dir_of(source_path, base_dir, sizeof(base_dir));

    size_t len = 0, cap = 65536;
    char *merged = malloc(cap);
    if (!merged) { *err = true; return NULL; }
    merged[0] = '\0';

    merged = pipeline_buf_append(merged, &len, &cap, "// @xeno:line 1\n", 16);
    if (!merged) { *err = true; return NULL; }

    *err = false;
    merged = pipeline_resolve_imports(ps, source, base_dir,
                                       source_path ? source_path : "<source>",
                                       merged, &len, &cap, staging, err);
    if (*err || !merged) { free(merged); return NULL; }
    return merged;
}

/*
 * pipeline_prepare — resolve all imports from `source` (at `source_path`),
 * populate `staging` with stdlib modules, and return the fully-merged source
 * string (heap-allocated, caller must free). Returns NULL on error.
 */
static char *pipeline_prepare_with_deps(const char *source, const char *source_path,
                                         const char *deps_dir,
                                         Module *staging, bool *err) {
    PipelineState ps;
    pipeline_state_init(&ps);
    pipeline_clear_error();
    /* Do NOT set deps_dir while loading stdlib — stdlib is always loaded from
     * the embedded binary (STDLIB_XAR_TABLE), never from disk. This prevents a
     * stale build/xar/math.xar on disk from silently shadowing the embedded one
     * when running `xenovm script.xeno` directly. */
    ps.deps_dir[0] = '\0';

    /* Always load all stdlib so compile-time class indices match
     * the runtime layout produced by xenovm (which loads all stdlib). */
    pipeline_load_sys_module(&ps, "core", staging);
    pipeline_load_sys_module(&ps, "math", staging);
    pipeline_load_sys_module(&ps, "collections", staging);

    /* Only now enable project dependency lookup (deps/<name>.xar), so a stray
     * deps/core.xar can never shadow the embedded stdlib loaded above. */
    if (deps_dir && *deps_dir)
        snprintf(ps.deps_dir, sizeof(ps.deps_dir), "%s", deps_dir);

    char base_dir[512] = "";
    if (source_path && *source_path)
        pipeline_dir_of(source_path, base_dir, sizeof(base_dir));

    size_t len = 0, cap = 65536;
    char *merged = malloc(cap);
    if (!merged) { *err = true; return NULL; }
    merged[0] = '\0';

    merged = pipeline_buf_append(merged, &len, &cap, "// @xeno:line 1\n", 16);
    if (!merged) { *err = true; return NULL; }

    *err = false;
    merged = pipeline_resolve_imports(&ps, source, base_dir,
                                       source_path ? source_path : "<source>",
                                       merged, &len, &cap, staging, err);
    if (*err || !merged) { free(merged); return NULL; }
    return merged;
}

/* pipeline_prepare — no project dependencies (stdlib only). */
static char *pipeline_prepare(const char *source, const char *source_path,
                               Module *staging, bool *err) {
    return pipeline_prepare_with_deps(source, source_path, NULL, staging, err);
}
/*
 * pipeline_prepare_project — like pipeline_prepare but with a deps_dir so
 * `import <name>` resolves against deps/name.xar before falling back to stdlib.
 * Used by xenoc build and xenovm project mode.
 */
static char *pipeline_prepare_project(const char *source,
                                       const char *source_path,
                                       const char *deps_dir,
                                       Module *staging, bool *err) {
    PipelineState ps;
    pipeline_state_init(&ps);
    if (deps_dir && *deps_dir)
        snprintf(ps.deps_dir, sizeof(ps.deps_dir), "%s", deps_dir);

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

/* Like pipeline_prepare_project but accepts a pre-initialised PipelineState
 * so stdlib names already in seed_state are not re-inlined. */
static char *pipeline_prepare_project_seeded(const char *source,
                                              const char *source_path,
                                              const char *deps_dir,
                                              Module *staging,
                                              const PipelineState *seed_state,
                                              bool *err) {
    PipelineState ps = *seed_state;  /* copy — inherits sys_loaded names */
    if (deps_dir && *deps_dir)
        snprintf(ps.deps_dir, sizeof(ps.deps_dir), "%s", deps_dir);

    char base_dir[512] = "";
    if (source_path && *source_path)
        pipeline_dir_of(source_path, base_dir, sizeof(base_dir));

    size_t len = 0, cap = 65536;
    char *merged = malloc(cap);
    if (!merged) { *err = true; return NULL; }
    merged[0] = '\0';

            /* Reset file + line so user source errors/debug map correctly */
    {
        const char *base = "source.xeno";
        if (source_path && *source_path) {
            base = source_path;
            const char *sl = strrchr(source_path, '/');
#ifdef _WIN32
            const char *bs = strrchr(source_path, '\\');
            if (bs && (!sl || bs > sl)) sl = bs;
#endif
            if (sl) base = sl + 1;
        }
        char tag[320];
        snprintf(tag, sizeof(tag), "// @xeno:file %s\n// @xeno:line 1\n", base);
        merged = pipeline_buf_append(merged, &len, &cap, tag, strlen(tag));
    }
    if (!merged) { *err = true; return NULL; }

    *err = false;
    merged = pipeline_resolve_imports(&ps, source, base_dir,
                                       source_path ? source_path : "<source>",
                                       merged, &len, &cap, staging, err);
    if (*err || !merged) { free(merged); return NULL; }
    return merged;
}

#pragma GCC diagnostic pop

#endif /* XENOSCRIPT_COMPILE_PIPELINE_H */