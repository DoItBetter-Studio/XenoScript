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
    { "abs",     TYPE_INT,   1, { TYPE_INT,   0,         0         } },
    { "absf",    TYPE_FLOAT, 1, { TYPE_FLOAT, 0,         0         } },
    { "min",     TYPE_INT,   2, { TYPE_INT,   TYPE_INT,  0         } },
    { "max",     TYPE_INT,   2, { TYPE_INT,   TYPE_INT,  0         } },
    { "minf",    TYPE_FLOAT, 2, { TYPE_FLOAT, TYPE_FLOAT,0         } },
    { "maxf",    TYPE_FLOAT, 2, { TYPE_FLOAT, TYPE_FLOAT,0         } },
    { "clamp",   TYPE_INT,   3, { TYPE_INT,   TYPE_INT,  TYPE_INT  } },
    { "clampf",  TYPE_FLOAT, 3, { TYPE_FLOAT, TYPE_FLOAT,TYPE_FLOAT} },
    { "sign",    TYPE_INT,   1, { TYPE_INT,   0,         0         } },
    /* floating-point */
    { "sqrt",    TYPE_FLOAT, 1, { TYPE_FLOAT, 0,         0         } },
    { "pow",     TYPE_FLOAT, 2, { TYPE_FLOAT, TYPE_FLOAT,0         } },
    { "floor",   TYPE_FLOAT, 1, { TYPE_FLOAT, 0,         0         } },
    { "ceil",    TYPE_FLOAT, 1, { TYPE_FLOAT, 0,         0         } },
    { "round",   TYPE_FLOAT, 1, { TYPE_FLOAT, 0,         0         } },
    { "sin",     TYPE_FLOAT, 1, { TYPE_FLOAT, 0,         0         } },
    { "cos",     TYPE_FLOAT, 1, { TYPE_FLOAT, 0,         0         } },
    { "tan",     TYPE_FLOAT, 1, { TYPE_FLOAT, 0,         0         } },
    { "log",     TYPE_FLOAT, 1, { TYPE_FLOAT, 0,         0         } },
    { "log2",    TYPE_FLOAT, 1, { TYPE_FLOAT, 0,         0         } },
    { "log10",   TYPE_FLOAT, 1, { TYPE_FLOAT, 0,         0         } },
    /* conversion */
    { "toFloat", TYPE_FLOAT, 1, { TYPE_INT,   0,         0         } },
    { "toInt",   TYPE_INT,   1, { TYPE_FLOAT, 0,         0         } },
};

#define STDLIB_MATH_COUNT \
    (int)(sizeof(STDLIB_MATH_TABLE) / sizeof(STDLIB_MATH_TABLE[0]))

/* VM host index of the first math function (0 = print) */
#define STDLIB_MATH_HOST_INDEX_BASE 1

#endif /* STDLIB_MATH_TABLE_H */
