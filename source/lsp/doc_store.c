/*
 * doc_store.c — Open document management for xenolsp
 */

#define _POSIX_C_SOURCE 200809L

/* Include the pipeline header (defines static helpers) */
#include "../compiler/compile_pipeline.h"

#include "doc_store.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ── Helpers ─────────────────────────────────────────────────────────── */

/* Convert a file:// URI to a filesystem path (in-place). */
static const char *uri_to_path(const char *uri) {
    if (strncmp(uri, "file://", 7) == 0) return uri + 7;
    return uri;
}

static void free_checker_state(DocEntry *e) {
    if (e->checker) {
        /* source_file is strdup'd in doc_store_run — free it here */
        if (e->checker->source_file)
            free((char *)e->checker->source_file);
        free(e->checker);
        e->checker = NULL;
    }
    if (e->arena)   { arena_free(e->arena); free(e->arena); e->arena = NULL; }
    if (e->program) { free(e->program); e->program = NULL; }
    if (e->merged_source) { free(e->merged_source); e->merged_source = NULL; }
}

/* ── Public API ──────────────────────────────────────────────────────── */

void doc_store_init(DocStore *store) {
    memset(store, 0, sizeof(*store));

    /* Load global staging (stdlib) once */
    store->global_staging = calloc(1, sizeof(Module));
    module_init(store->global_staging);

    PipelineState ps;
    pipeline_state_init(&ps);
    pipeline_load_sys_module(&ps, "core",        store->global_staging);
    pipeline_load_sys_module(&ps, "math",        store->global_staging);
    pipeline_load_sys_module(&ps, "collections", store->global_staging);
}

void doc_store_free(DocStore *store) {
    for (int i = 0; i < DOC_STORE_MAX; i++) {
        DocEntry *e = &store->entries[i];
        if (!e->in_use) continue;
        free(e->text);
        free_checker_state(e);
        if (e->staging) { module_free(e->staging); free(e->staging); }
    }
    if (store->global_staging) {
        module_free(store->global_staging);
        free(store->global_staging);
    }
    memset(store, 0, sizeof(*store));
}

DocEntry *doc_store_get(DocStore *store, const char *uri) {
    /* Find existing */
    for (int i = 0; i < DOC_STORE_MAX; i++) {
        if (store->entries[i].in_use &&
            strcmp(store->entries[i].uri, uri) == 0)
            return &store->entries[i];
    }
    /* Allocate new slot */
    for (int i = 0; i < DOC_STORE_MAX; i++) {
        DocEntry *e = &store->entries[i];
        if (!e->in_use) {
            memset(e, 0, sizeof(*e));
            e->in_use = true;
            strncpy(e->uri, uri, DOC_URI_MAX - 1);

            /* Give this doc its own staging module (copy of global) */
            e->staging = calloc(1, sizeof(Module));
            module_init(e->staging);
            module_merge(e->staging, store->global_staging);

            store->count++;
            return e;
        }
    }
    return NULL; /* store full */
}

void doc_store_close(DocStore *store, const char *uri) {
    for (int i = 0; i < DOC_STORE_MAX; i++) {
        DocEntry *e = &store->entries[i];
        if (!e->in_use || strcmp(e->uri, uri) != 0) continue;
        free(e->text);
        free_checker_state(e);
        if (e->staging) { module_free(e->staging); free(e->staging); }
        memset(e, 0, sizeof(*e));
        store->count--;
        return;
    }
}

bool doc_store_run(DocStore *store, DocEntry *entry,
                   const char *text,
                   Diagnostic *diags_out, int *diag_count_out) {
    (void)store;

    *diag_count_out = 0;

    /* Update stored text */
    free(entry->text);
    entry->text = strdup(text);

    const char *file_path = uri_to_path(entry->uri);

    /* ── 1. Prepare (resolve imports, merge source) ── */
    Module *staging = calloc(1, sizeof(Module));
    module_init(staging);
    module_merge(staging, entry->staging); /* copy global stdlib */

    bool import_err = false;
    char *merged = pipeline_prepare(text, file_path, staging, &import_err);

    if (import_err || !merged) {
        /* Emit a single diagnostic for import failure */
        if (diags_out && *diag_count_out < DOC_MAX_DIAGNOSTICS) {
            Diagnostic *d = &diags_out[(*diag_count_out)++];
            d->line       = 0;
            d->col        = 0;
            d->is_warning = false;
            snprintf(d->message, sizeof(d->message),
                     "Failed to resolve imports");
        }
        free(merged);
        module_free(staging); free(staging);
        entry->checker_ok = false;
        return false;
    }

    /* ── 2. Lex + Parse ── */
    Lexer   *lexer   = malloc(sizeof(Lexer));
    Parser  *parser  = malloc(sizeof(Parser));
    lexer_init(lexer, merged);
    parser_init(parser, lexer);

    Program *program = malloc(sizeof(Program));
    *program = parser_parse(parser);

    if (parser->had_error) {
        /* Collect parse errors as diagnostics.
         * The parser only exposes had_error; use a generic message. */
        if (diags_out && *diag_count_out < DOC_MAX_DIAGNOSTICS) {
            Diagnostic *d = &diags_out[(*diag_count_out)++];
            d->line       = 0;
            d->col        = 0;
            d->is_warning = false;
            snprintf(d->message, sizeof(d->message), "Parse error");
        }
        /* Don't update checker state — keep last-good */
        free(merged); free(program);
        arena_free(&parser->arena);
        free(parser); free(lexer);
        module_free(staging); free(staging);
        entry->checker_ok = false;
        return false;
    }

    /* ── 3. Type-check ── */
    Checker *checker = malloc(sizeof(Checker));
    Arena   *arena   = malloc(sizeof(Arena));
    *arena = parser->arena; /* take ownership of parser's arena */

    checker_init(checker, arena);
    checker->source_file = strdup(file_path); /* LSP needs this */

    /* Declare stdlib host functions */
    Type void_t = type_void(), any_t = type_any();
    checker_declare_host(checker, "print", void_t, &any_t, 1);

    CompilerHostTable host_table;
    compiler_host_table_init(&host_table);
    compiler_host_table_add_any(&host_table, "print", 0, 1);
    stdlib_declare_host_fns(checker, &host_table);
    pipeline_declare_staging(checker, staging);

    bool check_ok = checker_check(checker, program);

    /* Collect checker diagnostics */
    int max_diags = DOC_MAX_DIAGNOSTICS;
    for (int i = 0; i < checker->error_count && *diag_count_out < max_diags; i++) {
        Diagnostic *d = &diags_out[(*diag_count_out)++];
        /* checker errors are 1-based lines — convert to 0-based for LSP */
        d->line       = checker->errors[i].line > 0 ? checker->errors[i].line - 1 : 0;
        d->col        = 0;
        d->is_warning = checker->errors[i].is_warning;
        strncpy(d->message, checker->errors[i].message, sizeof(d->message) - 1);
        d->message[sizeof(d->message) - 1] = '\0';
    }

    if (check_ok) {
        /* Replace stored good state */
        free_checker_state(entry);
        entry->checker       = checker;
        entry->program       = program;
        entry->arena         = arena;
        entry->merged_source = merged;
        entry->checker_ok    = true;
    } else {
        /* Keep previous good state; discard this run's data */
        free(merged);
        if (checker->source_file) free((char *)checker->source_file);
        free(checker);
        arena_free(arena); free(arena);
        free(program);
        entry->checker_ok = false;
    }

    free(parser); free(lexer);
    module_free(staging); free(staging);

    return check_ok;
}
