/*
 * lexer.c — Lexer implementation
 *
 * The lexer is a simple hand-written scanner. It's a state machine where
 * the "state" is just the current character we're looking at. No complex
 * DFA tables — just a switch statement and a few helper functions.
 *
 * Performance note: this is a single-pass, zero-allocation scanner. It never
 * copies characters. All tokens are (pointer, length) slices of the source.
 */

#include "lexer.h"

#include <string.h>  /* strncmp         */
#include <stdint.h>  /* uint32_t        */
#include <stdbool.h> /* bool, true, false */

/* ─────────────────────────────────────────────────────────────────────────────
 * INTERNAL HELPERS
 *
 * These are static (file-scoped) because nothing outside this file should
 * call them directly. The public interface is just lexer_init + lexer_next_token.
 * ───────────────────────────────────────────────────────────────────────────*/

/* Are we at the end of the source? */
static bool is_at_end(const Lexer *lexer)
{
    return *lexer->current == '\0';
}

/* Consume the current character and advance the pointer. Returns the char. */
static char advance(Lexer *lexer)
{
    return *lexer->current++;
}

/* Look at the current character WITHOUT consuming it. */
static char peek(const Lexer *lexer)
{
    return *lexer->current;
}

/* Look one character AHEAD of the current position without consuming.
 * This is our "lookahead 1" — needed for two-character tokens like == and !=.
 * We never need more than 1 lookahead character in this grammar. */
static char peek_next(const Lexer *lexer)
{
    if (is_at_end(lexer))
        return '\0';
    return lexer->current[1];
}

/* If the next character matches `expected`, consume it and return true.
 * Otherwise leave the position alone and return false.
 * This is how we handle == vs =, != vs !, <= vs <, >= vs >. */
static bool match(Lexer *lexer, char expected)
{
    if (is_at_end(lexer))
        return false;
    if (*lexer->current != expected)
        return false;
    lexer->current++;
    return true;
}

/* Build a token of the given type from the current lexer position.
 * The token's lexeme spans from lexer->start to lexer->current. */
static Token make_token(const Lexer *lexer, TokenType type)
{
    Token t;
    t.type = type;
    t.start = lexer->start;
    t.length = (int)(lexer->current - lexer->start);
    t.line = lexer->line;
    return t;
}

/* Build an error token. The "lexeme" of an error token is the error MESSAGE
 * (a string literal), not a slice of source. The length is the message length.
 * The caller can retrieve it with token.start[0..token.length]. */
static Token error_token(const Lexer *lexer, const char *message)
{
    Token t;
    t.type = TOK_ERROR;
    t.start = message;
    t.length = (int)strlen(message);
    t.line = lexer->line;
    return t;
}

/* Skip whitespace and comments.
 * We track newlines here so the line counter is always accurate. */
static void skip_whitespace_and_comments(Lexer *lexer)
{
    for (;;)
    {
        char c = peek(lexer);
        switch (c)
        {
        case ' ':
        case '\r':
        case '\t':
            advance(lexer);
            break;

        case '\n':
            /* Increment line BEFORE consuming so the newline itself
             * is attributed to the line it ends, not the next one. */
            lexer->line++;
            advance(lexer);
            break;

        case '/':
            /* Line comment: // ... until end of line
             * We check peek_next to avoid consuming a lone '/'. */
            if (peek_next(lexer) == '/')
            {
                /* Consume everything until newline or EOF.
                 * We DON'T consume the newline itself — the outer loop
                 * will catch it on the next iteration and increment the
                 * line counter correctly. */
                while (peek(lexer) != '\n' && !is_at_end(lexer))
                    advance(lexer);
            }
            else if (peek_next(lexer) == '*')
            {
                advance(lexer); /* consume '/' */
                advance(lexer); /* consume '*' */

                bool closed = false;

                while (!is_at_end(lexer))
                {
                    if (peek(lexer) == '\n')
                    {
                        lexer->line++;
                        advance(lexer);
                    }
                    else if (peek(lexer) == '*' && peek_next(lexer) == '/')
                    {
                        advance(lexer);
                        advance(lexer);
                        closed = true;
                        break;
                    }
                    else
                    {
                        advance(lexer);
                    }
                }
                if (!closed)
                {
                    lexer->had_error = true;
                    lexer->error_message = "Unterminated block comment (expected '*/' before EOF)";
                    return;
                }
            }
            else
            {
                /* It's a division operator, not a comment. Stop skipping. */
                return;
            }
            break;

        default:
            return;
        }
    }
}

/* ─────────────────────────────────────────────────────────────────────────────
 * SCANNING SPECIFIC TOKEN KINDS
 * ───────────────────────────────────────────────────────────────────────────*/

/* ── Interpolated string text segment scanner ──────────────────────────────
 * Called when inside a $"..." and NOT inside a {} expression.
 * Scans literal chars until { (expr start), " (string end), or error.
 * {{ -> literal {,  }} -> literal },  \" \n \t \\ supported.
 * Cooked bytes are stored in a static buffer (single-threaded, single-pass). */
#define INTERP_TEXT_BUF_SIZE 4096
static char interp_text_buf[INTERP_TEXT_BUF_SIZE];
static int interp_text_buf_pos = 0;

static Token scan_interp_text(Lexer *lexer)
{
    int buf_start = interp_text_buf_pos;

    while (!is_at_end(lexer))
    {
        char ch = peek(lexer);

        if (ch == '{')
        {
            if (lexer->current[1] == '{')
            {
                advance(lexer);
                advance(lexer);
                if (interp_text_buf_pos < INTERP_TEXT_BUF_SIZE - 1)
                    interp_text_buf[interp_text_buf_pos++] = '{';
            }
            else
            {
                break; /* real expression start */
            }
        }
        else if (ch == '}')
        {
            if (lexer->current[1] == '}')
            {
                advance(lexer);
                advance(lexer);
                if (interp_text_buf_pos < INTERP_TEXT_BUF_SIZE - 1)
                    interp_text_buf[interp_text_buf_pos++] = '}';
            }
            else
            {
                advance(lexer);
                return error_token(lexer,
                                   "Unexpected '}' in interpolated string -- use '}}' for a literal '}'.");
            }
        }
        else if (ch == '"')
        {
            break; /* end of interpolated string */
        }
        else if (ch == '\\')
        {
            advance(lexer);
            char esc = peek(lexer);
            advance(lexer);
            char out = esc;
            if (esc == 'n')
                out = '\n';
            else if (esc == 't')
                out = '\t';
            else if (esc == 'r')
                out = '\r';
            else if (esc == '"')
                out = '"';
            else if (esc == '\\')
                out = '\\';
            if (interp_text_buf_pos < INTERP_TEXT_BUF_SIZE - 1)
                interp_text_buf[interp_text_buf_pos++] = out;
        }
        else
        {
            if (ch == '\n')
                lexer->line++;
            advance(lexer);
            if (interp_text_buf_pos < INTERP_TEXT_BUF_SIZE - 1)
                interp_text_buf[interp_text_buf_pos++] = ch;
        }
    }

    Token t;
    t.type = TOK_INTERP_TEXT;
    t.start = interp_text_buf + buf_start;
    t.length = interp_text_buf_pos - buf_start;
    t.line = lexer->line;
    return t;
}

/* Scan a string literal.
 * Precondition: the opening '"' has already been consumed.
 * We scan until we find the closing '"', tracking newlines along the way.
 * We don't support escape sequences yet — that's a Phase 2 enhancement. */
static Token scan_string(Lexer *lexer)
{
    while (peek(lexer) != '"' && !is_at_end(lexer))
    {
        if (peek(lexer) == '\n')
            lexer->line++;
        advance(lexer);
    }

    if (is_at_end(lexer))
        return error_token(lexer, "Unterminated string literal.");

    advance(lexer); /* consume the closing '"' */

    /* The token's lexeme INCLUDES the surrounding quotes.
     * The compiler/interpreter will strip them when it needs the raw value. */
    return make_token(lexer, TOK_STRING_LIT);
}

/* Scan a numeric literal — either int or float.
 * Precondition: the first digit has already been consumed.
 * A '.' followed by a digit makes it a float literal. */
/* Scan a character literal: 'A', '\n', '\\', '\'' etc.
 * The lexeme includes the surrounding single quotes.
 * Stored as a TOK_CHAR_LIT; the compiler reads the codepoint from between
 * the quotes and emits it as a LOAD_CONST_INT. */
static Token scan_char(Lexer *lexer)
{
    /* lexer->start points to the opening quote which was already consumed
     * by the main dispatch — wait, actually advance() in dispatch hasn't run.
     * The opening quote is at lexer->start (set before the switch).
     * We need to consume the character and closing quote. */
    if (is_at_end(lexer))
        return error_token(lexer, "Unterminated character literal.");

    /* Handle escape sequences */
    uint32_t codepoint;
    if (peek(lexer) == '\\')
    {
        advance(lexer); /* consume backslash */
        char esc = peek(lexer);
        advance(lexer);
        switch (esc)
        {
        case 'n':
            codepoint = '\n';
            break;
        case 't':
            codepoint = '\t';
            break;
        case 'r':
            codepoint = '\r';
            break;
        case '0':
            codepoint = 0;
            break;
        case '\\':
            codepoint = '\\';
            break;
        case '\'':
            codepoint = '\'';
            break;
        default:
            return error_token(lexer, "Unknown escape sequence in char literal.");
        }
    }
    else
    {
        unsigned char c = (unsigned char)peek(lexer);
        codepoint = c;
        advance(lexer);
    }

    if (peek(lexer) != '\'')
        return error_token(lexer, "Character literal must contain exactly one character.");
    advance(lexer); /* consume closing quote */

    (void)codepoint; /* stored in token lexeme; compiler extracts it */
    return make_token(lexer, TOK_CHAR_LIT);
}

static Token scan_number(Lexer *lexer)
{
    /* Consume remaining integer digits */
    while (peek(lexer) >= '0' && peek(lexer) <= '9')
        advance(lexer);

    /* Check for a decimal point to determine if this is a float.
     * We peek at the character AFTER the '.' — if it's not a digit,
     * the '.' belongs to something else (e.g. a method call later on). */
    if (peek(lexer) == '.' && peek_next(lexer) >= '0' && peek_next(lexer) <= '9')
    {
        advance(lexer); /* consume the '.' */
        while (peek(lexer) >= '0' && peek(lexer) <= '9')
            advance(lexer);
        if (peek(lexer) == 'f') advance(lexer); /* optional f suffix: 3.14f */
        return make_token(lexer, TOK_FLOAT_LIT);
    }

    /* Optional f suffix on integer: 1f — treat as float */
    if (peek(lexer) == 'f') {
        advance(lexer);
        return make_token(lexer, TOK_FLOAT_LIT);
    }

    return make_token(lexer, TOK_INT_LIT);
}

/* Scan an identifier or keyword.
 * Precondition: the first character (letter or '_') has already been consumed.
 *
 * Strategy: scan the whole word first, then check if it's a keyword.
 * This is simpler than a trie/DFA and fast enough for any realistic keyword set. */
static Token scan_identifier_or_keyword(Lexer *lexer)
{
    /* Identifiers can contain letters, digits, and underscores after the first char. */
    while (peek(lexer) == '_' ||
           (peek(lexer) >= 'a' && peek(lexer) <= 'z') ||
           (peek(lexer) >= 'A' && peek(lexer) <= 'Z') ||
           (peek(lexer) >= '0' && peek(lexer) <= '9'))
    {
        advance(lexer);
    }

    /* Now compare the scanned word against our keyword table.
     * We use strncmp with the token length to avoid needing a null terminator. */
    int len = (int)(lexer->current - lexer->start);
    const char *word = lexer->start;

    /* Type keywords — these are the foundation of the static type system */
    if (len == 3 && strncmp(word, "int", 3) == 0)
        return make_token(lexer, TOK_INT);
    if (len == 5 && strncmp(word, "float", 5) == 0)
        return make_token(lexer, TOK_FLOAT);
    if (len == 4 && strncmp(word, "bool", 4) == 0)
        return make_token(lexer, TOK_BOOL);
    if (len == 6 && strncmp(word, "string", 6) == 0)
        return make_token(lexer, TOK_STRING);
    if (len == 4 && strncmp(word, "void", 4) == 0)
        return make_token(lexer, TOK_VOID);

    /* Numeric type keywords */
    if (len == 5 && strncmp(word, "sbyte", 5) == 0)
        return make_token(lexer, TOK_SBYTE);
    if (len == 4 && strncmp(word, "byte", 4) == 0)
        return make_token(lexer, TOK_BYTE);
    if (len == 5 && strncmp(word, "short", 5) == 0)
        return make_token(lexer, TOK_SHORT);
    if (len == 6 && strncmp(word, "ushort", 6) == 0)
        return make_token(lexer, TOK_USHORT);
    if (len == 4 && strncmp(word, "uint", 4) == 0)
        return make_token(lexer, TOK_UINT);
    if (len == 4 && strncmp(word, "long", 4) == 0)
        return make_token(lexer, TOK_LONG);
    if (len == 5 && strncmp(word, "ulong", 5) == 0)
        return make_token(lexer, TOK_ULONG);
    if (len == 6 && strncmp(word, "double", 6) == 0)
        return make_token(lexer, TOK_DOUBLE);
    if (len == 4 && strncmp(word, "char", 4) == 0)
        return make_token(lexer, TOK_CHAR);

    /* Boolean literals */
    if (len == 4 && strncmp(word, "true", 4) == 0)
        return make_token(lexer, TOK_TRUE);
    if (len == 5 && strncmp(word, "false", 5) == 0)
        return make_token(lexer, TOK_FALSE);

    /* Control flow */
    if (len == 2 && strncmp(word, "if", 2) == 0)
        return make_token(lexer, TOK_IF);
    if (len == 4 && strncmp(word, "else", 4) == 0)
        return make_token(lexer, TOK_ELSE);
    if (len == 5 && strncmp(word, "while", 5) == 0)
        return make_token(lexer, TOK_WHILE);
    if (len == 3 && strncmp(word, "for", 3) == 0)
        return make_token(lexer, TOK_FOR);
    if (len == 7 && strncmp(word, "foreach", 7) == 0)
        return make_token(lexer, TOK_FOREACH);
    if (len == 2 && strncmp(word, "in", 2) == 0)
        return make_token(lexer, TOK_IN);
    if (len == 2 && strncmp(word, "is", 2) == 0)
        return make_token(lexer, TOK_IS);
    if (len == 2 && strncmp(word, "as", 2) == 0)
        return make_token(lexer, TOK_AS);
    if (len == 6 && strncmp(word, "typeof", 6) == 0)
        return make_token(lexer, TOK_TYPEOF);
    if (len == 6 && strncmp(word, "return", 6) == 0)
        return make_token(lexer, TOK_RETURN);
    if (len == 5 && strncmp(word, "break", 5) == 0)
        return make_token(lexer, TOK_BREAK);
    if (len == 8 && strncmp(word, "continue", 8) == 0)
        return make_token(lexer, TOK_CONTINUE);
    if (len == 5 && strncmp(word, "match", 5) == 0)
        return make_token(lexer, TOK_MATCH);
    if (len == 4 && strncmp(word, "case", 4) == 0)
        return make_token(lexer, TOK_CASE);
    if (len == 7 && strncmp(word, "default", 7) == 0)
        return make_token(lexer, TOK_DEFAULT);

    /* Declaration */
    if (len == 8 && strncmp(word, "function", 8) == 0)
        return make_token(lexer, TOK_FN);
    if (len == 5 && strncmp(word, "class", 5) == 0)
        return make_token(lexer, TOK_CLASS);
    if (len == 4 && strncmp(word, "enum", 4) == 0)
        return make_token(lexer, TOK_ENUM);
    if (len == 3 && strncmp(word, "new", 3) == 0)
        return make_token(lexer, TOK_NEW);
    if (len == 4 && strncmp(word, "this", 4) == 0)
        return make_token(lexer, TOK_THIS);
    if (len == 5 && strncmp(word, "super", 5) == 0)
        return make_token(lexer, TOK_SUPER);

    /* Access modifiers */
    if (len == 6 && strncmp(word, "public", 6) == 0)
        return make_token(lexer, TOK_PUBLIC);
    if (len == 7 && strncmp(word, "private", 7) == 0)
        return make_token(lexer, TOK_PRIVATE);
    if (len == 9 && strncmp(word, "protected", 9) == 0)
        return make_token(lexer, TOK_PROTECTED);
    if (len == 6 && strncmp(word, "static", 6) == 0)
        return make_token(lexer, TOK_STATIC);

    /* Nothing matched — it's a user-defined identifier */
    return make_token(lexer, TOK_IDENT);
}

/* ─────────────────────────────────────────────────────────────────────────────
 * PUBLIC INTERFACE
 * ───────────────────────────────────────────────────────────────────────────*/

void lexer_init(Lexer *lexer, const char *source)
{
    lexer->start = source;
    lexer->current = source;
    lexer->line = 1;
    lexer->interp_depth = 0;
    lexer->brace_depth = 0;
    lexer->in_expr = false;
}

Token lexer_next_token(Lexer *lexer)
{
    if (lexer->had_error) {
        lexer->had_error = false;
        return error_token(lexer, lexer->error_message);
    }

    /* ── Interpolated string mode ────────────────────────────────────────
     * When interp_depth > 0 we are inside a $"..." string.
     *   in_expr=false  -> scan text segment / handle { or closing "
     *   in_expr=true   -> scan normal tokens until matching } */
    if (lexer->interp_depth > 0 && !lexer->in_expr)
    {
        lexer->start = lexer->current;

        if (is_at_end(lexer))
            return error_token(lexer, "Unterminated interpolated string.");

        char ahead = peek(lexer);

        if (ahead == '"')
        {
            /* Closing quote */
            advance(lexer);
            lexer->interp_depth--;
            return make_token(lexer, TOK_INTERP_END);
        }

        if (ahead == '{')
        {
            /* Only treat as expression start if it's a single {.
             * {{ is an escaped brace → handled by scan_interp_text. */
            if (lexer->current[1] != '{')
            {
                /* Start of embedded expression */
                advance(lexer);
                lexer->in_expr = true;
                lexer->brace_depth = 0;
                return make_token(lexer, TOK_INTERP_LBRACE);
            }
            /* Fall through to scan_interp_text which handles {{ */
        }

        /* Otherwise scan a text segment */
        return scan_interp_text(lexer);
    }

    /* Skip any whitespace or comments before the next real token */
    skip_whitespace_and_comments(lexer);

    /* Mark the start of this token AFTER skipping whitespace.
     * This is why start and current are separate fields. */
    lexer->start = lexer->current;

    if (is_at_end(lexer))
        return make_token(lexer, TOK_EOF);

    char c = advance(lexer);

    /* ── Identifiers and keywords ──────────────────────────────────────── */
    if (c == '_' || (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z'))
        return scan_identifier_or_keyword(lexer);

    /* ── Numeric literals ──────────────────────────────────────────────── */
    if (c >= '0' && c <= '9')
        return scan_number(lexer);

    /* ── Single and double character tokens ───────────────────────────── */
    switch (c)
    {
    /* Delimiters — always single character */
    case '(':
        return make_token(lexer, TOK_LPAREN);
    case ')':
        return make_token(lexer, TOK_RPAREN);
    case '{':
        if (lexer->interp_depth > 0 && lexer->in_expr)
            lexer->brace_depth++;
        return make_token(lexer, TOK_LBRACE);
    case '}':
        if (lexer->interp_depth > 0 && lexer->in_expr)
        {
            if (lexer->brace_depth == 0)
            {
                /* This } closes the expression, not a nested block */
                lexer->in_expr = false;
                return make_token(lexer, TOK_INTERP_RBRACE);
            }
            lexer->brace_depth--;
        }
        return make_token(lexer, TOK_RBRACE);
    case '[':
        return make_token(lexer, TOK_LBRACKET);
    case ']':
        return make_token(lexer, TOK_RBRACKET);
    case ',':
        return make_token(lexer, TOK_COMMA);
    case ';':
        return make_token(lexer, TOK_SEMICOLON);
    case ':':
        return make_token(lexer, TOK_COLON);
    case '.':
        return make_token(lexer, TOK_DOT);

    /* Arithmetic — always single character at this stage */
    case '+':
        return make_token(lexer, match(lexer, '+') ? TOK_PLUS_PLUS : TOK_PLUS);
    case '-':
        return make_token(lexer, match(lexer, '-') ? TOK_MINUS_MINUS : TOK_MINUS);
    case '*':
        return make_token(lexer, TOK_STAR);
    case '/':
        return make_token(lexer, TOK_SLASH);
    case '%':
        return make_token(lexer, TOK_PERCENT);

    /* Two-character tokens: we use match() for the second character.
     * If match() succeeds the second char is already consumed. */
    case '@':
        return make_token(lexer, TOK_AT);
    case '!':
        return make_token(lexer, match(lexer, '=') ? TOK_NEQ : TOK_BANG);
    case '=':
        return make_token(lexer, match(lexer, '=') ? TOK_EQ : TOK_ASSIGN);
    case '<':
        return make_token(lexer, match(lexer, '=') ? TOK_LTE : TOK_LT);
    case '>':
        return make_token(lexer, match(lexer, '=') ? TOK_GTE : TOK_GT);
    case '&':
        if (match(lexer, '&'))
            return make_token(lexer, TOK_AND);
        return error_token(lexer, "Expected '&&'. Single '&' not supported.");
    case '|':
        if (match(lexer, '|'))
            return make_token(lexer, TOK_OR);
        return error_token(lexer, "Expected '||'. Single '|' not supported.");

    /* String literals — plain and interpolated */
    case '$':
        if (peek(lexer) == '"')
        {
            advance(lexer); /* consume the '"' */
            lexer->interp_depth++;
            lexer->in_expr = false;
            lexer->brace_depth = 0;
            return make_token(lexer, TOK_INTERP_BEGIN);
        }
        return error_token(lexer, "Expected '\"' after '$' for interpolated string.");
    case '"':
        return scan_string(lexer);
    case '\'':
        return scan_char(lexer);
    }

    /* If we reach here, the character isn't part of the language. */
    return error_token(lexer, "Unexpected character.");
}

/* ─────────────────────────────────────────────────────────────────────────────
 * UTILITY
 * ───────────────────────────────────────────────────────────────────────────*/

const char *token_type_name(TokenType type)
{
    switch (type)
    {
    case TOK_INT_LIT:
        return "int literal";
    case TOK_FLOAT_LIT:
        return "float literal";
    case TOK_STRING_LIT:
        return "string literal";
    case TOK_TRUE:
        return "true";
    case TOK_FALSE:
        return "false";
    case TOK_INTERP_BEGIN:
        return "$\"";
    case TOK_INTERP_TEXT:
        return "interpolated text";
    case TOK_INTERP_LBRACE:
        return "{";
    case TOK_INTERP_RBRACE:
        return "}";
    case TOK_INTERP_END:
        return "end of interpolated string";
    case TOK_IDENT:
        return "identifier";
    case TOK_INT:
        return "int";
    case TOK_FLOAT:
        return "float";
    case TOK_BOOL:
        return "bool";
    case TOK_STRING:
        return "string";
    case TOK_VOID:
        return "void";
    case TOK_IF:
        return "if";
    case TOK_ELSE:
        return "else";
    case TOK_WHILE:
        return "while";
    case TOK_FOR:
        return "for";
    case TOK_RETURN:
        return "return";
    case TOK_BREAK:
        return "break";
    case TOK_CONTINUE:
        return "continue";
    case TOK_MATCH:
        return "match";
    case TOK_CASE:
        return "case";
    case TOK_DEFAULT:
        return "default";
    case TOK_FN:
        return "function";
    case TOK_CLASS:
        return "class";
    case TOK_ENUM:
        return "enum";
    case TOK_NEW:
        return "new";
    case TOK_THIS:
        return "this";
    case TOK_SUPER:
        return "super";
    case TOK_AT:
        return "@";
    case TOK_PUBLIC:
        return "public";
    case TOK_PRIVATE:
        return "private";
    case TOK_PROTECTED:
        return "protected";
    case TOK_STATIC:
        return "static";
    case TOK_PLUS:
        return "+";
    case TOK_MINUS:
        return "-";
    case TOK_PLUS_PLUS:
        return "++";
    case TOK_MINUS_MINUS:
        return "--";
    case TOK_STAR:
        return "*";
    case TOK_SLASH:
        return "/";
    case TOK_PERCENT:
        return "%";
    case TOK_EQ:
        return "==";
    case TOK_NEQ:
        return "!=";
    case TOK_LT:
        return "<";
    case TOK_LTE:
        return "<=";
    case TOK_GT:
        return ">";
    case TOK_GTE:
        return ">=";
    case TOK_ASSIGN:
        return "=";
    case TOK_AND:
        return "&&";
    case TOK_OR:
        return "||";
    case TOK_BANG:
        return "!";
    case TOK_LPAREN:
        return "(";
    case TOK_RPAREN:
        return ")";
    case TOK_LBRACE:
        return "{";
    case TOK_RBRACE:
        return "}";
    case TOK_LBRACKET:
        return "[";
    case TOK_RBRACKET:
        return "]";
    case TOK_COMMA:
        return ",";
    case TOK_SEMICOLON:
        return ";";
    case TOK_COLON:
        return ":";
    case TOK_DOT:
        return ".";
    case TOK_EOF:
        return "<EOF>";
    case TOK_ERROR:
        return "<error>";
    default:
        return "<unknown>";
    }
}