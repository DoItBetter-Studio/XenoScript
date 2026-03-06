/*
 * xenoc_main.c — XenoScript standalone compiler
 *
 * Import resolution:
 *   import <n>;           — system module loaded from embedded .xar archive
 *   import "file.xeno";   — local file compiled and merged at compile time
 *
 * System imports are resolved by reading the embedded .xar blob for that
 * module name, loading its compiled .xbc chunks into a staging Module,
 * then merging the staging Module into the output Module so the type
 * checker and compiler can resolve calls to stdlib functions.
 */
#define _DEFAULT_SOURCE

#include "lexer.h"
#include "parser.h"
#include "checker.h"
#include "compiler.h"
#include "xbc.h"
#include "xar.h"
#include "stdlib_xar.h"
#include "../../source/stdlib/stdlib_declare.h"
#include "../../source/stdlib/stdlib_sources.h"
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

static char *read_file(const char *path) {
    FILE *f = fopen(path,"rb"); if(!f){fprintf(stderr,"xenoc: cannot open '%s'\n",path);return NULL;}
    fseek(f,0,SEEK_END); long sz=ftell(f); fseek(f,0,SEEK_SET);
    char *buf=malloc(sz+1); if(!buf){fclose(f);return NULL;}
    size_t nr = fread(buf,1,sz,f); (void)nr; fclose(f); buf[sz]='\0'; return buf;
}

static void dir_of(const char *path, char *out, size_t sz) {
    if (sz == 0) return;

    // Copy safely
    size_t len = strlen(path);
    if (len >= sz) len = sz - 1;
    memcpy(out, path, len);
    out[len] = '\0';

    // Strip filename
    char *sep = strrchr(out, '/');
    if (!sep) sep = strrchr(out, '\\');
    if (sep) *(sep + 1) = '\0';
    else out[0] = '\0';
}

/* ── System import (.xar) loading ────────────────────────────────────── */

static char sys_loaded[32][64];
static int  sys_loaded_count = 0;

static bool sys_already_loaded(const char *name) {
    for(int i=0;i<sys_loaded_count;i++) if(strcmp(sys_loaded[i],name)==0) return true;
    return false;
}

static bool load_system_module(const char *name, Module *staging) {
    if (sys_already_loaded(name)) return true;
    for (int i=0; i<STDLIB_XAR_TOTAL_COUNT; i++) {
        if (strcmp(STDLIB_XAR_TABLE[i].name, name)!=0) continue;
        size_t sz=(size_t)(STDLIB_XAR_TABLE[i].end-STDLIB_XAR_TABLE[i].start);
        XarArchive ar;
        if (xar_read_mem(&ar, STDLIB_XAR_TABLE[i].start, sz)!=XAR_OK) {
            fprintf(stderr,"xenoc: failed to read embedded stdlib '%s'\n",name); return false;
        }
        for (int j=0;j<ar.chunk_count;j++) {
            Module cm; module_init(&cm);
            if (xbc_read_mem(&cm,ar.chunks[j].data,ar.chunks[j].size)==XBC_OK) {
                module_merge(staging,&cm); module_free(&cm);
            }
        }
        xar_archive_free(&ar);
        if(sys_loaded_count<32) strncpy(sys_loaded[sys_loaded_count++],name,63);
        return true;
    }
    fprintf(stderr,"xenoc: unknown system module '<%s>'\n",name); return false;
}

/* ── Local import (source merge) ─────────────────────────────────────── */

#define MAX_LOCAL_IMPORTS 64
static char local_imported[MAX_LOCAL_IMPORTS][1024];
static int  local_import_count = 0;

static bool local_already_seen(const char *k) {
    for(int i=0;i<local_import_count;i++) if(strcmp(local_imported[i],k)==0) return true;
    return false;
}
static void local_mark_seen(const char *k) {
    if(local_import_count<MAX_LOCAL_IMPORTS) snprintf(local_imported[local_import_count++], 1024, "%s", k);
}

static char *buf_append(char *buf, size_t *len, size_t *cap, const char *s, size_t slen) {
    while(*len+slen+1>*cap){*cap*=2; buf=realloc(buf,*cap); if(!buf)return NULL;}
    memcpy(buf+*len,s,slen); *len+=slen; buf[*len]='\0'; return buf;
}

static char *resolve_imports(const char *source, const char *base_dir,
                               const char *label,
                               char *out, size_t *len, size_t *cap,
                               Module *staging, bool *err) {
    const char *p=source;
    while(*p) {
        while(*p==' '||*p=='\t'||*p=='\r'||*p=='\n') p++;
        if(!*p) break;
        if(p[0]=='/'&&p[1]=='/'){while(*p&&*p!='\n')p++; continue;}
        if(p[0]=='/'&&p[1]=='*'){p+=2;while(*p&&!(p[0]=='*'&&p[1]=='/'))p++;if(*p)p+=2;continue;}
        if(strncmp(p,"import",6)!=0||(p[6]!=' '&&p[6]!='\t'&&p[6]!='<'&&p[6]!='"')) break;
        p+=6; while(*p==' '||*p=='\t') p++;

        bool is_sys=false; char name[512]; int nlen=0;
        if(*p=='<'){
            is_sys=true; p++;
            const char *s=p; while(*p&&*p!='>'&&*p!='\n')p++;
            nlen=(int)(p-s); if(nlen>511)nlen=511; memcpy(name,s,nlen); name[nlen]='\0';
            if(*p=='>') p++;
        } else if(*p=='"'){
            p++;
            const char *s=p; while(*p&&*p!='"'&&*p!='\n')p++;
            nlen=(int)(p-s); if(nlen>511)nlen=511; memcpy(name,s,nlen); name[nlen]='\0';
            if(*p=='"') p++;
        } else { while(*p&&*p!=';'&&*p!='\n')p++; if(*p==';')p++; continue; }
        while(*p==' '||*p=='\t') p++;
        if(*p==';') p++;

        if(is_sys) {
            /* Modules with generic classes must be source-merged so the checker
             * can build full AST-backed class symbols with type parameter info.
             * Check if this module has source available in XENOSCRIPT_STDLIB. */
            bool source_merged = false;
            for (int si = 0; si < XENOSCRIPT_STDLIB_COUNT; si++) {
                if (strcmp(XENOSCRIPT_STDLIB[si].name, name) == 0) {
                    /* Source-merge: treat like a local import with the embedded source */
                    char skey[768]; snprintf(skey, sizeof(skey), "<src:%s>", name);
                    if (!local_already_seen(skey)) {
                        local_mark_seen(skey);
                        out = resolve_imports(XENOSCRIPT_STDLIB[si].source, "",
                                              name, out, len, cap, staging, err);
                        if (*err || !out) return out;
                    }
                    source_merged = true;
                    break;
                }
            }
            if (!source_merged) {
                /* No source available — load compiled xar into staging */
                if(!load_system_module(name,staging)){*err=true; return out;}
            }
        } else {
            char key[768]; snprintf(key,sizeof(key),"%s%s",base_dir,name);
            if(!local_already_seen(key)){
                local_mark_seen(key);
                char fpath[1024]; snprintf(fpath,sizeof(fpath),"%s%s",base_dir,name);
                char *src=read_file(fpath);
                if(!src){fprintf(stderr,"xenoc: cannot open '%s' (from '%s')\n",fpath,label);*err=true;return out;}
                char sub[512]=""; dir_of(fpath,sub,sizeof(sub));
                out=resolve_imports(src,sub,fpath,out,len,cap,staging,err);
                free(src); if(*err||!out)return out;
            }
        }
    }
    /* Append only the non-import portion of this file (p now points past all imports) */
    out=buf_append(out,len,cap,p,strlen(p));
    out=buf_append(out,len,cap,"\n",1);
    return out;
}

/* Declare staging functions to checker using the type signature stored
 * in each chunk (return_type_kind / param_type_kinds). */
static Type kind_to_type(int kind) {
    switch (kind) {
        case TYPE_INT:    return type_int();
        case TYPE_FLOAT:  return type_float();
        case TYPE_BOOL:   return type_bool();
        case TYPE_STRING: return type_string();
        case TYPE_VOID:   return type_void();
        default:          return type_any();
    }
}

static void declare_staging(Checker *checker, const Module *staging) {
    for(int i=0;i<staging->count;i++){
        const char *nm=staging->names[i];
        /* Skip internal names and class methods (contain '.') */
        if(strcmp(nm,"__sinit__")==0) continue;
        if(strchr(nm,'.') != NULL) continue;  /* class method — registered via class def */
        Chunk *ch=&staging->chunks[i];
        Type ret = kind_to_type(ch->return_type_kind);
        int pc = ch->param_count < 16 ? ch->param_count : 16;
        Type params[16];
        for(int j=0;j<pc;j++) params[j]=kind_to_type(ch->param_type_kinds[j]);
        checker_declare_host(checker,nm,ret,pc>0?params:NULL,pc);
    }
    for(int i=0;i<staging->class_count;i++)
        checker_declare_class_from_def(checker,&staging->classes[i]);
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

    char *main_source=read_file(input_path); if(!main_source) return 2;

    sys_loaded_count=0; local_import_count=0;
    Module staging; module_init(&staging);
    load_system_module("core",&staging);  /* always load core */

    char base_dir[512]=""; dir_of(input_path,base_dir,sizeof(base_dir));
    size_t cap=65536,len=0;
    char *merged=malloc(cap); if(!merged){free(main_source);return 2;} merged[0]='\0';

    bool import_err=false;
    merged=resolve_imports(main_source,base_dir,input_path,merged,&len,&cap,&staging,&import_err);
    free(main_source);
    if(import_err||!merged){free(merged);module_free(&staging);return 1;}

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
    declare_staging(checker,&staging);

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
