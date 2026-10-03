/*
 * xdoc.h — XenoScript Documentation Archive (.xdoc)
 *
 * Side-car to .xar: human-readable, IDE-only metadata extracted from
 * /\\* #Docs ... #!Docs *\\/ blocks. Never loaded by the VM.
 *
 * Format (text, version 1):
 *   XDOC1
 *   ENTRY <symbol>          // e.g. Dictionary, Dictionary.add
 *   title <text>
 *   summary <text>
 *   description <text>
 *   param <name> <text>     // name without leading $
 *   returns <text>
 *   throws <type> <text>
 *   example <text>
 *   see <text>
 *   link <text>
 *   since <text>
 *   deprecated <text>
 *   note <text>
 *   warning <text>
 *   important <text>
 *   remarks <text>
 *   END
 */

#ifndef XENO_XDOC_H
#define XENO_XDOC_H

#include <stddef.h>
#include <stdbool.h>

#define XDOC_MAX_ENTRIES   512
#define XDOC_MAX_PARAMS    16
#define XDOC_MAX_THROWS    8
#define XDOC_SYM_MAX       128
#define XDOC_TEXT_MAX      512
#define XDOC_NAME_MAX      64

typedef struct {
	char name[XDOC_NAME_MAX];
	char doc[XDOC_TEXT_MAX];
} XdocParam;

typedef struct {
	char type_name[XDOC_NAME_MAX];
	char doc[XDOC_TEXT_MAX];
} XdocThrow;

typedef struct {
	char symbol[XDOC_SYM_MAX]; /* "Class" or "Class.member" */

	char title[XDOC_TEXT_MAX];
	char summary[XDOC_TEXT_MAX];
	char description[XDOC_TEXT_MAX];
	char returns_doc[XDOC_TEXT_MAX];
	char example[XDOC_TEXT_MAX];
	char see[XDOC_TEXT_MAX];
	char link[XDOC_TEXT_MAX];
	char since[XDOC_TEXT_MAX];
	char deprecated[XDOC_TEXT_MAX];
	char note[XDOC_TEXT_MAX];
	char warning[XDOC_TEXT_MAX];
	char important[XDOC_TEXT_MAX];
	char remarks[XDOC_TEXT_MAX];

	XdocParam params[XDOC_MAX_PARAMS];
	int      param_count;
	XdocThrow throws[XDOC_MAX_THROWS];
	int      throw_count;
} XdocEntry;

typedef struct {
	XdocEntry *entries;
	int       count;
	int       capacity;
} XdocArchive;

/* Extract docs from source text. current_class may be NULL at top level.
 * On title mismatch vs following decl: prints warning, still binds.
 * Docs with no following declaration are dropped.
 * Returns number of entries added (appends to out). */
int xdoc_extract_from_source(const char *source, const char *filename,
							XdocArchive *out);

/* Load / save */
bool xdoc_load(XdocArchive *out, const char *path);
bool xdoc_load_mem(XdocArchive *out, const char *data, size_t size);
bool xdoc_save(const XdocArchive *ar, const char *path);
void xdoc_free(XdocArchive *ar);

/* Lookup: "Dictionary" or "Dictionary.add". Returns NULL if missing. */
const XdocEntry *xdoc_find(const XdocArchive *ar, const char *symbol);

/* Copy all entries from src into dst (replace same symbol, else append). */
void xdoc_merge(XdocArchive *dst, const XdocArchive *src);

/* Format entry as markdown for LSP hover (caller frees). */
char *xdoc_format_markdown(const XdocEntry *e);

#endif /* XENO_XDOC_H */