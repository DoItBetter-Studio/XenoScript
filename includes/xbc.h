/*
 * xbc.h — XenoScript Bytecode (.xbc) serialization
 *
 * Provides write and read functions for compiled Modules.
 * The .xbc format is a compact binary representation of a Module —
 * flat, portable, position-independent.
 *
 * FORMAT OVERVIEW:
 *
 *   Header (8 bytes):
 *     [4]  magic    "XBC\0"
 *     [1]  version  format version (currently 1)
 *     [2]  fn_count number of functions (uint16_t, big-endian)
 *     [1]  reserved padding
 *
 *   Per function:
 *     [1]  name_len
 *     [N]  name     (name_len bytes, no null terminator in file)
 *     [1]  param_count
 *     [1]  local_count
 *     [2]  const_count  (uint16_t)
 *     Per constant:
 *       [1]  kind    0=int 1=float 2=bool 3=string
 *       int/float/bool: [8] raw bytes (int64_t or double, big-endian)
 *       string:         [4] length (uint32_t) + [N] chars (no null)
 *     [4]  code_len (uint32_t)
 *     [N]  code     (raw bytecode bytes)
 *     line table (v23+, RLE):
 *       [4]  run_count (uint32_t)
 *       per run:
 *         [4]  length (uint32_t) — consecutive bytes sharing this line
 *         [4]  line   (uint32_t) — 1-based source line (0 = unknown)
 *       sum of run lengths must equal code_len
 *     source file (v24+):
 *       [1]  path_len + [N] basename (empty string if unknown)
 *
 * All multi-byte integers are big-endian for portability.
 * Strings in the constant pool are stored with explicit length — no
 * null terminators in the file.
 */

#ifndef XBC_H
#define XBC_H

#include "compiler.h"   /* Module, Chunk */
#include <stdint.h>
#include <stdbool.h>
#include <stdio.h>

#define XBC_MAGIC    "XBC\0"
#define XBC_VERSION  24

typedef enum {
    XBC_OK,
    XBC_ERR_IO,          /* File read/write failed              */
    XBC_ERR_BAD_MAGIC,   /* Not a valid .xbc file               */
    XBC_ERR_BAD_VERSION, /* Version mismatch                    */
    XBC_ERR_CORRUPT,     /* Data looks corrupted                */
    XBC_ERR_OOM,         /* Out of memory during load           */
} XbcResult;

const char *xbc_result_str(XbcResult r);

/*
 * Write a compiled Module to a .xbc file.
 * Returns XBC_OK on success.
 */
XbcResult xbc_write(const Module *module, const char *path);

/*
 * Read a .xbc file into a Module.
 * The module must be uninitialized — xbc_read calls module_init internally.
 * The caller is responsible for calling module_free() when done.
 * Returns XBC_OK on success.
 */
XbcResult xbc_read(Module *module, const char *path);

/*
 * Write to / read from an in-memory buffer instead of a file.
 * Useful for embedding bytecode directly in your binary (ROM-style).
 *
 * xbc_write_mem: writes into *buf (caller must free), sets *size.
 * xbc_read_mem:  reads from buf of given size into module.
 */
XbcResult xbc_write_mem(const Module *module, uint8_t **buf, size_t *size);
XbcResult xbc_read_mem(Module *module, const uint8_t *buf, size_t size);

/*
 * Verification mode.
 *   PRE_MERGE  — single .xbc / chunk load. Hollow stubs (empty code,
 *                fn_index == -1, field_count == 0) are allowed.
 *   POST_MERGE — after module_merge has filled shells and remapped indices.
 *                Strict: no unresolved methods, name operands must resolve,
 *                OP_CALL argc must match callee param_count, static field
 *                indices must be in range with no hollow escape.
 */
typedef enum {
    XBC_VERIFY_PRE_MERGE  = 0,
    XBC_VERIFY_POST_MERGE = 1,
} XbcVerifyMode;

/* Optional host-function table for OP_CALL_HOST checks (NULL to skip). */
typedef struct {
    int        host_fn_count;
    const int *param_counts; /* length host_fn_count; -1 = variadic; may be NULL */
} XbcVerifyHostCtx;

/*
 * Structural + instruction-boundary verification of an already-deserialized
 * Module. xbc_read / xbc_read_mem call PRE_MERGE. VM post-merge sites call
 * POST_MERGE, optionally with host context for OP_CALL_HOST checks.
 */
XbcResult xbc_verify_module(const Module *module, XbcVerifyMode mode);
XbcResult xbc_verify_module_ex(const Module *module, XbcVerifyMode mode,
                               const XbcVerifyHostCtx *host);

#endif /* XBC_H */