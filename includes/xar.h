/*
 * xar.h — XenoScript Archive (.xar) format
 *
 * A .xar file is a named package of compiled XenoScript bytecode.
 * It is the unit of distribution for both the standard library and
 * user-authored mods/libraries.
 *
 * BINARY FORMAT:
 *
 *   [4]  magic        'X','A','R','1'
 *   [4]  manifest_len (uint32_t, big-endian) — byte length of manifest JSON
 *   [N]  manifest     UTF-8 JSON, manifest_len bytes (no null terminator)
 *   [4]  chunk_count  (uint32_t, big-endian) — number of .xbc blobs
 *   Per chunk:
 *     [4]  name_len   (uint32_t, big-endian)
 *     [N]  name       chunk name (e.g. "math", "collections.list"), no null
 *     [4]  data_len   (uint32_t, big-endian)
 *     [N]  data       raw .xbc bytes (as produced by xbc_write_mem)
 *
 * MANIFEST JSON fields:
 *   "name"         string  — package name, e.g. "collections"
 *   "version"      string  — semver string, e.g. "1.0.0"
 *   "exports"      array   — exported symbol names (functions, classes)
 *   "dependencies" array   — names of other .xar packages required
 *   "author"       string  — optional
 *   "description"  string  — optional
 *
 * EMBEDDING via objcopy:
 *   objcopy --input-target binary --output-target elf64-x86-64 \
 *           --binary-architecture i386:x86-64 \
 *           foo.xar foo.xar.o
 *
 *   Declares symbols (use __asm() for mingw underscore compat):
 *     extern const uint8_t xar_foo_start[] __asm("_binary_foo_xar_start");
 *     extern const uint8_t xar_foo_end[]   __asm("_binary_foo_xar_end");
 */

#ifndef XAR_H
#define XAR_H

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

#define XAR_MAGIC        "XAR1"
#define XAR_MAGIC_LEN    4
#define XAR_MAX_CHUNKS   256
#define XAR_MAX_NAME     256
#define XAR_MAX_EXPORTS  1024
#define XAR_MAX_DEPS     64

/* ── Result codes ─────────────────────────────────────────────────────── */

typedef enum {
    XAR_OK,
    XAR_ERR_IO,          /* File read/write failed            */
    XAR_ERR_BAD_MAGIC,   /* Not a valid .xar file             */
    XAR_ERR_CORRUPT,     /* Truncated or malformed data       */
    XAR_ERR_OOM,         /* Allocation failure                */
    XAR_ERR_TOO_MANY,    /* Exceeded chunk/export limits      */
} XarResult;

const char *xar_result_str(XarResult r);

/* ── Manifest ─────────────────────────────────────────────────────────── */

typedef struct {
    char  name[XAR_MAX_NAME];
    char  version[64];
    char  author[128];
    char  description[256];
    char  exports[XAR_MAX_EXPORTS][XAR_MAX_NAME];
    int   export_count;
    char  dependencies[XAR_MAX_DEPS][XAR_MAX_NAME];
    int   dep_count;
} XarManifest;

/* ── In-memory chunk ──────────────────────────────────────────────────── */

typedef struct {
    char     name[XAR_MAX_NAME];  /* chunk name within the archive */
    uint8_t *data;                /* heap-allocated .xbc bytes     */
    size_t   size;
} XarChunk;

/* ── Archive (read result) ────────────────────────────────────────────── */

typedef struct {
    XarManifest  manifest;
    XarChunk     chunks[XAR_MAX_CHUNKS];
    int          chunk_count;
} XarArchive;

/* Free all heap memory inside a XarArchive (not the struct itself). */
void xar_archive_free(XarArchive *ar);

/* ── Write API ────────────────────────────────────────────────────────── */

/*
 * Write a .xar to a file.
 * manifest  — package metadata
 * chunks    — array of XarChunk (data/size point to .xbc bytes)
 * n_chunks  — number of chunks
 */
XarResult xar_write(const char *path,
                    const XarManifest *manifest,
                    const XarChunk *chunks, int n_chunks);

/* Write to an in-memory buffer (caller must free *out). */
XarResult xar_write_mem(uint8_t **out, size_t *out_size,
                         const XarManifest *manifest,
                         const XarChunk *chunks, int n_chunks);

/* ── Read API ─────────────────────────────────────────────────────────── */

/* Read a .xar file into a XarArchive. Caller must call xar_archive_free(). */
XarResult xar_read(XarArchive *ar, const char *path);

/* Read from an in-memory buffer (e.g. objcopy-embedded blob). */
XarResult xar_read_mem(XarArchive *ar, const uint8_t *buf, size_t size);

/* ── Manifest JSON helpers ────────────────────────────────────────────── */

/* Serialise manifest to a heap-allocated JSON string. Caller must free(). */
char *xar_manifest_to_json(const XarManifest *m);

/* Parse JSON into a manifest. Returns false on malformed input. */
bool  xar_manifest_from_json(XarManifest *m, const char *json, size_t len);

#endif /* XAR_H */
