/*
 * stub_gen.h — XenoScript stub generator
 *
 * Given a ClassDef (from a compiled .xar), generates a read-only synthetic
 * .xeno source file that the LSP presents when the user navigates to a
 * definition inside a binary dependency.
 *
 * Param names are synthesised as arg0, arg1, ... (Java-style) since the
 * compiled ClassDef does not embed the original param names.
 */

#ifndef STUB_GEN_H
#define STUB_GEN_H

#include "../../includes/bytecode.h"
#include "../../includes/compiler.h"
#include <stddef.h>

/* Generate a stub source string for `def`.
 * Returns a heap-allocated null-terminated string the caller must free().
 * Returns NULL on allocation failure. */
char *stub_gen_class(const ClassDef *def, const Module *module);

#endif /* STUB_GEN_H */
