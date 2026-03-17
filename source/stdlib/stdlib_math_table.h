/*
 * stdlib_math_table.h — <math> host function name/signature table
 *
 * Included by both the compiler-side (stdlib_declare.c) and VM-side
 * (stdlib_register.c) to guarantee they register in identical order.
 * Index 0 of this table maps to VM host_fn index 1 (index 0 = "print").
 *
 * Each row: { name, return_kind, param_count, param_kinds[4] }
 * TypeKind values: TYPE_INT=3, TYPE_FLOAT=4 (from ast.h)
 */

#ifndef STDLIB_MATH_TABLE_H
#define STDLIB_MATH_TABLE_H

#include "ast.h"  /* TypeKind */

typedef struct {
    const char *name;
    int         return_kind;
    int         param_count;
    int         param_kinds[4];
} StdlibMathEntry;

static const StdlibMathEntry STDLIB_MATH_TABLE[] = {
    /* integer arithmetic */
    { "__abs",     TYPE_INT,   1, { TYPE_INT,   0,         0         } },
    { "__absf",    TYPE_FLOAT, 1, { TYPE_FLOAT, 0,         0         } },
    { "__min",     TYPE_INT,   2, { TYPE_INT,   TYPE_INT,  0         } },
    { "__max",     TYPE_INT,   2, { TYPE_INT,   TYPE_INT,  0         } },
    { "__minf",    TYPE_FLOAT, 2, { TYPE_FLOAT, TYPE_FLOAT,0         } },
    { "__maxf",    TYPE_FLOAT, 2, { TYPE_FLOAT, TYPE_FLOAT,0         } },
    { "__clamp",   TYPE_INT,   3, { TYPE_INT,   TYPE_INT,  TYPE_INT  } },
    { "__clampf",  TYPE_FLOAT, 3, { TYPE_FLOAT, TYPE_FLOAT,TYPE_FLOAT} },
    { "__sign",    TYPE_INT,   1, { TYPE_INT,   0,         0         } },
    /* floating-point */
    { "__sqrt",    TYPE_FLOAT, 1, { TYPE_FLOAT, 0,         0         } },
    { "__pow",     TYPE_FLOAT, 2, { TYPE_FLOAT, TYPE_FLOAT,0         } },
    { "__floor",   TYPE_FLOAT, 1, { TYPE_FLOAT, 0,         0         } },
    { "__ceil",    TYPE_FLOAT, 1, { TYPE_FLOAT, 0,         0         } },
    { "__round",   TYPE_FLOAT, 1, { TYPE_FLOAT, 0,         0         } },
    { "__sin",     TYPE_FLOAT, 1, { TYPE_FLOAT, 0,         0         } },
    { "__cos",     TYPE_FLOAT, 1, { TYPE_FLOAT, 0,         0         } },
    { "__tan",     TYPE_FLOAT, 1, { TYPE_FLOAT, 0,         0         } },
    { "__log",     TYPE_FLOAT, 1, { TYPE_FLOAT, 0,         0         } },
    { "__log2",    TYPE_FLOAT, 1, { TYPE_FLOAT, 0,         0         } },
    { "__log10",   TYPE_FLOAT, 1, { TYPE_FLOAT, 0,         0         } },
    /* conversion */
    { "__toFloat", TYPE_FLOAT, 1, { TYPE_INT,   0,         0         } },
    { "__toInt",   TYPE_INT,   1, { TYPE_FLOAT, 0,         0         } },
};

#define STDLIB_MATH_COUNT \
    (int)(sizeof(STDLIB_MATH_TABLE) / sizeof(STDLIB_MATH_TABLE[0]))

/* VM host index of the first math function (0 = print) */
#define STDLIB_MATH_HOST_INDEX_BASE 1

#endif /* STDLIB_MATH_TABLE_H */
