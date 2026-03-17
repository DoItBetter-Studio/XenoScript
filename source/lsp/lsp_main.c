/*
 * lsp_main.c — XenoScript Language Server (xenolsp)
 *
 * Single-threaded JSON-RPC server over stdio.
 * Protocol: LSP 3.17 (subset)
 *
 * Supported methods:
 *   initialize / initialized / shutdown / exit
 *   textDocument/didOpen
 *   textDocument/didChange
 *   textDocument/didClose
 *   textDocument/hover
 *   textDocument/definition
 *   textDocument/references
 *   $/cancelRequest   (no-op)
 */

#define _POSIX_C_SOURCE 200809L

#ifdef _WIN32
#  include <fcntl.h>
#  include <io.h>
#endif

#include "jsonrpc.h"
#include "json.h"
#include "doc_store.h"
#include "stub_gen.h"

#include "../../includes/checker.h"
#include "../../includes/bytecode.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdbool.h>

/* ── Global state ─────────────────────────────────────────────────────── */

static DocStore g_store;
static bool     g_shutdown = false;

/* ── Response helpers ─────────────────────────────────────────────────── */

/* Send a success response with a JSON result value */
static void respond_result(long long id, const char *result_json) {
    JsonBuf b;
    json_buf_init(&b);
    json_buf_raw(&b, "{\"jsonrpc\":\"2.0\",\"id\":");
    json_buf_int(&b, id);
    json_buf_raw(&b, ",\"result\":");
    json_buf_raw(&b, result_json);
    json_buf_raw(&b, "}");
    char *msg = json_buf_take(&b);
    json_buf_free(&b);
    if (msg) { jsonrpc_write_str(msg); free(msg); }
}

/* Send a null result (for methods with no meaningful return) */
static void respond_null(long long id) {
    respond_result(id, "null");
}

/* Send an error response */
static void respond_error(long long id, int code, const char *message) {
    JsonBuf b;
    json_buf_init(&b);
    json_buf_rawf(&b, "{\"jsonrpc\":\"2.0\",\"id\":%lld,"
                       "\"error\":{\"code\":%d,\"message\":", id, code);
    json_buf_str(&b, message);
    json_buf_raw(&b, "}}");
    char *msg = json_buf_take(&b);
    json_buf_free(&b);
    if (msg) { jsonrpc_write_str(msg); free(msg); }
}

/* Send a notification (no id) */
static void notify(const char *method, const char *params_json) {
    JsonBuf b;
    json_buf_init(&b);
    json_buf_raw(&b, "{\"jsonrpc\":\"2.0\",\"method\":");
    json_buf_str(&b, method);
    json_buf_raw(&b, ",\"params\":");
    json_buf_raw(&b, params_json);
    json_buf_raw(&b, "}");
    char *msg = json_buf_take(&b);
    json_buf_free(&b);
    if (msg) { jsonrpc_write_str(msg); free(msg); }
}

/* ── Diagnostics publisher ────────────────────────────────────────────── */

static void publish_diagnostics(const char *uri,
                                  Diagnostic *diags, int count) {
    JsonBuf b;
    json_buf_init(&b);
    json_buf_raw(&b, "{\"uri\":");
    json_buf_str(&b, uri);
    json_buf_raw(&b, ",\"diagnostics\":[");

    for (int i = 0; i < count; i++) {
        if (i > 0) json_buf_raw(&b, ",");
        /* severity: 1=Error, 2=Warning */
        int sev = diags[i].is_warning ? 2 : 1;
        json_buf_rawf(&b,
            "{\"range\":{\"start\":{\"line\":%d,\"character\":%d},"
                        "\"end\":{\"line\":%d,\"character\":%d}},"
            "\"severity\":%d,\"message\":",
            diags[i].line, diags[i].col,
            diags[i].line, diags[i].col + 1,
            sev);
        json_buf_str(&b, diags[i].message);
        json_buf_raw(&b, ",\"source\":\"xenolsp\"}");
    }

    json_buf_raw(&b, "]}");
    char *params = json_buf_take(&b);
    json_buf_free(&b);
    if (params) {
        notify("textDocument/publishDiagnostics", params);
        free(params);
    }
}

/* ── initialize ──────────────────────────────────────────────────────── */

static void handle_initialize(long long id) {
    /* Return server capabilities */
    const char *caps =
        "{"
        "  \"capabilities\": {"
        "    \"textDocumentSync\": {"
        "      \"openClose\": true,"
        "      \"change\": 1"   /* Full sync */
        "    },"
        "    \"hoverProvider\": true,"
        "    \"definitionProvider\": true,"
        "    \"referencesProvider\": true"
        "  },"
        "  \"serverInfo\": {"
        "    \"name\": \"xenolsp\","
        "    \"version\": \"0.1.0\""
        "  }"
        "}";
    respond_result(id, caps);
}

/* ── textDocument/didOpen ─────────────────────────────────────────────── */

static void handle_did_open(const char *params) {
    char *uri  = json_get_str(params, "textDocument.uri");
    char *text = json_get_str(params, "textDocument.text");
    if (!uri || !text) { free(uri); free(text); return; }

    DocEntry *entry = doc_store_get(&g_store, uri);
    if (entry) {
        Diagnostic diags[DOC_MAX_DIAGNOSTICS];
        int diag_count = 0;
        doc_store_run(&g_store, entry, text, diags, &diag_count);
        publish_diagnostics(uri, diags, diag_count);
    }

    free(uri); free(text);
}

/* ── textDocument/didChange ───────────────────────────────────────────── */

static void handle_did_change(const char *params) {
    char *uri = json_get_str(params, "textDocument.uri");
    /* Full sync: changes[0].text */
    char *text = json_get_str(params, "contentChanges.0.text");
    /* Some clients send it as an array object — also try direct */
    if (!text) text = json_get_str(params, "contentChanges.text");
    if (!uri || !text) { free(uri); free(text); return; }

    DocEntry *entry = doc_store_get(&g_store, uri);
    if (entry) {
        Diagnostic diags[DOC_MAX_DIAGNOSTICS];
        int diag_count = 0;
        doc_store_run(&g_store, entry, text, diags, &diag_count);
        publish_diagnostics(uri, diags, diag_count);
    }

    free(uri); free(text);
}

/* ── textDocument/didClose ───────────────────────────────────────────── */

static void handle_did_close(const char *params) {
    char *uri = json_get_str(params, "textDocument.uri");
    if (!uri) return;
    doc_store_close(&g_store, uri);
    /* Publish empty diagnostics to clear squiggles */
    publish_diagnostics(uri, NULL, 0);
    free(uri);
}

/* ── Hover ────────────────────────────────────────────────────────────── */

/*
 * Format a Symbol into a markdown hover string.
 * Returns a heap-allocated string; caller frees.
 */
static char *format_hover(const Symbol *sym) {
    if (!sym) return NULL;

    JsonBuf b;
    json_buf_init(&b);

    /* Build a short type annotation string */
    char type_str[128] = "unknown";
    switch (sym->kind) {
        case SYM_VAR: {
            /* Format type.kind as a keyword */
            const char *tk = "unknown";
            switch (sym->type.kind) {
                case TYPE_INT:    tk = "int";    break;
                case TYPE_FLOAT:  tk = "float";  break;
                case TYPE_BOOL:   tk = "bool";   break;
                case TYPE_STRING: tk = "string"; break;
                case TYPE_VOID:   tk = "void";   break;
                case TYPE_OBJECT: tk = sym->type.class_name[0]
                                        ? sym->type.class_name : "object"; break;
                case TYPE_ENUM:   tk = sym->type.class_name[0]
                                        ? sym->type.class_name : "enum"; break;
                case TYPE_ANY:    tk = "any";    break;
                default:          tk = "object"; break;
            }
            snprintf(type_str, sizeof(type_str), "%s%s",
                     tk, sym->type.is_nullable ? "?" : "");
            snprintf(type_str, sizeof(type_str), "(var) %.*s: %s%s",
                     sym->length, sym->name, tk,
                     sym->type.is_nullable ? "?" : "");
            break;
        }
        case SYM_FN: {
            /* Show return type */
            const char *rt = "void";
            switch (sym->type.kind) {
                case TYPE_INT:    rt = "int";    break;
                case TYPE_FLOAT:  rt = "float";  break;
                case TYPE_BOOL:   rt = "bool";   break;
                case TYPE_STRING: rt = "string"; break;
                case TYPE_VOID:   rt = "void";   break;
                case TYPE_OBJECT: rt = sym->type.class_name[0]
                                        ? sym->type.class_name : "object"; break;
                default:          rt = "object"; break;
            }
            snprintf(type_str, sizeof(type_str), "(function) %.*s(): %s",
                     sym->length, sym->name, rt);
            break;
        }
        case SYM_CLASS:
            snprintf(type_str, sizeof(type_str), "(class) %.*s",
                     sym->length, sym->name);
            break;
        case SYM_ENUM:
            snprintf(type_str, sizeof(type_str), "(enum) %.*s",
                     sym->length, sym->name);
            break;
        case SYM_INTERFACE:
            snprintf(type_str, sizeof(type_str), "(interface) %.*s",
                     sym->length, sym->name);
            break;
        case SYM_EVENT:
            snprintf(type_str, sizeof(type_str), "(event) %.*s",
                     sym->length, sym->name);
            break;
    }

    /* Location annotation */
    char loc[256] = "";
    if (sym->def_file && sym->def_line > 0)
        snprintf(loc, sizeof(loc), "\n\n*Defined at %s:%d*",
                 sym->def_file, sym->def_line + 1);  /* +1: display as 1-based */

    /* Wrap in LSP Hover markdown format */
    json_buf_raw(&b, "{\"contents\":{\"kind\":\"markdown\",\"value\":");
    char full[512];
    snprintf(full, sizeof(full), "```xenoscript\n%s\n```%s", type_str, loc);
    json_buf_str(&b, full);
    json_buf_raw(&b, "}}");

    char *out = json_buf_take(&b);
    json_buf_free(&b);
    return out;
}

static void handle_hover(long long id, const char *params) {
    char *uri = json_get_str(params, "textDocument.uri");
    long long line = 0, col = 0;
    json_get_int(params, "position.line",      &line);
    json_get_int(params, "position.character", &col);

    if (!uri) { respond_null(id); return; }

    DocEntry *entry = doc_store_get(&g_store, uri);
    free(uri);

    if (!entry || !entry->checker) { respond_null(id); return; }

    /* LSP positions are 0-based; checker also uses 0-based lines (after
     * pipeline_prepare's blank-line preservation aligns them). Columns are
     * 0-based in LSP, 1-based in checker — add 1 for col only. */
    const Symbol *sym = checker_find_symbol_at(entry->checker,
                                                (int)line, (int)col + 1);
    if (!sym) { respond_null(id); return; }

    char *hover_json = format_hover(sym);
    if (hover_json) {
        respond_result(id, hover_json);
        free(hover_json);
    } else {
        respond_null(id);
    }
}

/* ── Definition ───────────────────────────────────────────────────────── */

static void handle_definition(long long id, const char *params) {
    char *uri = json_get_str(params, "textDocument.uri");
    long long line = 0, col = 0;
    json_get_int(params, "position.line",      &line);
    json_get_int(params, "position.character", &col);

    if (!uri) { respond_null(id); return; }

    DocEntry *entry = doc_store_get(&g_store, uri);
    free(uri);

    if (!entry || !entry->checker) { respond_null(id); return; }

    const Symbol *sym = checker_find_definition(entry->checker,
                                                 (int)line, (int)col + 1);
    if (!sym) { respond_null(id); return; }

    JsonBuf b;
    json_buf_init(&b);

    if (sym->def_file && sym->def_line > 0) {
        /* Known source location */
        char def_uri[DOC_URI_MAX];
        if (strncmp(sym->def_file, "file://", 7) == 0) {
            snprintf(def_uri, sizeof(def_uri), "%s", sym->def_file);
        } else {
            snprintf(def_uri, sizeof(def_uri), "file://%s", sym->def_file);
        }
        int def_line = sym->def_line;  /* already 0-based (checker line == LSP line) */
        int def_col  = sym->def_col  > 0 ? sym->def_col - 1 : 0;

        json_buf_raw(&b, "{\"uri\":");
        json_buf_str(&b, def_uri);
        json_buf_rawf(&b, ",\"range\":{\"start\":{\"line\":%d,\"character\":%d},"
                          "\"end\":{\"line\":%d,\"character\":%d}}}",
                      def_line, def_col, def_line, def_col + sym->length);
    } else {
        /* No source — try to generate a stub via stub_gen_class */
        const ClassDef *cdef = NULL;
        if ((sym->kind == SYM_CLASS || sym->kind == SYM_ENUM ||
             sym->kind == SYM_INTERFACE) && sym->class_def) {
            cdef = (const ClassDef *)sym->class_def;
        }

        if (cdef) {
            char *stub = stub_gen_class(cdef, NULL);
            if (stub) {
                /* Return a special xeno-stub URI — VS Code content provider
                 * must be registered to handle this scheme */
                char stub_uri[256];
                snprintf(stub_uri, sizeof(stub_uri),
                         "xeno-stub:///%s", cdef->name);

                json_buf_raw(&b, "{\"uri\":");
                json_buf_str(&b, stub_uri);
                json_buf_raw(&b,
                    ",\"range\":{\"start\":{\"line\":0,\"character\":0},"
                    "\"end\":{\"line\":0,\"character\":0}},"
                    "\"_stubContent\":");
                json_buf_str(&b, stub);
                json_buf_raw(&b, "}");
                free(stub);
            } else {
                json_buf_free(&b);
                respond_null(id);
                return;
            }
        } else {
            json_buf_free(&b);
            respond_null(id);
            return;
        }
    }

    char *result = json_buf_take(&b);
    json_buf_free(&b);
    if (result) { respond_result(id, result); free(result); }
    else respond_null(id);
}

/* ── References ───────────────────────────────────────────────────────── */

static void handle_references(long long id, const char *params) {
    char *uri = json_get_str(params, "textDocument.uri");
    long long line = 0, col = 0;
    json_get_int(params, "position.line",      &line);
    json_get_int(params, "position.character", &col);

    if (!uri) { respond_null(id); return; }

    DocEntry *entry = doc_store_get(&g_store, uri);

    if (!entry || !entry->checker) {
        free(uri); respond_null(id); return;
    }

    UsageRecord usages[512];
    int count = checker_usages_of(entry->checker,
                                   (int)line, (int)col + 1,
                                   usages, 512);
    free(uri);

    JsonBuf b;
    json_buf_init(&b);
    json_buf_raw(&b, "[");

    for (int i = 0; i < count; i++) {
        if (i > 0) json_buf_raw(&b, ",");

        const char *ref_file = usages[i].file ? usages[i].file : "";
        char ref_uri[DOC_URI_MAX];
        if (strncmp(ref_file, "file://", 7) == 0) {
            snprintf(ref_uri, sizeof(ref_uri), "%s", ref_file);
        } else if (ref_file[0]) {
            snprintf(ref_uri, sizeof(ref_uri), "file://%s", ref_file);
        } else {
            ref_uri[0] = '\0';
        }

        int ref_line = usages[i].line;  /* already 0-based */
        int ref_col  = usages[i].col  > 0 ? usages[i].col  - 1 : 0;
        int ref_end  = ref_col + usages[i].length;

        json_buf_raw(&b, "{\"uri\":");
        json_buf_str(&b, ref_uri);
        json_buf_rawf(&b,
            ",\"range\":{\"start\":{\"line\":%d,\"character\":%d},"
                        "\"end\":{\"line\":%d,\"character\":%d}}}",
            ref_line, ref_col, ref_line, ref_end);
    }

    json_buf_raw(&b, "]");
    char *result = json_buf_take(&b);
    json_buf_free(&b);
    if (result) { respond_result(id, result); free(result); }
    else respond_null(id);
}

/* ── Main dispatch loop ───────────────────────────────────────────────── */

static void dispatch(const char *json) {
    char   *method = json_get_str(json, "method");
    long long id   = -1;
    json_get_int(json, "id", &id);

    char   *params_raw = json_get_raw(json, "params");
    const char *params = params_raw ? params_raw : "{}";

    if (!method) { free(params_raw); return; }

    /* ── Lifecycle ── */
    if (strcmp(method, "initialize") == 0) {
        handle_initialize(id);

    } else if (strcmp(method, "initialized") == 0) {
        /* No-op notification */

    } else if (strcmp(method, "shutdown") == 0) {
        g_shutdown = true;
        respond_null(id);

    } else if (strcmp(method, "exit") == 0) {
        /* exit without prior shutdown is an error per spec */
        exit(g_shutdown ? 0 : 1);

    /* ── Document sync ── */
    } else if (strcmp(method, "textDocument/didOpen") == 0) {
        handle_did_open(params);

    } else if (strcmp(method, "textDocument/didChange") == 0) {
        handle_did_change(params);

    } else if (strcmp(method, "textDocument/didClose") == 0) {
        handle_did_close(params);

    /* ── LSP features ── */
    } else if (strcmp(method, "textDocument/hover") == 0) {
        handle_hover(id, params);

    } else if (strcmp(method, "textDocument/definition") == 0) {
        handle_definition(id, params);

    } else if (strcmp(method, "textDocument/references") == 0) {
        handle_references(id, params);

    /* ── Cancellation / unknown ── */
    } else if (strcmp(method, "$/cancelRequest") == 0) {
        /* No-op — single-threaded, requests complete immediately */

    } else {
        /* Unknown method with an id → MethodNotFound */
        if (id >= 0) respond_error(id, -32601, "Method not found");
    }

    free(method);
    free(params_raw);
}

/* ── Entry point ──────────────────────────────────────────────────────── */

int main(void) {
    /* Set stdio to binary mode so \r\n is not mangled on Windows */
#ifdef _WIN32
    _setmode(_fileno(stdin),  _O_BINARY);
    _setmode(_fileno(stdout), _O_BINARY);
#endif

    /* Disable buffering on stdout so responses go out immediately */
    setvbuf(stdout, NULL, _IONBF, 0);

    doc_store_init(&g_store);

    /* Main read loop */
    for (;;) {
        char *msg = jsonrpc_read();
        if (!msg) break; /* EOF */
        dispatch(msg);
        free(msg);
    }

    doc_store_free(&g_store);
    return 0;
}
