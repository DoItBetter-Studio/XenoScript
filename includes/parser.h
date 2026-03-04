/*
 * parser.h — Parser interface
 *
 * The parser consumes tokens from a Lexer and produces an AST Program.
 * It owns an Arena for all node allocations — the caller gets back a Program
 * whose nodes all live in that arena.
 *
 * Error handling:
 *   The parser tracks errors in an internal error list. It does NOT crash or
 *   longjmp on a bad token. Instead it records the error, synchronizes to a
 *   safe position, and continues parsing. This means a single parse pass can
 *   report multiple errors.
 *
 *   After parse() returns, check parser.had_error. If true, the returned
 *   Program is incomplete and should NOT be passed to the type checker.
 */

#ifndef PARSER_H
#define PARSER_H

#include "lexer.h"
#include "ast.h"
#include "arena.h"

#define PARSER_MAX_ERRORS 32   /* Stop accumulating errors after this many */

typedef struct {
    char message[256];
    int  line;
} ParseError;

typedef struct {
    /* Input */
    Lexer  *lexer;

    /* Lookahead — we keep the current, next, and peek token buffered.
     * current is what we're looking at, next is one token ahead,
     * peek is two tokens ahead. The 3-token lookahead is
     * needed to disambiguate generic type args from comparison operators:
     * foo<Bar>(x) vs a < b  — we check that after the type name comes > or , */
    Token   current;
    Token   next;
    Token   peek;
    Token   previous;

    /* Memory — all AST nodes come from here */
    Arena   arena;

    /* Error tracking */
    ParseError errors[PARSER_MAX_ERRORS];
    int        error_count;
    bool       had_error;

    /* Panic mode flag — set when we hit an error, cleared after synchronizing.
     * While in panic mode we suppress cascading errors. */
    bool       panic_mode;
} Parser;


/* Initialize the parser. Initializes the arena internally.
 * The lexer must already be initialized and pointing at the source. */
void parser_init(Parser *parser, Lexer *lexer);

/* Parse the entire source into a Program.
 * Returns the program (possibly incomplete if had_error is true).
 * The program's nodes live in parser->arena — free it with parser_free()
 * when you're done with the AST. */
Program parser_parse(Parser *parser);

/* Free the arena. Call this after you're done with the AST
 * (i.e. after the compiler has consumed it). */
void parser_free(Parser *parser);

/* Print all parse errors to stderr */
void parser_print_errors(const Parser *parser);

#endif /* PARSER_H */