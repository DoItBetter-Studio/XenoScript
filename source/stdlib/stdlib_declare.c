/*
 * stdlib_declare.c — Declare stdlib host functions to the checker/compiler.
 *
 * Linked into xenoc (the standalone compiler).
 * Does NOT link against vm.c — no VM types used here.
 *
 * Call stdlib_declare_host_fns() after initialising the Checker and
 * CompilerHostTable, before parsing the user's source file.
 */

#include "stdlib_declare.h"
#include "stdlib_math_table.h"
#include <string.h>

void stdlib_declare_host_fns(Checker *checker, CompilerHostTable *host_table) {
    for (int i = 0; i < STDLIB_MATH_COUNT; i++) {
        const StdlibMathEntry *e = &STDLIB_MATH_TABLE[i];
        int host_index = STDLIB_MATH_HOST_INDEX_BASE + i;

        /* Build param Type array for the checker */
        Type param_types[4];
        for (int j = 0; j < e->param_count && j < 4; j++) {
            switch (e->param_kinds[j]) {
                case TYPE_FLOAT: param_types[j] = type_float(); break;
                default:         param_types[j] = type_int();   break;
            }
        }

        Type ret;
        switch (e->return_kind) {
            case TYPE_FLOAT: ret = type_float(); break;
            default:         ret = type_int();   break;
        }

        checker_declare_host(checker, e->name, ret,
                              e->param_count > 0 ? param_types : NULL,
                              e->param_count);

        compiler_host_table_add(host_table, e->name,
                                 host_index, e->param_count);
    }

    /* __array_grow(arr: T[], new_cap: int): T[]
     * Declared with TYPE_ANY so it accepts any array type. */
    {
        Type grow_params[2];
        grow_params[0].kind = TYPE_ANY;
        grow_params[0].element_type = NULL;
        grow_params[1] = type_int();
        Type grow_ret;
        grow_ret.kind = TYPE_ANY;
        grow_ret.element_type = NULL;
        int host_index = STDLIB_MATH_HOST_INDEX_BASE + STDLIB_MATH_COUNT;
        checker_declare_host(checker, "__array_grow", grow_ret, grow_params, 2);
        compiler_host_table_add(host_table, "__array_grow", host_index, 2);
    }
}
