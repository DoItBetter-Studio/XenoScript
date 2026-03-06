/*
 * toml.h — Minimal TOML parser for xeno.project files.
 *
 * Only handles the subset used by xeno.project:
 *   - Section headers:  [mod]  [dependencies]
 *   - String values:    key = "value"
 *   - No arrays, no inline tables, no multi-line strings
 *
 * Usage:
 *   TomlDoc doc;
 *   if (!toml_parse(&doc, source)) { ... error ... }
 *   const char *id = toml_get(&doc, "mod", "id");    // NULL if missing
 *   toml_free(&doc);
 */

#ifndef XENO_TOML_H
#define XENO_TOML_H

#include <stdbool.h>

#define TOML_MAX_ENTRIES  128
#define TOML_MAX_KEY      64
#define TOML_MAX_SECTION  64
#define TOML_MAX_VALUE    256

typedef struct {
    char section[TOML_MAX_SECTION];
    char key    [TOML_MAX_KEY];
    char value  [TOML_MAX_VALUE];
} TomlEntry;

typedef struct {
    TomlEntry entries[TOML_MAX_ENTRIES];
    int       count;
    char      error[128];   /* set on parse failure */
} TomlDoc;

/*
 * Parse TOML source into doc. Returns true on success.
 * On failure, doc->error contains a description.
 */
bool toml_parse(TomlDoc *doc, const char *source);

/*
 * Look up a value by section + key. Returns NULL if not found.
 * Section may be "" for keys before the first section header.
 */
const char *toml_get(const TomlDoc *doc,
                     const char *section, const char *key);

/* Free any heap memory inside a TomlDoc (currently a no-op, all static). */
void toml_free(TomlDoc *doc);

#endif /* XENO_TOML_H */
