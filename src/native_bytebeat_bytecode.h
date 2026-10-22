#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    NATIVE_BYTEBEAT_MODE_BYTEBEAT = 0,
    NATIVE_BYTEBEAT_MODE_FLOATBEAT = 1,
    NATIVE_BYTEBEAT_MODE_SIGNED = 2,
    NATIVE_BYTEBEAT_MODE_FUNCBEAT = 3
} native_bytebeat_mode_t;

typedef enum {
    NATIVE_BC_PUSH_NUMBER = 1,
    NATIVE_BC_PUSH_STRING,
    NATIVE_BC_PUSH_BOOLEAN,
    NATIVE_BC_PUSH_NULL,
    NATIVE_BC_PUSH_UNDEFINED,
    NATIVE_BC_TO_STRING,
    NATIVE_BC_LOAD_THIS,
    NATIVE_BC_LOAD_T,
    NATIVE_BC_LOAD_SAMPLE_RATE,
    NATIVE_BC_LOAD_LOCAL,
    NATIVE_BC_LOAD_GLOBAL,
    NATIVE_BC_BINARY,
    NATIVE_BC_JUMP_IF_FALSE_POP,
    NATIVE_BC_JUMP,
    NATIVE_BC_UNARY,
    NATIVE_BC_POP,
    NATIVE_BC_SPREAD_ARRAY,
    NATIVE_BC_PUSH_MATH_CONSTANT,
    NATIVE_BC_MAKE_OBJECT,
    NATIVE_BC_GET_INDEX,
    NATIVE_BC_GET_PROPERTY,
    NATIVE_BC_CALL,
    NATIVE_BC_CALL_VALUE,
    NATIVE_BC_SPREAD_ARGUMENT,
    NATIVE_BC_DUP,
    NATIVE_BC_STORE_GLOBAL,
    NATIVE_BC_SET_PROPERTY,
    NATIVE_BC_JUMP_IF_NOT_NULLISH,
    NATIVE_BC_KEEP_VALUE_DROP_REF,
    NATIVE_BC_GET_PROPERTY_KEEP_REF,
    NATIVE_BC_UPDATE_PROPERTY,
    NATIVE_BC_DUP2,
    NATIVE_BC_MAKE_ARRAY,
    NATIVE_BC_MAKE_FUNCTION,
    NATIVE_BC_JUMP_IF_FALSE,
    NATIVE_BC_JUMP_IF_NULLISH,
    NATIVE_BC_JUMP_IF_TRUE,
    NATIVE_BC_UPDATE_LOCAL,
    NATIVE_BC_UPDATE_GLOBAL,
    NATIVE_BC_JUMP_IF_UNDEFINED,
    NATIVE_BC_RETURN,
    NATIVE_BC_STORE_LOCAL,
    NATIVE_BC_THROW,
    NATIVE_BC_ITER_BEGIN,
    NATIVE_BC_ITER_NEXT,
    NATIVE_BC_ITER_END,
    NATIVE_BC_CONSTRUCT_ARRAY
} native_bytebeat_opcode_t;

typedef enum {
    NATIVE_BC_FIELD_VALUE = 1,
    NATIVE_BC_FIELD_NAME,
    NATIVE_BC_FIELD_OPERATOR,
    NATIVE_BC_FIELD_TARGET,
    NATIVE_BC_FIELD_ARGUMENT_COUNT,
    NATIVE_BC_FIELD_KEYS,
    NATIVE_BC_FIELD_CONSTRUCTOR,
    NATIVE_BC_FIELD_PARAMETERS,
    NATIVE_BC_FIELD_REST_INDEX,
    NATIVE_BC_FIELD_INSTRUCTIONS,
    NATIVE_BC_FIELD_PREFIX,
    NATIVE_BC_FIELD_ITERATOR,
    NATIVE_BC_FIELD_LENGTH,
    NATIVE_BC_FIELD_SPREAD_FLAGS,
    NATIVE_BC_FIELD_LOCAL
} native_bytebeat_field_id_t;

typedef enum {
    NATIVE_BC_VALUE_NULL,
    NATIVE_BC_VALUE_BOOLEAN,
    NATIVE_BC_VALUE_INTEGER,
    NATIVE_BC_VALUE_NUMBER,
    NATIVE_BC_VALUE_STRING,
    NATIVE_BC_VALUE_LIST,
    NATIVE_BC_VALUE_INSTRUCTIONS
} native_bytebeat_value_type_t;

typedef struct native_bytebeat_value native_bytebeat_value_t;
typedef struct native_bytebeat_instruction native_bytebeat_instruction_t;

struct native_bytebeat_value {
    native_bytebeat_value_type_t type;
    union {
        bool boolean;
        int64_t integer;
        double number;
        struct {
            char *data;
            uint32_t length;
        } string;
        struct {
            native_bytebeat_value_t *items;
            uint32_t count;
        } list;
        struct {
            native_bytebeat_instruction_t *items;
            uint32_t count;
        } instructions;
    } as;
};

typedef struct {
    uint8_t id;
    native_bytebeat_value_t value;
} native_bytebeat_field_t;

struct native_bytebeat_instruction {
    uint8_t opcode;
    native_bytebeat_field_t *fields;
    uint8_t field_count;
};

typedef struct {
    uint8_t mode;
    double sample_rate;
    native_bytebeat_instruction_t *instructions;
    uint32_t instruction_count;
} native_bytebeat_program_t;

typedef enum {
    NATIVE_BC_ERROR_NONE,
    NATIVE_BC_ERROR_INVALID_ARGUMENT,
    NATIVE_BC_ERROR_INVALID_HEADER,
    NATIVE_BC_ERROR_UNSUPPORTED_VERSION,
    NATIVE_BC_ERROR_INVALID_MODE,
    NATIVE_BC_ERROR_INVALID_FLAGS,
    NATIVE_BC_ERROR_INVALID_LENGTH,
    NATIVE_BC_ERROR_CHECKSUM,
    NATIVE_BC_ERROR_MALFORMED_PAYLOAD,
    NATIVE_BC_ERROR_NO_MEMORY
} native_bytebeat_error_t;

native_bytebeat_error_t native_bytebeat_program_load(
    const uint8_t *data,
    size_t length,
    native_bytebeat_program_t **out_program);

void native_bytebeat_program_free(native_bytebeat_program_t *program);

#ifdef __cplusplus
}
#endif
