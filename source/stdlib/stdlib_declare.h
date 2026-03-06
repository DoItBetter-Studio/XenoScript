/*
 * stdlib_declare.h — Compiler/checker-side stdlib API
 * Link stdlib_declare.c into xenoc. Does not require vm.c.
 */
#ifndef STDLIB_DECLARE_H
#define STDLIB_DECLARE_H

#include "checker.h"
#include "compiler.h"

/* Declare all stdlib host functions to the checker and compiler host table.
 * Call before parsing any source that uses a stdlib module. */
void stdlib_declare_host_fns(Checker *checker, CompilerHostTable *host_table);

#endif
