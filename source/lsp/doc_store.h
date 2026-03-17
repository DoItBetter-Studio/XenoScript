/*
 * doc_store.h — Open document management for xenolsp
 *
 * Each open document gets its own slot with:
 *   - A copy of the document text (updated on didChange)
 *   - The URI the client sent us
 *   - The last successful checker state (for hover/definition/references)
 *   - The last diagnostics list (from checker.errors[])
 *   - The staging Module (stdlib + deps, shared reference)
 *
 * Strategy: last-good-state.
 *   On every didOpen/didChange we run the full pipeline.
 *   If parse + check succeeds, we update the stored checker state.
 *   If it fails, we keep the previous checker state for queries but
 *   always publish fresh diagnostics from the failed run.
 */

#ifndef DOC_STORE_H
#define DOC_STORE_H

#include "../../includes/checker.h"
#include "../../includes/compiler.h"
#include "../../includes/ast.h"
#include "../../includes/arena.h"
#include <stdbool.h>

#define DOC_STORE_MAX 32
#define DOC_URI_MAX   1024

/* One open document */
typedef struct {
    bool   in_use;
    char   uri[DOC_URI_MAX];

    /* Current text — heap-allocated, always valid */
    char  *text;

    /* Last successfully type-checked state.
     * checker_ok is true only if the last run completed without errors. */
    bool   checker_ok;

    /* The checker, program, and arena from the last successful run.
     * These are heap-allocated and replaced on each successful run.
     * On a failed run they are kept for hover/definition queries. */
    Checker *checker;   /* NULL until first successful run */
    Program *program;   /* NULL until first successful run */
    Arena   *arena;     /* NULL until first successful run — owns program nodes */

    /* Merged source from last pipeline run (stored for path computation) */
    char *merged_source;

    /* Staging module — loaded once, shared across all runs for this doc.
     * Heap-allocated. */
    Module *staging;

} DocEntry;

typedef struct {
    DocEntry entries[DOC_STORE_MAX];
    int      count;

    /* Global staging module, shared across all documents.
     * Loaded once at startup with all stdlib XARs. */
    Module  *global_staging;
} DocStore;

/* Initialise the store. Loads stdlib into global_staging. */
void doc_store_init(DocStore *store);
void doc_store_free(DocStore *store);

/* Find or create an entry for `uri`. Returns NULL if store is full. */
DocEntry *doc_store_get(DocStore *store, const char *uri);

/* Remove an entry (on didClose). */
void doc_store_close(DocStore *store, const char *uri);

/*
 * Run the full compile pipeline on `entry` using `text`.
 * Updates entry->checker/program/arena on success.
 * Always updates entry->text.
 * Returns true if lex+parse+check all succeeded.
 *
 * diagnostics_out: caller-supplied buffer for error messages.
 * diag_count_out:  number of errors written.
 */
#define DOC_MAX_DIAGNOSTICS 64

typedef struct {
    int  line;       /* 0-based for LSP */
    int  col;        /* 0-based for LSP */
    char message[256];
    bool is_warning;
} Diagnostic;

bool doc_store_run(DocStore *store, DocEntry *entry, const char *text,
                   Diagnostic *diags_out, int *diag_count_out);

#endif /* DOC_STORE_H */
