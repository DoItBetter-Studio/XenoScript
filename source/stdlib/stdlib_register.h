/*
 * stdlib_register.h — VM-side stdlib API
 * Link stdlib_register.c into xenovm. Requires vm.c.
 */
#ifndef STDLIB_REGISTER_H
#define STDLIB_REGISTER_H

#include "vm.h"

/* Register all stdlib host functions with the VM.
 * Call after registering "print" (index 0), before running any script. */
void stdlib_register_host_fns(XenoVM *vm);

#endif
