/*
 * xenoc_main.c — XenoScript standalone compiler
 *
 * Usage:
 *   xenoc <source.xeno>              Compile to <source.xbc>
 *   xenoc <source.xeno> -o <out>     Compile to specified output path
 *   xenoc <source.xeno> --dump       Compile and disassemble (no .xbc output)
 *   xenoc --help
 *
 * Exit codes:
 *   0  Success
 *   1  Compile error
 *   2  I/O error
 *   3  Usage error
 */

#include "lexer.h"
#include "parser.h"
#include "checker.h"
#include "compiler.h"
#include "xbc.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static void print_usage(void) {
    printf("Usage: xenoc <source.xeno> [-o output.xbc] [--dump]\n");
    printf("       xenoc --help\n");
    printf("\n");
    printf("Options:\n");
    printf("  -o <path>   Output path (default: replaces .xeno with .xbc)\n");
    printf("  --dump      Disassemble output instead of writing .xbc\n");
    printf("  --help      Show this message\n");
}

/* Replace extension: "foo.xeno" -> "foo.xbc" */
static void make_output_path(const char *input, char *out, size_t out_size) {
    strncpy(out, input, out_size - 1);
    out[out_size - 1] = '\0';
    char *dot = strrchr(out, '.');
    if (dot) {
        strncpy(dot, ".xbc", out_size - (dot - out) - 1);
    } else {
        strncat(out, ".xbc", out_size - strlen(out) - 1);
    }
}

/* Read entire file into a heap-allocated string. Caller must free(). */
static char *read_file(const char *path) {
    FILE *f = fopen(path, "rb");
    if (!f) { fprintf(stderr, "xenoc: cannot open '%s'\n", path); return NULL; }

    fseek(f, 0, SEEK_END);
    long size = ftell(f);
    fseek(f, 0, SEEK_SET);

    char *buf = malloc(size + 1);
    if (!buf) { fclose(f); return NULL; }

    size_t nread = fread(buf, 1, size, f);
    if (nread != (size_t)size) { free(buf); fclose(f); return NULL; }
    buf[size] = '\0';
    fclose(f);
    return buf;
}

int main(int argc, char **argv) {
    if (argc < 2) { print_usage(); return 3; }
    if (strcmp(argv[1], "--help") == 0) { print_usage(); return 0; }

    const char *input_path  = NULL;
    const char *output_path = NULL;
    bool        dump_only   = false;

    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "-o") == 0) {
            if (++i >= argc) {
                fprintf(stderr, "xenoc: -o requires a path\n");
                return 3;
            }
            output_path = argv[i];
        } else if (strcmp(argv[i], "--dump") == 0) {
            dump_only = true;
        } else if (argv[i][0] == '-') {
            fprintf(stderr, "xenoc: unknown option '%s'\n", argv[i]);
            return 3;
        } else {
            if (input_path) {
                fprintf(stderr, "xenoc: multiple input files not supported\n");
                return 3;
            }
            input_path = argv[i];
        }
    }

    if (!input_path) {
        fprintf(stderr, "xenoc: no input file\n");
        print_usage();
        return 3;
    }

    /* Read source */
    char *source = read_file(input_path);
    if (!source) return 2;

    /* Pipeline */
    Lexer    lexer;
    Parser   parser;
    Checker *checker  = malloc(sizeof(Checker));
    Compiler compiler;
    Module   module;

    lexer_init(&lexer, source);
    parser_init(&parser, &lexer);
    checker_init(checker, &parser.arena);
    module_init(&module);

    int exit_code = 0;

    /* Declare the standard host functions that xenovm provides.
     * When compiling for a custom engine, these would come from a
     * host declaration file (--hostdecl flag — future work).
     * For now: declare the standard set so standalone scripts compile. */
        Type void_t   = type_void();
    Type any_t    = type_any();
    checker_declare_host(checker, "print", void_t, &any_t, 1);
    /* Also build the compiler host table for OP_CALL_HOST emission.
     * Without this, calls to host fns would emit OP_CALL (wrong). */
    CompilerHostTable host_table;
    compiler_host_table_init(&host_table);
    compiler_host_table_add_any(&host_table, "print", 0, 1);

    Program program = parser_parse(&parser);
    if (parser.had_error) {
        fprintf(stderr, "xenoc: parse errors in '%s':\n", input_path);
        parser_print_errors(&parser);
        exit_code = 1;
        goto cleanup;
    }

    bool checker_ok = checker_check(checker, &program);
    /* Always print warnings (and errors if any) */
    if (checker->error_count > 0) {
        if (!checker_ok)
            fprintf(stderr, "xenoc: type errors in '%s':\n", input_path);
        checker_print_errors(checker);
    }
    if (!checker_ok) {
        exit_code = 1;
        goto cleanup;
    }

    if (!compiler_compile(&compiler, &program, &module, &host_table)) {
        fprintf(stderr, "xenoc: compile errors in '%s':\n", input_path);
        compiler_print_errors(&compiler);
        exit_code = 1;
        goto cleanup;
    }

    if (dump_only) {
        module_disassemble(&module);
    } else {
        char default_out[512];
        if (!output_path) {
            make_output_path(input_path, default_out, sizeof(default_out));
            output_path = default_out;
        }

        XbcResult r = xbc_write(&module, output_path);
        if (r != XBC_OK) {
            fprintf(stderr, "xenoc: failed to write '%s': %s\n",
                    output_path, xbc_result_str(r));
            exit_code = 2;
            goto cleanup;
        }

        printf("xenoc: compiled '%s' -> '%s'\n", input_path, output_path);
    }

cleanup:
    module_free(&module);
    parser_free(&parser);
    free(checker);
    free(source);
    return exit_code;
}