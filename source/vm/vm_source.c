#include "vm_source.h"

#include "checker.h"
#include "lexer.h"
#include "parser.h"
#include "../../source/compiler/compile_pipeline.h"

#include <stdio.h>
#include <stdlib.h>

XenoResult xeno_vm_run_source(XenoVM *vm, const char *source)
{
    if (vm->has_source_module)
    {
        module_free(&vm->source_module);
        vm->has_source_module = false;
    }

    Lexer lexer;
    Parser parser;
    Checker *checker = malloc(sizeof(Checker));
    Compiler compiler;
    CompilerHostTable host_table;

    if (!checker)
    {
        xeno_vm_error(vm, "Out of memory");
        return XENO_RUNTIME_ERROR;
    }

    lexer_init(&lexer, source);
    parser_init(&parser, &lexer);
    checker_init(checker, &parser.arena);
    compiler_host_table_init(&host_table);
    module_init(&vm->source_module);

    Module *staging = (Module *)calloc(1, sizeof(Module));
    module_init(staging);
    bool import_err = false;
    char *merged = pipeline_prepare(source, NULL, staging, &import_err);
    if (import_err || !merged)
    {
        free(merged);
        module_free(staging);
        free(staging);
        xeno_vm_error(vm, "Import error");
        free(checker);
        return XENO_COMPILE_ERROR;
    }

    parser_free(&parser);
    lexer_init(&lexer, merged);
    parser_init(&parser, &lexer);
    checker_init(checker, &parser.arena);

    for (int i = 0; i < vm->host_fn_count; i++)
    {
        HostFnEntry *e = &vm->host_fns[i];
        Type ret_type = {.kind = e->return_type_kind};
        Type params[MAX_PARAMS];
        int pcount = e->param_count < 0 ? 0 : e->param_count;
        if (pcount > MAX_PARAMS)
            pcount = MAX_PARAMS;

        for (int p = 0; p < pcount; p++)
            params[p].kind = e->param_type_kinds[p];

        checker_declare_host(checker, e->name, ret_type, params, pcount);

        bool has_any = false;
        for (int p = 0; p < e->param_count; p++)
            if (e->param_type_kinds[p] == TYPE_ANY)
            {
                has_any = true;
                break;
            }
        if (has_any)
            compiler_host_table_add_any(&host_table, e->name, i, pcount);
        else
            compiler_host_table_add(&host_table, e->name, i, pcount);
    }

    pipeline_declare_staging(checker, staging);
    for (int i = 0; i < vm->stdlib_module_count; i++)
        pipeline_declare_staging(checker, vm->stdlib_modules[i]);

    Program program = parser_parse(&parser);
    if (parser.had_error)
    {
        xeno_vm_error(vm, "Parse error at line %d: %s",
                      parser.errors[0].line,
                      parser.errors[0].message);
        parser_free(&parser);
        free(merged);
        free(checker);
        module_free(staging);
        free(staging);
        return XENO_COMPILE_ERROR;
    }

    bool checker_ok = checker_check(checker, &program);
    for (int i = 0; i < checker->error_count; i++)
    {
        if (checker->errors[i].is_warning)
            fprintf(stderr, "[line %d] Warning: %s\n",
                    checker->errors[i].line, checker->errors[i].message);
    }
    if (!checker_ok)
    {
        xeno_vm_error(vm, "Type error at line %d: %s",
                      checker->errors[0].line,
                      checker->errors[0].message);
        parser_free(&parser);
        free(merged);
        free(checker);
        module_free(staging);
        free(staging);
        return XENO_COMPILE_ERROR;
    }

    for (int i = 0; i < vm->stdlib_module_count; i++)
        module_merge(staging, vm->stdlib_modules[i]);

    if (vm->compile_source_path)
        compiler_set_source_path(&compiler, vm->compile_source_path);
    if (!compiler_compile_staged(&compiler, &program, &vm->source_module,
                                 &host_table, staging))
    {
        xeno_vm_error(vm, "Compile error at line %d: %s",
                      compiler.errors[0].line,
                      compiler.errors[0].message);
        parser_free(&parser);
        free(merged);
        free(checker);
        module_free(staging);
        free(staging);
        return XENO_COMPILE_ERROR;
    }

    module_free(staging);
    free(staging);
    module_extract_mod_metadata(&vm->source_module, NULL);

    parser_free(&parser);
    free(merged);
    free(checker);
    vm->has_source_module = true;
    return xeno_vm_run(vm, &vm->source_module);
}
