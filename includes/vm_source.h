#ifndef VM_SOURCE_H
#define VM_SOURCE_H

#include "vm.h"

/*
 * Development-only compile-and-run helper. Embedders should use xenoc to
 * compile scripts and pass compiled modules to xeno_vm_run().
 */
XenoResult xeno_vm_run_source(XenoVM *vm, const char *source);

#endif
