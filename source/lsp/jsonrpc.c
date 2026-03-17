/*
 * jsonrpc.c — LSP JSON-RPC message framing
 */

#define _POSIX_C_SOURCE 200809L
#include "jsonrpc.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Read one line from stdin into buf (up to bufsz-1 chars), stripping \r\n.
 * Returns false on EOF or error. */
static bool read_line(char *buf, size_t bufsz) {
    size_t i = 0;
    int c;
    while ((c = getchar()) != EOF) {
        if (c == '\n') break;
        if (c == '\r') continue;
        if (i < bufsz - 1) buf[i++] = (char)c;
    }
    buf[i] = '\0';
    return c != EOF || i > 0;
}

char *jsonrpc_read(void) {
    size_t content_length = 0;
    bool   got_length     = false;

    /* Read headers until blank line */
    for (;;) {
        char line[256];
        if (!read_line(line, sizeof(line))) return NULL; /* EOF */

        if (line[0] == '\0') break; /* blank line = end of headers */

        /* Parse Content-Length */
        if (strncmp(line, "Content-Length:", 15) == 0 ||
            strncmp(line, "content-length:", 15) == 0) {
            const char *p = line + 15;
            while (*p == ' ' || *p == '\t') p++;
            content_length = (size_t)strtoul(p, NULL, 10);
            got_length = true;
        }
        /* Ignore all other headers (Content-Type etc.) */
    }

    if (!got_length || content_length == 0) return NULL;

    char *buf = malloc(content_length + 1);
    if (!buf) return NULL;

    size_t nr = fread(buf, 1, content_length, stdin);
    buf[nr] = '\0';

    if (nr != content_length) {
        /* Partial read — still return what we got, parser will fail gracefully */
    }

    return buf;
}

void jsonrpc_write(const char *json, size_t len) {
    /* Write to stdout — use fprintf for the header, fwrite for the body */
    fprintf(stdout, "Content-Length: %zu\r\n\r\n", len);
    fwrite(json, 1, len, stdout);
    fflush(stdout);
}

void jsonrpc_write_str(const char *json) {
    jsonrpc_write(json, strlen(json));
}
