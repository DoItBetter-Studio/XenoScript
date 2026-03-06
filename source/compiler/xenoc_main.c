/*
 * xenoc_main.c — XenoScript standalone compiler
 */
#include "compile_pipeline.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static void print_usage(void) {
    printf("Usage: xenoc <source.xeno> [-o output.xbc] [--dump]\n");
    printf("       xenoc --help\n\n");
    printf("System modules:\n");
    for (int i = 0; i < STDLIB_XAR_TOTAL_COUNT; i++)
        printf("  <%s>\n", STDLIB_XAR_TABLE[i].name);
}

static void make_output_path(const char *in, char *out, size_t sz) {
    strncpy(out, in, sz-1); out[sz-1]='\0';
    char *dot = strrchr(out,'.');
    if (dot) strncpy(dot,".xbc",sz-(dot-out)-1);
    else      strncat(out,".xbc",sz-strlen(out)-1);
}

/* ── Main ─────────────────────────────────────────────────────────────── */

int main(int argc, char **argv) {
    if(argc<2){print_usage();return 3;}
    if(strcmp(argv[1],"--help")==0){print_usage();return 0;}

    const char *input_path=NULL, *output_path=NULL;
    bool dump_only=false;

    for(int i=1;i<argc;i++){
        if(strcmp(argv[i],"-o")==0){if(++i>=argc){fprintf(stderr,"xenoc: -o requires path\n");return 3;} output_path=argv[i];}
        else if(strcmp(argv[i],"--dump")==0) dump_only=true;
        else if(argv[i][0]=='-'){fprintf(stderr,"xenoc: unknown option '%s'\n",argv[i]);return 3;}
        else { if(input_path){fprintf(stderr,"xenoc: multiple inputs not supported\n");return 3;} input_path=argv[i]; }
    }
    if(!input_path){fprintf(stderr,"xenoc: no input file\n");print_usage();return 3;}

    char *main_source = pipeline_read_file(input_path);
    if (!main_source) { fprintf(stderr,"xenoc: cannot open '%s'\n",input_path); return 2; }

    Module staging; module_init(&staging);
    bool import_err = false;
    char *merged = pipeline_prepare(main_source, input_path, &staging, &import_err);
    free(main_source);
    if (import_err || !merged) { free(merged); module_free(&staging); return 1; }

    Lexer lexer; Parser parser; Checker *checker=malloc(sizeof(Checker));
    Compiler compiler; Module module;
    lexer_init(&lexer,merged); parser_init(&parser,&lexer);
    checker_init(checker,&parser.arena); module_init(&module);

    int exit_code=0;

    Type void_t=type_void(), any_t=type_any();
    checker_declare_host(checker,"print",void_t,&any_t,1);
    CompilerHostTable host_table;
    compiler_host_table_init(&host_table);
    compiler_host_table_add_any(&host_table,"print",0,1);
    stdlib_declare_host_fns(checker,&host_table);
    pipeline_declare_staging(checker,&staging);

    /* Merge staging into module BEFORE compilation so compiler can find
     * stdlib function indices via module_find (emits OP_CALL not OP_CALL_HOST) */
    module_merge(&module,&staging);

    Program program=parser_parse(&parser);
    if(parser.had_error){
        fprintf(stderr,"xenoc: parse errors in '%s':\n",input_path);
        parser_print_errors(&parser); exit_code=1; goto cleanup;
    }

    {
        bool ok=checker_check(checker,&program);
        if(checker->error_count>0){if(!ok)fprintf(stderr,"xenoc: type errors in '%s':\n",input_path); checker_print_errors(checker);}
        if(!ok){exit_code=1;goto cleanup;}
    }

    if(!compiler_compile(&compiler,&program,&module,&host_table)){
        fprintf(stderr,"xenoc: compile errors in '%s':\n",input_path);
        compiler_print_errors(&compiler); exit_code=1; goto cleanup;
    }

    if(dump_only){
        module_disassemble(&module);
    } else {
        char default_out[512];
        if(!output_path){make_output_path(input_path,default_out,sizeof(default_out));output_path=default_out;}
        XbcResult r=xbc_write(&module,output_path);
        if(r!=XBC_OK){fprintf(stderr,"xenoc: failed to write '%s': %s\n",output_path,xbc_result_str(r));exit_code=2;goto cleanup;}
        printf("xenoc: compiled '%s' -> '%s'\n",input_path,output_path);
    }

cleanup:
    module_free(&module); module_free(&staging);
    parser_free(&parser); free(checker); free(merged);
    return exit_code;
}
