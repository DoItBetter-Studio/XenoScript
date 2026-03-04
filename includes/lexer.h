/*
 * lexer.h — The Lexer interface
 *
 * The lexer (also called a "scanner" or "tokenizer") takes a raw source
 * string and produces tokens one at a time on demand.
 *
 * We use a LAZY / PULL model: the parser asks for the next token by calling
 * lexer_next_token(), rather than the lexer producing all tokens upfront into
 * an array. This means:
 *   - Zero upfront allocation
 *   - The lexer and parser run in lockstep
 *   - Easy to report errors at the exact position they occur
 *
 * The Lexer struct holds all state needed to scan through the source.
 * It's meant to live on the stack — no heap allocation needed.
 */

#ifndef LEXER_H
#define LEXER_H

#include "token.h"
#include <stdbool.h>

/* ─────────────────────────────────────────────────────────────────────────────
 * LEXER STATE
 *
 * start   — points to the beginning of the CURRENT token being scanned
 * current — points to the character we're about to look at (one ahead of start
 *            once we begin scanning a token)
 * line    — tracks the current line for error reporting
 * ───────────────────────────────────────────────────────────────────────────*/
typedef struct {
    const char *start;    /* Start of the current token being scanned */
    const char *current;  /* Next character to be consumed            */
    int         line;     /* Current line number (1-based)            */

    /* ── Interpolated string state ──────────────────────────────────────
     * interp_depth > 0 means we are inside a $"..." string.
     * We use a small stack to support future nesting if needed.
     *   interp_depth   — how many $"..." levels deep we are (0 = normal)
     *   brace_depth    — brace nesting inside the current {} expression,
     *                    so that {foo()} doesn't end early on the inner }
     *   after_lbrace   — true right after we emitted TOK_INTERP_LBRACE,
     *                    meaning the next call should scan normal tokens
     *                    until we see the matching } */
    int  interp_depth;    /* 0 = normal mode; >0 = inside $"..."      */
    int  brace_depth;     /* depth of { inside current expression     */
    bool in_expr;         /* true: scanning expression tokens; false: text */
    bool had_error;
    const char *error_message;
} Lexer;


/* Initialize a lexer with a null-terminated source string.
 * The source must remain valid for the lifetime of the lexer. */
void lexer_init(Lexer *lexer, const char *source);

/* Scan and return the next token from the source.
 * When the source is exhausted, returns a token of type TOK_EOF.
 * On an unrecognized character, returns TOK_ERROR with the bad character
 * as the lexeme — the caller decides how to handle it. */
Token lexer_next_token(Lexer *lexer);

#endif /* LEXER_H */