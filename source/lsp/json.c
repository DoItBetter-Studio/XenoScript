/*
 * json.c — Minimal JSON parser and emitter
 */

#define _POSIX_C_SOURCE 200809L
#include "json.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#include <ctype.h>

/* ── Internal helpers ────────────────────────────────────────────────── */

/* Skip whitespace */
static const char *skip_ws(const char *p) {
    while (*p && (*p == ' ' || *p == '\t' || *p == '\r' || *p == '\n')) p++;
    return p;
}

/* Decode a JSON string starting at the opening quote.
 * Returns pointer past the closing quote, or NULL on malformed input.
 * Writes decoded text into out (heap-allocated). */
static const char *decode_string(const char *p, char **out) {
    if (*p != '"') { *out = NULL; return NULL; }
    p++;

    size_t cap = 256, len = 0;
    char *buf = malloc(cap);
    if (!buf) { *out = NULL; return NULL; }

    while (*p && *p != '"') {
        char c;
        if (*p == '\\') {
            p++;
            switch (*p) {
                case '"':  c = '"';  break;
                case '\\': c = '\\'; break;
                case '/':  c = '/';  break;
                case 'n':  c = '\n'; break;
                case 'r':  c = '\r'; break;
                case 't':  c = '\t'; break;
                case 'b':  c = '\b'; break;
                case 'f':  c = '\f'; break;
                case 'u':  /* \uXXXX — simplified: skip 4 hex digits, emit '?' */
                    for (int i = 0; i < 4 && p[1]; i++) p++;
                    c = '?';
                    break;
                default:   c = *p;   break;
            }
        } else {
            c = *p;
        }
        if (len + 1 >= cap) {
            cap *= 2;
            char *nb = realloc(buf, cap);
            if (!nb) { free(buf); *out = NULL; return NULL; }
            buf = nb;
        }
        buf[len++] = c;
        p++;
    }
    buf[len] = '\0';
    if (*p == '"') p++;
    *out = buf;
    return p;
}

/* Skip a JSON value (any type) and return pointer past it. */
static const char *skip_value(const char *p);

static const char *skip_object(const char *p) {
    /* p points at '{' */
    p++;
    p = skip_ws(p);
    if (*p == '}') return p + 1;
    for (;;) {
        p = skip_ws(p);
        if (*p == '"') { char *tmp = NULL; p = decode_string(p, &tmp); free(tmp); }
        p = skip_ws(p);
        if (*p == ':') p++;
        p = skip_ws(p);
        p = skip_value(p);
        p = skip_ws(p);
        if (*p == ',') { p++; continue; }
        if (*p == '}') return p + 1;
        break;
    }
    return p;
}

static const char *skip_array(const char *p) {
    /* p points at '[' */
    p++;
    p = skip_ws(p);
    if (*p == ']') return p + 1;
    for (;;) {
        p = skip_ws(p);
        p = skip_value(p);
        p = skip_ws(p);
        if (*p == ',') { p++; continue; }
        if (*p == ']') return p + 1;
        break;
    }
    return p;
}

static const char *skip_value(const char *p) {
    p = skip_ws(p);
    if (!*p) return p;
    if (*p == '{') return skip_object(p);
    if (*p == '[') return skip_array(p);
    if (*p == '"') {
        char *tmp = NULL;
        const char *r = decode_string(p, &tmp);
        free(tmp);
        return r ? r : p + 1;
    }
    /* number, true, false, null */
    while (*p && *p != ',' && *p != '}' && *p != ']' && *p != '\r' && *p != '\n') p++;
    return p;
}

/*
 * Find a key in a JSON object (p points to the '{').
 * Returns pointer to the VALUE (past the colon), or NULL if not found.
 * Searches only one level deep; call recursively via key_path splitting.
 */
static const char *find_key(const char *p, const char *key, size_t keylen) {
    p = skip_ws(p);
    if (*p != '{') return NULL;
    p++;
    while (*p) {
        p = skip_ws(p);
        if (*p == '}') return NULL;
        if (*p != '"') return NULL;

        char *k = NULL;
        const char *after_key = decode_string(p, &k);
        if (!after_key || !k) return NULL;

        bool match = (strlen(k) == keylen && memcmp(k, key, keylen) == 0);
        free(k);
        p = after_key;

        p = skip_ws(p);
        if (*p != ':') return NULL;
        p++;
        p = skip_ws(p);

        if (match) return p; /* caller reads the value from here */

        p = skip_value(p);
        p = skip_ws(p);
        if (*p == ',') { p++; continue; }
        if (*p == '}') return NULL;
        return NULL;
    }
    return NULL;
}

/*
 * Navigate a dotted path and return pointer to the final value, or NULL.
 */
static const char *navigate(const char *json, const char *key_path) {
    const char *p = json;
    const char *seg = key_path;

    while (*seg) {
        const char *dot = strchr(seg, '.');
        size_t seglen = dot ? (size_t)(dot - seg) : strlen(seg);

        p = skip_ws(p);
        p = find_key(p, seg, seglen);
        if (!p) return NULL;

        seg = dot ? dot + 1 : seg + seglen;
    }
    return skip_ws(p);
}

/* ── Public parser API ───────────────────────────────────────────────── */

char *json_get_str(const char *json, const char *key_path) {
    const char *p = navigate(json, key_path);
    if (!p || *p != '"') return NULL;
    char *out = NULL;
    decode_string(p, &out);
    return out;
}

bool json_get_int(const char *json, const char *key_path, long long *out) {
    const char *p = navigate(json, key_path);
    if (!p) return false;
    /* null → 0 treated as missing */
    if (strncmp(p, "null", 4) == 0) return false;
    char *end;
    long long v = strtoll(p, &end, 10);
    if (end == p) return false;
    *out = v;
    return true;
}

bool json_has_key(const char *json, const char *key_path) {
    return navigate(json, key_path) != NULL;
}

char *json_get_raw(const char *json, const char *key_path) {
    const char *p = navigate(json, key_path);
    if (!p) return NULL;
    const char *end = skip_value(p);
    size_t len = (size_t)(end - p);
    char *buf = malloc(len + 1);
    if (!buf) return NULL;
    memcpy(buf, p, len);
    buf[len] = '\0';
    return buf;
}

/* ── Emitter ─────────────────────────────────────────────────────────── */

void json_buf_init(JsonBuf *b) {
    b->cap  = 4096;
    b->len  = 0;
    b->data = malloc(b->cap);
    if (b->data) b->data[0] = '\0';
}

void json_buf_free(JsonBuf *b) {
    free(b->data);
    b->data = NULL;
    b->len  = 0;
    b->cap  = 0;
}

static void json_buf_grow(JsonBuf *b, size_t need) {
    if (b->len + need + 1 <= b->cap) return;
    while (b->cap < b->len + need + 1) b->cap *= 2;
    b->data = realloc(b->data, b->cap);
}

void json_buf_raw(JsonBuf *b, const char *s) {
    size_t n = strlen(s);
    json_buf_grow(b, n);
    memcpy(b->data + b->len, s, n);
    b->len += n;
    b->data[b->len] = '\0';
}

void json_buf_rawf(JsonBuf *b, const char *fmt, ...) {
    char tmp[1024];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(tmp, sizeof(tmp), fmt, ap);
    va_end(ap);
    json_buf_raw(b, tmp);
}

void json_buf_str(JsonBuf *b, const char *s) {
    if (!s) { json_buf_raw(b, "null"); return; }
    json_buf_raw(b, "\"");
    for (const char *p = s; *p; p++) {
        char esc[8];
        switch (*p) {
            case '"':  json_buf_raw(b, "\\\""); break;
            case '\\': json_buf_raw(b, "\\\\"); break;
            case '\n': json_buf_raw(b, "\\n");  break;
            case '\r': json_buf_raw(b, "\\r");  break;
            case '\t': json_buf_raw(b, "\\t");  break;
            default:
                if ((unsigned char)*p < 0x20) {
                    snprintf(esc, sizeof(esc), "\\u%04x", (unsigned char)*p);
                    json_buf_raw(b, esc);
                } else {
                    char ch[2] = { *p, '\0' };
                    json_buf_raw(b, ch);
                }
                break;
        }
    }
    json_buf_raw(b, "\"");
}

void json_buf_int(JsonBuf *b, long long v) {
    char tmp[32];
    snprintf(tmp, sizeof(tmp), "%lld", v);
    json_buf_raw(b, tmp);
}

char *json_buf_take(JsonBuf *b) {
    char *out = malloc(b->len + 1);
    if (!out) return NULL;
    memcpy(out, b->data, b->len + 1);
    return out;
}
