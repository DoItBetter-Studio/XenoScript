/*
 * xdbg.h — XenoScript debug symbol sidecars (.xdbg)
 *
 * Optional companion files for .xbc / .xar packages. Production packages do
 * not embed local names; when compiling, xenoc writes a sibling .xdbg that
 * xenovm can load under --debug for named locals in the interactive debugger.
 *
 * Format (little-endian integers, length-prefixed strings):
 *   magic      "XDBG" (4 bytes)
 *   version    u8 (= 1)
 *   chunk_count u32
 *   for each chunk:
 *     name         string
 *     source_file  string
 *     local_count  u16
 *     for each local slot 0..local_count-1:
 *       name       string  (empty if unknown)
 */

#ifndef XENO_XDBG_H
#define XENO_XDBG_H

#include "bytecode.h"
#include "compiler.h"
#include <stdbool.h>
#include <stddef.h>

#define XDBG_VERSION 2
#define XDBG_MAGIC   "XDBG"

/* Write debug symbols for every chunk in `module` to `path`. */
bool xdbg_write(const Module *module, const char *path);

/* Load debug symbols from `path` into matching chunks of `module`
 * (matched by chunk name). Unknown chunks are ignored. */
bool xdbg_load(Module *module, const char *path);

/* Same as xdbg_load, but from an in-memory buffer (embedded stdlib sidecars). */
bool xdbg_load_mem(Module *module, const void *data, size_t size);

/* Derive a sibling .xdbg path from a .xbc or .xar path.
 * Writes into `out` (capacity `out_sz`). Returns false if path is empty. */
bool xdbg_path_from_artifact(const char *artifact_path, char *out, size_t out_sz);

/* Ensure chunk has a local_names table sized for at least `slot_count` slots. */
bool chunk_ensure_local_names(Chunk *chunk, int slot_count);

/* Record a local name for slot `slot` (no-op if names table missing). */
void chunk_set_local_name(Chunk *chunk, int slot, const char *name, int length);

/* Record name + TypeKind for a local slot. */
void chunk_set_local_info(Chunk *chunk, int slot, const char *name, int length,
                          int type_kind);

#endif /* XENO_XDBG_H */
