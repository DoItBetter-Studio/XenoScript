/*
 * token.h — Token types and the Token struct
 *
 * This file is the shared vocabulary of the entire compiler pipeline.
 * Every stage (lexer, parser, type checker, compiler) speaks in terms
 * of these token types, so we define them once here.
 */

#ifndef TOKEN_H
#define TOKEN_H

#include <stddef.h>  /* size_t */

/* ─────────────────────────────────────────────────────────────────────────────
 * TOKEN TYPES
 *
 * Grouped by category for readability. The grouping has no effect at runtime —
 * it's purely for the human reading this file.
 * ───────────────────────────────────────────────────────────────────────────*/
typedef enum {

    /* ── Literals ──────────────────────────────────────────────────────────
     * These are tokens whose VALUE matters, not just their kind.
     * e.g. the integer literal 42, the string "hello", etc.
     * The actual value text is stored in Token.start / Token.length. */
    TOK_INT_LIT,      /* e.g.  42          */
    TOK_FLOAT_LIT,    /* e.g.  3.14        */
    TOK_STRING_LIT,   /* e.g.  "hello"     */
    TOK_CHAR_LIT,     /* e.g.  'A'         */
    TOK_TRUE,         /* true  (keyword, but also a literal value) */
    TOK_FALSE,        /* false             */

    /* ── Interpolated string tokens ─────────────────────────────────────────
     * $"Hello, {name}!" is lexed as a stream of alternating text/expr tokens.
     *   TOK_INTERP_BEGIN  — the $" opener; start collecting segments
     *   TOK_INTERP_TEXT   — a literal text run between { } or at start/end
     *   TOK_INTERP_LBRACE — the { that opens an embedded expression
     *   TOK_INTERP_RBRACE — the } that closes an embedded expression
     *   TOK_INTERP_END    — the closing "
     * The lexer switches into a special mode after seeing $". */
    TOK_INTERP_BEGIN,   /* $"   */
    TOK_INTERP_TEXT,    /* literal text segment inside $"..." */
    TOK_INTERP_LBRACE,  /* {  inside an interpolated string  */
    TOK_INTERP_RBRACE,  /* }  inside an interpolated string  */
    TOK_INTERP_END,     /* closing "                         */

    /* ── Identifiers ───────────────────────────────────────────────────────
     * Any user-defined name: variable names, function names, etc. */
    TOK_IDENT,        /* e.g.  myVar, foo, player_health */

    /* ── Type keywords ─────────────────────────────────────────────────────
     * Because the language is STATICALLY TYPED these are reserved words.
     * A modder writes:   int x = 5;
     * Not:               var x = 5;
     * The type is part of the syntax, not inferred at runtime. */
    TOK_INT,          /* int    */
    TOK_FLOAT,        /* float  */
    TOK_BOOL,         /* bool   */
    TOK_STRING,       /* string */
    TOK_VOID,         /* void   */
    /* Numeric type keywords */
    TOK_SBYTE,        /* sbyte  - int8_t            */
    TOK_BYTE,         /* byte   - uint8_t           */
    TOK_SHORT,        /* short  - int16_t           */
    TOK_USHORT,       /* ushort - uint16_t          */
    TOK_UINT,         /* uint   - uint32_t          */
    TOK_LONG,         /* long   - int64_t (alias)   */
    TOK_ULONG,        /* ulong  - uint64_t          */
    TOK_DOUBLE,       /* double - float alias       */
    TOK_CHAR,         /* char   - Unicode codepoint */

    /* ── Control flow keywords ─────────────────────────────────────────── */
    TOK_IF,           /* if     */
    TOK_ELSE,         /* else   */
    TOK_WHILE,        /* while  */
    TOK_FOR,          /* for      */
    TOK_FOREACH,      /* foreach  */
    TOK_IN,           /* in       */
    TOK_IS,           /* is       */
    TOK_AS,           /* as       */
    TOK_TYPEOF,       /* typeof   */
    TOK_RETURN,       /* return   */
    TOK_BREAK,        /* break    */
    TOK_CONTINUE,     /* continue */
    TOK_MATCH,        /* match    */
    TOK_CASE,         /* case     */
    TOK_DEFAULT,      /* default  */

    /* ── Declaration keywords ──────────────────────────────────────────── */
    TOK_FN,           /* function  — function declaration */
    TOK_CLASS,        /* class     — class declaration    */
    TOK_ENUM,         /* enum      — enum declaration     */
    TOK_AT,           /* @         — annotation prefix    */
    TOK_NEW,          /* new       — object instantiation */
    TOK_THIS,         /* this      — current instance     */
    TOK_SUPER,        /* super     — parent class access  */
    TOK_PUBLIC,       /* public    — access modifier      */
    TOK_PRIVATE,      /* private   — access modifier      */
    TOK_PROTECTED,    /* protected — access modifier      */
    TOK_STATIC,       /* static    — static member        */
    TOK_FINAL,        /* final     — immutable field      */
    TOK_VIRTUAL,      /* virtual   — overridable method   */
    TOK_OVERRIDE,     /* override  — overrides virtual    */
    TOK_EVENT,        /* event     — event declaration    */
    TOK_INTERFACE,    /* interface — interface declaration */
    TOK_WHERE,        /* where     — generic type constraint */
    TOK_IMPORT,       /* import    — import declaration      */

    /* ── Arithmetic operators ──────────────────────────────────────────── */
    TOK_PLUS,         /* +  */
    TOK_MINUS,        /* -  */
    TOK_STAR,         /* *  */
    TOK_PLUS_PLUS,    /* ++ */
    TOK_MINUS_MINUS,  /* -- */
    TOK_PLUS_ASSIGN,  /* += */
    TOK_MINUS_ASSIGN, /* -= */
    TOK_SLASH,        /* /  */
    TOK_PERCENT,      /* %  */

    /* ── Comparison operators ──────────────────────────────────────────── */
    TOK_EQ,           /* == */
    TOK_NEQ,          /* != */
    TOK_LT,           /* <  */
    TOK_LTE,          /* <= */
    TOK_GT,           /* >  */
    TOK_GTE,          /* >= */

    /* ── Assignment ────────────────────────────────────────────────────── */
    TOK_ASSIGN,       /* =  */

    /* ── Logical operators ─────────────────────────────────────────────── */
    TOK_AND,          /* && */
    TOK_OR,           /* || */
    TOK_BANG,         /* !  */

    /* ── Nullable operators ──────────────────────────────────────────────── */
    TOK_QUESTION,         /* ?   — nullable type suffix: string?         */
    TOK_QUESTION_DOT,     /* ?.  — null-safe member access: x?.name      */
    TOK_QUESTION_QUESTION,/* ??  — null coalescing: x ?? defaultVal      */

    /* ── Keywords ───────────────────────────────────────────────────────── */
    TOK_NULL,         /* null — the null literal                         */
    TOK_THROW,        /* throw                                           */
    TOK_TRY,          /* try                                             */
    TOK_CATCH,        /* catch                                           */
    TOK_FINALLY,      /* finally                                         */

    /* ── Delimiters ─────────────────────────────────────────────────────── */
    TOK_LPAREN,       /* (  */
    TOK_RPAREN,       /* )  */
    TOK_LBRACE,       /* {  */
    TOK_RBRACE,       /* }  */
    TOK_LBRACKET,     /* [  */
    TOK_RBRACKET,     /* ]  */
    TOK_COMMA,        /* ,  */
    TOK_SEMICOLON,    /* ;  */
    TOK_COLON,        /* :  */
    TOK_DOT,          /* .  — member access              */

    /* ── Special ─────────────────────────────────────────────────────────── */
    TOK_EOF,          /* End of source — signals the parser to stop       */
    TOK_ERROR         /* Malformed token — carries an error message in
                       * the lexeme field so the caller can report it     */

} TokenType;


/* ─────────────────────────────────────────────────────────────────────────────
 * TOKEN STRUCT
 *
 * We store tokens as a (pointer, length) pair into the ORIGINAL source string
 * rather than allocating new strings for each token. This is a key performance
 * and simplicity win: zero allocations in the lexer.
 *
 * The tradeoff is that the source string must stay alive as long as any tokens
 * derived from it are in use — which is the entire compilation lifetime, so
 * that's fine.
 * ───────────────────────────────────────────────────────────────────────────*/
typedef struct {
    TokenType   type;    /* What kind of token this is                      */
    const char *start;   /* Pointer into the source string where this began */
    int         length;  /* How many characters long this token is          */
    int         line;    /* Source line number, 1-based, for error messages  */
    int         col;     /* Source column number, 1-based, for LSP          */
} Token;


/* ─────────────────────────────────────────────────────────────────────────────
 * UTILITY
 * ───────────────────────────────────────────────────────────────────────────*/

/* Returns a static human-readable name for a token type.
 * Useful for error messages: "expected ';', got 'int'" */
const char *token_type_name(TokenType type);

#endif /* TOKEN_H */