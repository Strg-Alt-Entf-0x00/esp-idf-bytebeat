#include "native_bytebeat_bytecode.h"

#include <float.h>
#include <limits.h>
#include <stdlib.h>
#include <string.h>

#define BYTECODE_HEADER_SIZE 24U
#define BYTECODE_VERSION 1U
#define MAX_NESTING_DEPTH 64U

enum {
    VALUE_TAG_NULL = 0,
    VALUE_TAG_FALSE,
    VALUE_TAG_TRUE,
    VALUE_TAG_INTEGER,
    VALUE_TAG_NUMBER,
    VALUE_TAG_STRING,
    VALUE_TAG_LIST,
    VALUE_TAG_INSTRUCTIONS
};

typedef struct {
    const uint8_t *data;
    size_t length;
    size_t position;
    native_bytebeat_error_t error;
} bytecode_reader_t;

static bool read_bytes(bytecode_reader_t *reader, size_t count, const uint8_t **out) {
    if (count > reader->length - reader->position) {
        reader->error = NATIVE_BC_ERROR_MALFORMED_PAYLOAD;
        return false;
    }
    *out = reader->data + reader->position;
    reader->position += count;
    return true;
}

static bool read_u8(bytecode_reader_t *reader, uint8_t *out) {
    const uint8_t *data;
    if (!read_bytes(reader, 1, &data)) return false;
    *out = data[0];
    return true;
}

static bool read_u16(bytecode_reader_t *reader, uint16_t *out) {
    const uint8_t *data;
    if (!read_bytes(reader, 2, &data)) return false;
    *out = (uint16_t)data[0] | ((uint16_t)data[1] << 8);
    return true;
}

static bool read_u32(bytecode_reader_t *reader, uint32_t *out) {
    const uint8_t *data;
    if (!read_bytes(reader, 4, &data)) return false;
    *out = (uint32_t)data[0]
        | ((uint32_t)data[1] << 8)
        | ((uint32_t)data[2] << 16)
        | ((uint32_t)data[3] << 24);
    return true;
}

static bool read_u64(bytecode_reader_t *reader, uint64_t *out) {
    const uint8_t *data;
    if (!read_bytes(reader, 8, &data)) return false;
    *out = (uint64_t)data[0]
        | ((uint64_t)data[1] << 8)
        | ((uint64_t)data[2] << 16)
        | ((uint64_t)data[3] << 24)
        | ((uint64_t)data[4] << 32)
        | ((uint64_t)data[5] << 40)
        | ((uint64_t)data[6] << 48)
        | ((uint64_t)data[7] << 56);
    return true;
}

static uint32_t crc32_bytes(const uint8_t *data, size_t length) {
    uint32_t crc = 0xFFFFFFFFU;
    for (size_t index = 0; index < length; ++index) {
        crc ^= data[index];
        for (unsigned bit = 0; bit < 8; ++bit) {
            const uint32_t mask = 0U - (crc & 1U);
            crc = (crc >> 1) ^ (0xEDB88320U & mask);
        }
    }
    return ~crc;
}

static void free_instructions(native_bytebeat_instruction_t *instructions, uint32_t count);

static void free_value(native_bytebeat_value_t *value) {
    if (!value) return;
    switch (value->type) {
        case NATIVE_BC_VALUE_STRING:
            free(value->as.string.data);
            break;
        case NATIVE_BC_VALUE_LIST:
            for (uint32_t index = 0; index < value->as.list.count; ++index) {
                free_value(&value->as.list.items[index]);
            }
            free(value->as.list.items);
            break;
        case NATIVE_BC_VALUE_INSTRUCTIONS:
            free_instructions(value->as.instructions.items, value->as.instructions.count);
            break;
        default:
            break;
    }
    memset(value, 0, sizeof(*value));
}

static void free_instructions(native_bytebeat_instruction_t *instructions, uint32_t count) {
    if (!instructions) return;
    for (uint32_t index = 0; index < count; ++index) {
        if (instructions[index].fields) {
            for (uint8_t field = 0; field < instructions[index].field_count; ++field) {
                free_value(&instructions[index].fields[field].value);
            }
        }
        free(instructions[index].fields);
    }
    free(instructions);
}

static bool valid_field_id(uint8_t id) {
    return id >= NATIVE_BC_FIELD_VALUE && id <= NATIVE_BC_FIELD_LOCAL;
}

static bool read_instruction_array(
    bytecode_reader_t *reader,
    uint32_t depth,
    native_bytebeat_instruction_t **out_instructions,
    uint32_t *out_count);

static bool read_value(
    bytecode_reader_t *reader,
    uint8_t field_id,
    uint32_t depth,
    native_bytebeat_value_t *out_value) {
    uint8_t tag;
    memset(out_value, 0, sizeof(*out_value));
    if (!read_u8(reader, &tag)) return false;

    switch (tag) {
        case VALUE_TAG_NULL:
            out_value->type = NATIVE_BC_VALUE_NULL;
            return field_id != NATIVE_BC_FIELD_INSTRUCTIONS;
        case VALUE_TAG_FALSE:
        case VALUE_TAG_TRUE:
            out_value->type = NATIVE_BC_VALUE_BOOLEAN;
            out_value->as.boolean = tag == VALUE_TAG_TRUE;
            return field_id != NATIVE_BC_FIELD_INSTRUCTIONS;
        case VALUE_TAG_INTEGER: {
            uint64_t bits;
            if (!read_u64(reader, &bits)) return false;
            memcpy(&out_value->as.integer, &bits, sizeof(bits));
            out_value->type = NATIVE_BC_VALUE_INTEGER;
            return field_id != NATIVE_BC_FIELD_INSTRUCTIONS;
        }
        case VALUE_TAG_NUMBER: {
            uint64_t bits;
            if (!read_u64(reader, &bits)) return false;
            memcpy(&out_value->as.number, &bits, sizeof(bits));
            out_value->type = NATIVE_BC_VALUE_NUMBER;
            return field_id != NATIVE_BC_FIELD_INSTRUCTIONS;
        }
        case VALUE_TAG_STRING: {
            uint32_t length;
            const uint8_t *text;
            if (!read_u32(reader, &length)
                || !read_bytes(reader, length, &text)
                || (size_t)length == SIZE_MAX) {
                return false;
            }
            out_value->as.string.data = (char *)malloc((size_t)length + 1);
            if (!out_value->as.string.data) {
                reader->error = NATIVE_BC_ERROR_NO_MEMORY;
                return false;
            }
            memcpy(out_value->as.string.data, text, length);
            out_value->as.string.data[length] = '\0';
            out_value->as.string.length = length;
            out_value->type = NATIVE_BC_VALUE_STRING;
            if (field_id == NATIVE_BC_FIELD_INSTRUCTIONS) {
                free_value(out_value);
                reader->error = NATIVE_BC_ERROR_MALFORMED_PAYLOAD;
                return false;
            }
            return true;
        }
        case VALUE_TAG_LIST: {
            uint32_t count;
            if (field_id == NATIVE_BC_FIELD_INSTRUCTIONS
                || !read_u32(reader, &count)
                || count > reader->length - reader->position) {
                reader->error = NATIVE_BC_ERROR_MALFORMED_PAYLOAD;
                return false;
            }
            if (count > SIZE_MAX / sizeof(native_bytebeat_value_t)) {
                reader->error = NATIVE_BC_ERROR_NO_MEMORY;
                return false;
            }
            out_value->as.list.items = count
                ? (native_bytebeat_value_t *)calloc(count, sizeof(native_bytebeat_value_t))
                : NULL;
            if (count && !out_value->as.list.items) {
                reader->error = NATIVE_BC_ERROR_NO_MEMORY;
                return false;
            }
            out_value->as.list.count = count;
            out_value->type = NATIVE_BC_VALUE_LIST;
            for (uint32_t index = 0; index < count; ++index) {
                if (!read_value(reader, 0, depth + 1, &out_value->as.list.items[index])) {
                    return false;
                }
            }
            return true;
        }
        case VALUE_TAG_INSTRUCTIONS:
            if (field_id != NATIVE_BC_FIELD_INSTRUCTIONS) {
                reader->error = NATIVE_BC_ERROR_MALFORMED_PAYLOAD;
                return false;
            }
            out_value->type = NATIVE_BC_VALUE_INSTRUCTIONS;
            return read_instruction_array(
                reader,
                depth + 1,
                &out_value->as.instructions.items,
                &out_value->as.instructions.count);
        default:
            reader->error = NATIVE_BC_ERROR_MALFORMED_PAYLOAD;
            return false;
    }
}

static bool read_instruction_array(
    bytecode_reader_t *reader,
    uint32_t depth,
    native_bytebeat_instruction_t **out_instructions,
    uint32_t *out_count) {
    uint32_t count;
    if (depth > MAX_NESTING_DEPTH || !read_u32(reader, &count)) {
        reader->error = NATIVE_BC_ERROR_MALFORMED_PAYLOAD;
        return false;
    }
    if (count > (reader->length - reader->position) / 2
        || count > SIZE_MAX / sizeof(native_bytebeat_instruction_t)) {
        reader->error = NATIVE_BC_ERROR_MALFORMED_PAYLOAD;
        return false;
    }

    native_bytebeat_instruction_t *instructions = count
        ? (native_bytebeat_instruction_t *)calloc(count, sizeof(*instructions))
        : NULL;
    if (count && !instructions) {
        reader->error = NATIVE_BC_ERROR_NO_MEMORY;
        return false;
    }
    *out_instructions = instructions;
    *out_count = count;

    for (uint32_t index = 0; index < count; ++index) {
        native_bytebeat_instruction_t *instruction = &instructions[index];
        uint8_t field_count;
        if (!read_u8(reader, &instruction->opcode)
            || instruction->opcode < NATIVE_BC_PUSH_NUMBER
            || instruction->opcode > NATIVE_BC_CONSTRUCT_ARRAY
            || !read_u8(reader, &field_count)) {
            reader->error = NATIVE_BC_ERROR_MALFORMED_PAYLOAD;
            return false;
        }
        instruction->field_count = field_count;
        if (field_count > (reader->length - reader->position) / 2) {
            reader->error = NATIVE_BC_ERROR_MALFORMED_PAYLOAD;
            return false;
        }
        instruction->fields = field_count
            ? (native_bytebeat_field_t *)calloc(field_count, sizeof(*instruction->fields))
            : NULL;
        if (field_count && !instruction->fields) {
            reader->error = NATIVE_BC_ERROR_NO_MEMORY;
            return false;
        }
        for (uint8_t field_index = 0; field_index < field_count; ++field_index) {
            native_bytebeat_field_t *field = &instruction->fields[field_index];
            if (!read_u8(reader, &field->id) || !valid_field_id(field->id)) {
                reader->error = NATIVE_BC_ERROR_MALFORMED_PAYLOAD;
                return false;
            }
            for (uint8_t previous = 0; previous < field_index; ++previous) {
                if (instruction->fields[previous].id == field->id) {
                    reader->error = NATIVE_BC_ERROR_MALFORMED_PAYLOAD;
                    return false;
                }
            }
            if (!read_value(reader, field->id, depth, &field->value)) {
                if (reader->error == NATIVE_BC_ERROR_NONE) {
                    reader->error = NATIVE_BC_ERROR_MALFORMED_PAYLOAD;
                }
                return false;
            }
        }
    }
    return true;
}

native_bytebeat_error_t native_bytebeat_program_load(
    const uint8_t *data,
    size_t length,
    native_bytebeat_program_t **out_program) {
    if (!out_program) return NATIVE_BC_ERROR_INVALID_ARGUMENT;
    *out_program = NULL;
    if (!data || length < BYTECODE_HEADER_SIZE) {
        return NATIVE_BC_ERROR_INVALID_ARGUMENT;
    }
    if (memcmp(data, "BBBC", 4) != 0) return NATIVE_BC_ERROR_INVALID_HEADER;
    if (data[4] != BYTECODE_VERSION) return NATIVE_BC_ERROR_UNSUPPORTED_VERSION;
    if (data[5] > NATIVE_BYTEBEAT_MODE_FUNCBEAT) return NATIVE_BC_ERROR_INVALID_MODE;
    if (data[6] != 0 || data[7] != 0) return NATIVE_BC_ERROR_INVALID_FLAGS;

    bytecode_reader_t header_reader = {
        .data = data + 8,
        .length = BYTECODE_HEADER_SIZE - 8,
        .position = 0,
        .error = NATIVE_BC_ERROR_NONE,
    };
    uint64_t rate_bits;
    uint32_t payload_length;
    uint32_t checksum;
    if (!read_u64(&header_reader, &rate_bits)
        || !read_u32(&header_reader, &payload_length)
        || !read_u32(&header_reader, &checksum)) {
        return NATIVE_BC_ERROR_INVALID_HEADER;
    }
    double sample_rate;
    memcpy(&sample_rate, &rate_bits, sizeof(sample_rate));
    if (!(sample_rate > 0.0) || sample_rate > DBL_MAX) {
        return NATIVE_BC_ERROR_INVALID_HEADER;
    }
    if ((size_t)payload_length != length - BYTECODE_HEADER_SIZE) {
        return NATIVE_BC_ERROR_INVALID_LENGTH;
    }

    const uint8_t *payload = data + BYTECODE_HEADER_SIZE;
    if (crc32_bytes(payload, payload_length) != checksum) {
        return NATIVE_BC_ERROR_CHECKSUM;
    }

    native_bytebeat_program_t *program =
        (native_bytebeat_program_t *)calloc(1, sizeof(*program));
    if (!program) return NATIVE_BC_ERROR_NO_MEMORY;
    program->mode = data[5];
    program->sample_rate = sample_rate;

    bytecode_reader_t reader = {
        .data = payload,
        .length = payload_length,
        .position = 0,
        .error = NATIVE_BC_ERROR_NONE,
    };
    if (!read_instruction_array(
            &reader,
            0,
            &program->instructions,
            &program->instruction_count)
        || reader.position != reader.length) {
        native_bytebeat_error_t error = reader.error == NATIVE_BC_ERROR_NONE
            ? NATIVE_BC_ERROR_MALFORMED_PAYLOAD
            : reader.error;
        native_bytebeat_program_free(program);
        return error;
    }

    *out_program = program;
    return NATIVE_BC_ERROR_NONE;
}

void native_bytebeat_program_free(native_bytebeat_program_t *program) {
    if (!program) return;
    free_instructions(program->instructions, program->instruction_count);
    free(program);
}
