
/*
 * stub_gen.h — XenoScript stub generator
 *
 * Given a ClassDef (from a compiled .xar), generates a read-only synthetic
 * .xeno source file that the LSP presents when the user navigates to a
 * definition inside a binary dependency.
 *
 * Param names default to arg0, arg1, ...  When an XdocArchive is provided,
 * # param: $name entries for Class.method are used by index instead.
 * Parameter names are never stored in XBC — docs stay IDE-side only.
 */

#ifndef STUB_GEN_H
#define STUB_GEN_H

#include "../../includes/bytecode.h"
#include "../../includes/compiler.h"
#include "../../includes/xdoc.h"
#include <stddef.h>

/* Generate a stub source string for `def`.
 * `docs` may be NULL (then all params are argN).
 * Returns a heap-allocated null-terminated string the caller must free().
 * Returns NULL on allocation failure. */
char *stub_gen_class(const ClassDef *def, const Module *module, const XdocArchive *docs);

#endif /* STUB_GEN_H */