/*
 * module_strip_stdlib.c — strip embedded stdlib from a compiled Module.
 *
 * Kept as a separate TU because it needs stdlib_xar.h (the embedded XAR
 * blobs), which must only be linked when the stdlib .o objects are present.
 * In XAR_BOOTSTRAP builds the table is empty so this is a safe no-op.
 */

#include "compiler.h"
#include "stdlib_xar.h"
#include "xar.h"
#include "xbc.h"
#include <stdlib.h>
#include <string.h>

void module_strip_stdlib(Module *module, const Module *staging) {
    /* Detect whether this module used stdlib at all */
    bool had_stdlib = false;
    for (int i = 0; i < staging->class_count && !had_stdlib; i++)
        if (module_find_class(module, staging->classes[i].name) >= 0)
            had_stdlib = true;
    if (!had_stdlib) {
        for (int xi = 0; xi < STDLIB_XAR_TOTAL_COUNT && !had_stdlib; xi++) {
            size_t sz = (size_t)(STDLIB_XAR_TABLE[xi].end - STDLIB_XAR_TABLE[xi].start);
            XarArchive ar; memset(&ar, 0, sizeof(ar));
            if (xar_read_mem(&ar, STDLIB_XAR_TABLE[xi].start, sz) != XAR_OK) continue;
            for (int ci = 0; ci < ar.chunk_count && !had_stdlib; ci++) {
                Module *cm = (Module*)calloc(1, sizeof(Module)); module_init(cm);
                if (xbc_read_mem(cm, ar.chunks[ci].data, ar.chunks[ci].size) == XBC_OK)
                    for (int ki = 0; ki < cm->class_count && !had_stdlib; ki++)
                        if (module_find_class(module, cm->classes[ki].name) >= 0)
                            had_stdlib = true;
                module_free(cm); free(cm);
            }
            xar_archive_free(&ar);
        }
    }
    module->uses_stdlib = had_stdlib;
    if (!had_stdlib) return;

    /* Pass 1: strip staging-sourced (non-generic) stdlib chunks and classes */
    {
        const char **names   = malloc(staging->count * sizeof(char*));
        const char **classes = malloc((staging->class_count + 1) * sizeof(char*));
        if (names && classes) {
            for (int i = 0; i < staging->count;       i++) names[i]   = staging->names[i];
            for (int i = 0; i < staging->class_count; i++) classes[i] = staging->classes[i].name;
            module_strip(module, names, staging->count, classes, staging->class_count);
        }
        free(names); free(classes);
    }

    /* Pass 2: strip each embedded stdlib XAR's chunks and classes */
    for (int xi = 0; xi < STDLIB_XAR_TOTAL_COUNT; xi++) {
        size_t sz = (size_t)(STDLIB_XAR_TABLE[xi].end - STDLIB_XAR_TABLE[xi].start);
        XarArchive ar; memset(&ar, 0, sizeof(ar));
        if (xar_read_mem(&ar, STDLIB_XAR_TABLE[xi].start, sz) != XAR_OK) continue;
        for (int ci = 0; ci < ar.chunk_count; ci++) {
            Module *cm = (Module*)calloc(1, sizeof(Module)); module_init(cm);
            if (xbc_read_mem(cm, ar.chunks[ci].data, ar.chunks[ci].size) == XBC_OK) {
                const char **fn_names = malloc(cm->count * sizeof(char*));
                const char **cl_names = malloc((cm->class_count + 1) * sizeof(char*));
                if (fn_names && cl_names) {
                    for (int fi = 0; fi < cm->count;       fi++) fn_names[fi] = cm->names[fi];
                    for (int ki = 0; ki < cm->class_count; ki++) cl_names[ki] = cm->classes[ki].name;
                    module_strip(module, fn_names, cm->count, cl_names, cm->class_count);
                }
                free(fn_names); free(cl_names);
            }
            module_free(cm); free(cm);
        }
        xar_archive_free(&ar);
    }
}
