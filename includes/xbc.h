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
#define XBC_VERSION  17

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

#endif /* XBC_H */