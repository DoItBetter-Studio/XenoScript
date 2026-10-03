/*
 * module_strip_stdlib.c — strip embedded stdlib and project deps from a Module.
 *
 * Kept as a separate TU because it needs stdlib_xar.h (the embedded XAR
 * blobs), which must only be linked when the stdlib .o objects are present.
 * In XAR_BOOTSTRAP builds the table is empty so this is a safe no-op.
 *
 * Design:
 *   staging holds stdlib then project deps (e.g. libraries → Utils).
 *   User classes stay fully in the output .xar.
 *   Staging classes/chunks are removed entirely (no hollow shells).
 *   Bytecode keeps absolute compile-time class indices; the VM pre-seeds
 *   stdlib/deps in the same order before merging the mod.
 *
 * At runtime the VM must:
 *   1. Pre-seed stdlib (same order as compile)
 *   2. Load/merge each dep .xar once from mods/ or deps/
 *   3. Merge this mod (user ClassDefs append after the stdlib prefix)
 *
 * Multiple mods can share one libraries.xar without embedding it.
 */

#include "compiler.h"
#include "stdlib_xar.h"
#include "xar.h"
#include "xbc.h"
#include <stdlib.h>
#include <string.h>

void module_strip_stdlib(Module *module, const Module *staging) {
    bool had_external = false;
    if (staging) {
        for (int i = 0; i < staging->class_count && !had_external; i++)
            if (module_find_class(module, staging->classes[i].name) >= 0)
                had_external = true;
    }
    if (!had_external) {
        for (int xi = 0; xi < STDLIB_XAR_TOTAL_COUNT && !had_external; xi++) {
            size_t sz = (size_t)(STDLIB_XAR_TABLE[xi].end - STDLIB_XAR_TABLE[xi].start);
            XarArchive ar; memset(&ar, 0, sizeof(ar));
            if (xar_read_mem(&ar, STDLIB_XAR_TABLE[xi].start, sz) != XAR_OK) continue;
            for (int ci = 0; ci < ar.chunk_count && !had_external; ci++) {
                Module *cm = (Module*)calloc(1, sizeof(Module)); module_init(cm);
                if (xbc_read_mem(cm, ar.chunks[ci].data, ar.chunks[ci].size) == XBC_OK)
                    for (int ki = 0; ki < cm->class_count && !had_external; ki++)
                        if (module_find_class(module, cm->classes[ki].name) >= 0)
                            had_external = true;
                module_free(cm); free(cm);
            }
            xar_archive_free(&ar);
        }
    }
    module->uses_stdlib = had_external;
    if (!had_external) return;

    /* Pass 1: strip everything that lived in staging (stdlib + deps).
     * ClassDefs and chunks are removed; bytecode class indices are left as
     * absolute compile-time values for the VM stdlib seed to recreate. */
    if (staging && (staging->count > 0 || staging->class_count > 0)) {
        const char **names   = malloc((size_t)staging->count * sizeof(char*));
        const char **classes = malloc((size_t)(staging->class_count + 1) * sizeof(char*));
        if (names && classes) {
            for (int i = 0; i < staging->count;       i++) names[i]   = staging->names[i];
            for (int i = 0; i < staging->class_count; i++) classes[i] = staging->classes[i].name;
            module_strip(module, names, staging->count, classes, staging->class_count);
        }
        free(names); free(classes);
    }

    /* Pass 2: also strip each embedded stdlib XAR (covers anything that
     * matched stdlib by name but wasn't still listed in staging). */
    for (int xi = 0; xi < STDLIB_XAR_TOTAL_COUNT; xi++) {
        size_t sz = (size_t)(STDLIB_XAR_TABLE[xi].end - STDLIB_XAR_TABLE[xi].start);
        XarArchive ar; memset(&ar, 0, sizeof(ar));
        if (xar_read_mem(&ar, STDLIB_XAR_TABLE[xi].start, sz) != XAR_OK) continue;
        for (int ci = 0; ci < ar.chunk_count; ci++) {
            Module *cm = (Module*)calloc(1, sizeof(Module)); module_init(cm);
            if (xbc_read_mem(cm, ar.chunks[ci].data, ar.chunks[ci].size) == XBC_OK) {
                const char **fn_names = malloc((size_t)cm->count * sizeof(char*));
                const char **cl_names = malloc((size_t)(cm->class_count + 1) * sizeof(char*));
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