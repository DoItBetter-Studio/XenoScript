/*
 * json.h — Minimal JSON parser and emitter for LSP messages
 *
 * We don't need a full JSON library. LSP messages are structured and
 * predictable. This gives us:
 *   - String extraction by key (nested path: "params.textDocument.uri")
 *   - Integer extraction by key
 *   - A simple string-builder for emitting JSON responses
 *
 * Limitations (intentional):
 *   - No recursive value tree — just key lookups
 *   - String values are returned as heap-allocated copies (caller frees)
 *   - Numbers returned as long long or double
 *   - Does not validate full JSON correctness
 */

#ifndef JSON_H
#define JSON_H

#include <stddef.h>
#include <stdbool.h>

/* ── Parser ──────────────────────────────────────────────────────────── */

/*
 * Extract a string value from a JSON object by dotted key path.
 * e.g. json_get_str(msg, "params.textDocument.uri")
 * Returns a heap-allocated null-terminated string, or NULL if not found.
 * Caller must free().
 *
 * Handles JSON string escaping for \" and \\.
 */
char *json_get_str(const char *json, const char *key_path);

/*
 * Extract an integer value by dotted key path.
 * Returns the value via *out; returns false if key not found or not a number.
 */
bool json_get_int(const char *json, const char *key_path, long long *out);

/*
 * Check whether a key path exists in the JSON.
 */
bool json_has_key(const char *json, const char *key_path);

/*
 * Extract the raw JSON substring of a value by dotted key path.
 * Useful for passing a sub-object to another json_get_* call.
 * Returns heap-allocated string. Caller must free().
 */
char *json_get_raw(const char *json, const char *key_path);

/* ── Emitter ─────────────────────────────────────────────────────────── */

/*
 * Simple string builder for JSON emission.
 */
typedef struct {
    char  *data;
    size_t len;
    size_t cap;
} JsonBuf;

void json_buf_init(JsonBuf *b);
void json_buf_free(JsonBuf *b);

/* Append raw text (no escaping) */
void json_buf_raw(JsonBuf *b, const char *s);
void json_buf_rawf(JsonBuf *b, const char *fmt, ...);

/* Append a JSON-escaped string value (including surrounding quotes) */
void json_buf_str(JsonBuf *b, const char *s);

/* Append a JSON integer */
void json_buf_int(JsonBuf *b, long long v);

/* Convenience: return the built string (heap copy). Caller frees. */
char *json_buf_take(JsonBuf *b);

#endif /* JSON_H */
