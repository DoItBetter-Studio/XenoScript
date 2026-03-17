/*
 * jsonrpc.h — LSP JSON-RPC message framing (Content-Length over stdio)
 *
 * The LSP wire protocol is:
 *   Content-Length: <N>\r\n
 *   \r\n
 *   <N bytes of UTF-8 JSON>
 *
 * We read from stdin, write to stdout. Both are set to binary mode so
 * \r\n is not mangled on Windows.
 */

#ifndef JSONRPC_H
#define JSONRPC_H

#include <stddef.h>
#include <stdbool.h>

/*
 * Read one JSON-RPC message from stdin.
 * Returns a heap-allocated null-terminated JSON string the caller must free(),
 * or NULL on EOF or parse error.
 */
char *jsonrpc_read(void);

/*
 * Write one JSON-RPC message to stdout.
 * Writes the Content-Length header and the body.
 */
void jsonrpc_write(const char *json, size_t len);

/* Convenience — write a null-terminated string */
void jsonrpc_write_str(const char *json);

#endif /* JSONRPC_H */
