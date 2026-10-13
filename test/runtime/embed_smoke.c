#include "vm.h"
#include "xbc.h"
#include "stdlib_register.h"

#include <stdio.h>
#include <string.h>

static int callback_count;

static XenoResult host_print(XenoVM *vm, int argc, Value *argv, Value *out)
{
    (void)out;
    if (argc != 1 || argv[0].is_null || !argv[0].s)
    {
        xeno_vm_error(vm, "embed smoke test received an invalid print argument");
        return XENO_RUNTIME_ERROR;
    }

    if (strcmp(argv[0].s, "embedded VM host callback passed") != 0)
    {
        xeno_vm_error(vm, "embed smoke test received unexpected callback text");
        return XENO_RUNTIME_ERROR;
    }

    callback_count++;
    return XENO_OK;
}

int main(int argc, char **argv)
{
    if (argc != 2)
    {
        fprintf(stderr, "usage: embed_smoke <script.xbc>\n");
        return 2;
    }

    XenoVM vm;
    Module module;
    int print_param_kind[] = {TYPE_ANY};

    xeno_vm_init(&vm);
    if (xeno_register_fn_typed(&vm, "print", host_print, TYPE_VOID, 1,
                               print_param_kind) < 0)
    {
        fprintf(stderr, "failed to register print callback\n");
        xeno_vm_free(&vm);
        return 1;
    }
    stdlib_register_host_fns(&vm);
    xeno_vm_load_stdlib(&vm);

    XbcResult read_result = xbc_read(&module, argv[1]);
    if (read_result != XBC_OK)
    {
        fprintf(stderr, "failed to read bytecode: %s\n",
                xbc_result_str(read_result));
        module_free(&module);
        xeno_vm_free(&vm);
        return 1;
    }

    XenoResult run_result = xeno_vm_run(&vm, &module);
    if (run_result != XENO_OK)
        xeno_vm_print_error(&vm);

    module_free(&module);
    xeno_vm_free(&vm);
    if (run_result != XENO_OK || callback_count != 1)
    {
        fprintf(stderr, "expected exactly one successful host callback\n");
        return 1;
    }
    return 0;
}
