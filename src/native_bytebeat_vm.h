#pragma once

#include "native_bytebeat_bytecode.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct native_bytebeat_vm native_bytebeat_vm_t;

typedef enum {
    NATIVE_BYTEBEAT_VM_OK,
    NATIVE_BYTEBEAT_VM_INVALID_ARGUMENT,
    NATIVE_BYTEBEAT_VM_UNSUPPORTED_OPERATION,
    NATIVE_BYTEBEAT_VM_STACK_ERROR,
    NATIVE_BYTEBEAT_VM_RUNTIME_ERROR,
    NATIVE_BYTEBEAT_VM_STEP_LIMIT,
    NATIVE_BYTEBEAT_VM_NO_MEMORY
} native_bytebeat_vm_result_t;

/**
 * Create a VM only when its bytecode is supported and its initial sample runs.
 * On failure, out_vm is set to NULL and the precise failure code is returned.
 */
native_bytebeat_vm_result_t native_bytebeat_vm_create(
    const native_bytebeat_program_t *program,
    native_bytebeat_vm_t **out_vm);

void native_bytebeat_vm_destroy(native_bytebeat_vm_t *vm);

void native_bytebeat_vm_reset(native_bytebeat_vm_t *vm);

native_bytebeat_vm_result_t native_bytebeat_vm_evaluate(
    native_bytebeat_vm_t *vm,
    double source_time,
    double *out_sample);

#ifdef __cplusplus
}
#endif
