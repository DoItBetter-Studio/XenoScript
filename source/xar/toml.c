/*
 * toml.c — Minimal TOML parser for xeno.project files.
 *
 * Handles only the subset used by xeno.project:
 *   [section]
 *   key = "string value"
 *   key = 'string value'   (single-quoted, no escapes)
 *   # comments
 *
 * Keys before any section header are stored under section "".
 * All other TOML features (arrays, inline tables, integers, booleans,
 * multi-line strings, dotted keys) are silently skipped.
 */

#include "toml.h"
#include <string.h>
#include <stdio.h>

/* ── Helpers ──────────────────────────────────────────────────────────── */

static const char *skip_ws(const char *p) {
    while (*p == ' ' || *p == '\t') p++;
    return p;
}

static const char *skip_line(const char *p) {
    while (*p && *p != '\n') p++;
    if (*p == '\n') p++;
    return p;
}

/* Parse a quoted string (double or single quotes) into buf[cap].
 * Returns pointer past closing quote, or NULL on error. */
static const char *parse_quoted(const char *p, char *buf, size_t cap) {
    char quote = *p++;   /* consume opening quote */
    size_t i = 0;
    while (*p && *p != quote && *p != '\n') {
        if (quote == '"' && *p == '\\') {
            p++;
            if (!*p) return NULL;
            char esc = *p++;
            char c;
            switch (esc) {
                case 'n':  c = '\n'; break;
                case 't':  c = '\t'; break;
                case 'r':  c = '\r'; break;
                case '"':  c = '"';  break;
                case '\\': c = '\\'; break;
                default:   c = esc;  break;
            }
            if (i < cap - 1) buf[i++] = c;
        } else {
            if (i < cap - 1) buf[i++] = *p++;
            else p++;
        }
    }
    if (*p != quote) return NULL;   /* unterminated string */
    buf[i] = '\0';
    return p + 1;   /* past closing quote */
}

/* Parse a bare key (alphanumeric + _ + -) into buf[cap].
 * Returns pointer past key. */
static const char *parse_bare_key(const char *p, char *buf, size_t cap) {
    size_t i = 0;
    while ((*p >= 'a' && *p <= 'z') || (*p >= 'A' && *p <= 'Z') ||
           (*p >= '0' && *p <= '9') || *p == '_' || *p == '-') {
        if (i < cap - 1) buf[i++] = *p;
        p++;
    }
    buf[i] = '\0';
    return p;
}

/* ── Public API ───────────────────────────────────────────────────────── */

bool toml_parse(TomlDoc *doc, const char *source) {
    memset(doc, 0, sizeof(*doc));
    char cur_section[TOML_MAX_SECTION] = "";

    const char *p = source;
    int line_num  = 1;

    while (*p) {
        p = skip_ws(p);

        /* Empty line */
        if (*p == '\n') { p++; line_num++; continue; }
        if (*p == '\r') { p++; continue; }

        /* End of input */
        if (!*p) break;

        /* Comment */
        if (*p == '#') { p = skip_line(p); line_num++; continue; }

        /* Section header: [section.name] or [section] */
        if (*p == '[') {
            p++;
            /* Skip double bracket [[...]] — not supported, skip whole line */
            if (*p == '[') { p = skip_line(p); line_num++; continue; }

            size_t i = 0;
            while (*p && *p != ']' && *p != '\n') {
                if (i < TOML_MAX_SECTION - 1)
                    cur_section[i++] = *p;
                p++;
            }
            cur_section[i] = '\0';
            /* Trim trailing whitespace */
            while (i > 0 && (cur_section[i-1] == ' ' || cur_section[i-1] == '\t'))
                cur_section[--i] = '\0';

            if (*p != ']') {
                snprintf(doc->error, sizeof(doc->error),
                         "line %d: unterminated section header", line_num);
                return false;
            }
            p++;    /* past ] */
            p = skip_line(p); line_num++;
            continue;
        }

        /* Key = value line */
        /* Key may be bare or quoted */
        char key[TOML_MAX_KEY] = {0};
        if (*p == '"' || *p == '\'') {
            p = parse_quoted(p, key, sizeof(key));
            if (!p) {
                snprintf(doc->error, sizeof(doc->error),
                         "line %d: invalid quoted key", line_num);
                return false;
            }
        } else if ((*p >= 'a' && *p <= 'z') || (*p >= 'A' && *p <= 'Z') ||
                   (*p >= '0' && *p <= '9') || *p == '_' || *p == '-') {
            p = parse_bare_key(p, key, sizeof(key));
        } else {
            /* Unrecognised line — skip */
            p = skip_line(p); line_num++;
            continue;
        }

        p = skip_ws(p);
        if (*p != '=') {
            /* Not a key=value line (e.g. dotted key continuation) — skip */
            p = skip_line(p); line_num++;
            continue;
        }
        p++;    /* past = */
        p = skip_ws(p);

        /* Value must be a quoted string for our use case */
        if (*p != '"' && *p != '\'') {
            /* Non-string value (integer, boolean, array, etc.) — skip */
            p = skip_line(p); line_num++;
            continue;
        }

        char value[TOML_MAX_VALUE] = {0};
        p = parse_quoted(p, value, sizeof(value));
        if (!p) {
            snprintf(doc->error, sizeof(doc->error),
                     "line %d: unterminated string value", line_num);
            return false;
        }

        /* Store entry if we have room */
        if (doc->count < TOML_MAX_ENTRIES) {
            TomlEntry *e = &doc->entries[doc->count++];
            snprintf(e->section, sizeof(e->section), "%s", cur_section);
            snprintf(e->key,     sizeof(e->key),     "%s", key);
            snprintf(e->value,   sizeof(e->value),   "%s", value);
        }

        p = skip_ws(p);
        /* Allow inline comment after value */
        if (*p == '#') { p = skip_line(p); line_num++; continue; }
        if (*p == '\n') { p++; line_num++; }
        else if (*p == '\r') { p++; }
        continue;
    }

    return true;
}

const char *toml_get(const TomlDoc *doc,
                     const char *section, const char *key) {
    for (int i = 0; i < doc->count; i++) {
        const TomlEntry *e = &doc->entries[i];
        if (strcmp(e->section, section) == 0 &&
            strcmp(e->key,     key)     == 0)
            return e->value;
    }
    return NULL;
}

void toml_free(TomlDoc *doc) {
    (void)doc;  /* all storage is static inside TomlDoc */
}
