#include "native_bytebeat_vm.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define VM_STACK_CAPACITY 512U
#define VM_INLINE_ARGUMENT_CAPACITY 16U
#define VM_HEAP_COLLECTION_THRESHOLD (32U * 1024U)
#define VM_MAX_ARRAY_LENGTH 65536U
#define VM_LOCAL_CAPACITY 128U
#define VM_GLOBAL_CAPACITY 512U
#define VM_ITERATOR_CAPACITY 32U
#define VM_CALL_DEPTH 8U
#define VM_STEP_LIMIT 100000U
#define UINT32_MODULUS 4294967296.0

typedef enum {
    VM_VALUE_UNDEFINED,
    VM_VALUE_NULL,
    VM_VALUE_BOOLEAN,
    VM_VALUE_NUMBER,
    VM_VALUE_STRING,
    VM_VALUE_THIS,
    VM_VALUE_FUNCTION,
    VM_VALUE_BUILTIN_FUNCTION,
    VM_VALUE_ARRAY,
    VM_VALUE_OBJECT,
    VM_VALUE_ARRAY_SPREAD,
    VM_VALUE_ARRAY_METHOD,
    VM_VALUE_STRING_METHOD
} vm_value_type_t;

typedef struct vm_heap_object vm_heap_object_t;

typedef struct {
    vm_value_type_t type;
    union {
        bool boolean;
        double number;
        struct {
            const char *data;
            uint32_t length;
        } string;
        struct {
            const native_bytebeat_instruction_t *instructions;
            uint32_t instruction_count;
            const native_bytebeat_value_t *parameters;
            uint32_t parameter_count;
            int64_t rest_index;
        } function;
        const char *builtin_function;
        vm_heap_object_t *heap_object;
        struct {
            vm_heap_object_t *receiver;
            const char *name;
        } array_method;
        struct {
            const char *receiver_data;
            uint32_t receiver_length;
            const char *name;
        } string_method;
    } as;
} vm_value_t;

struct vm_heap_object {
    vm_heap_object_t *next;
    size_t allocation_size;
    bool marked;
    vm_value_t *items;
    const char **keys;
    size_t item_count;
    size_t item_capacity;
};

typedef struct {
    const char *name;
    vm_value_t value;
} vm_binding_t;

typedef struct {
    vm_value_t iterable;
    size_t index;
    bool keys;
    bool active;
} vm_iterator_t;

typedef struct {
    vm_value_t stack[VM_STACK_CAPACITY];
    size_t stack_size;
    vm_value_t arguments[VM_INLINE_ARGUMENT_CAPACITY];
    const vm_value_t *active_arguments;
    size_t argument_count;
    vm_value_t active_receiver;
    vm_binding_t locals[VM_LOCAL_CAPACITY];
    size_t local_count;
    vm_iterator_t iterators[VM_ITERATOR_CAPACITY];
} vm_frame_t;

struct native_bytebeat_vm {
    const native_bytebeat_program_t *program;
    vm_binding_t globals[VM_GLOBAL_CAPACITY];
    size_t global_count;
    vm_frame_t frames[VM_CALL_DEPTH];
    vm_value_t funcbeat;
    vm_heap_object_t *heap_objects;
    size_t heap_bytes;
    uint32_t random_state;
    bool initialized;
};

static const native_bytebeat_value_t *find_field(
    const native_bytebeat_instruction_t *instruction,
    uint8_t id) {
    for (uint8_t index = 0; index < instruction->field_count; ++index) {
        if (instruction->fields[index].id == id) {
            return &instruction->fields[index].value;
        }
    }
    return NULL;
}

static bool get_string(
    const native_bytebeat_instruction_t *instruction,
    uint8_t id,
    const char **out_string) {
    const native_bytebeat_value_t *field = find_field(instruction, id);
    if (!field || field->type != NATIVE_BC_VALUE_STRING) return false;
    *out_string = field->as.string.data;
    return true;
}

static bool get_integer(
    const native_bytebeat_instruction_t *instruction,
    uint8_t id,
    int64_t *out_integer) {
    const native_bytebeat_value_t *field = find_field(instruction, id);
    if (!field || field->type != NATIVE_BC_VALUE_INTEGER) return false;
    *out_integer = field->as.integer;
    return true;
}

static bool get_boolean(
    const native_bytebeat_instruction_t *instruction,
    uint8_t id,
    bool *out_boolean) {
    const native_bytebeat_value_t *field = find_field(instruction, id);
    if (!field || field->type != NATIVE_BC_VALUE_BOOLEAN) return false;
    *out_boolean = field->as.boolean;
    return true;
}

static bool string_equals(const char *left, const char *right) {
    return left && right && strcmp(left, right) == 0;
}

static vm_value_t value_undefined(void) {
    vm_value_t value = {.type = VM_VALUE_UNDEFINED};
    return value;
}

static vm_value_t value_number(double number) {
    vm_value_t value = {.type = VM_VALUE_NUMBER};
    value.as.number = number;
    return value;
}

static vm_value_t value_boolean(bool boolean) {
    vm_value_t value = {.type = VM_VALUE_BOOLEAN};
    value.as.boolean = boolean;
    return value;
}

static bool heap_mark_value(vm_value_t value) {
    vm_heap_object_t *object = value.type == VM_VALUE_ARRAY
        || value.type == VM_VALUE_OBJECT
        || value.type == VM_VALUE_ARRAY_SPREAD
        ? value.as.heap_object
        : value.type == VM_VALUE_ARRAY_METHOD
            ? value.as.array_method.receiver
            : NULL;
    if (!object || object->marked) {
        return false;
    }
    object->marked = true;
    return true;
}

static void collect_heap(native_bytebeat_vm_t *vm) {
    bool changed = false;
    for (size_t index = 0; index < vm->global_count; ++index) {
        changed |= heap_mark_value(vm->globals[index].value);
    }
    changed |= heap_mark_value(vm->funcbeat);
    for (size_t frame_index = 0; frame_index < VM_CALL_DEPTH; ++frame_index) {
        const vm_frame_t *frame = &vm->frames[frame_index];
        for (size_t index = 0; index < frame->stack_size; ++index) {
            changed |= heap_mark_value(frame->stack[index]);
        }
        for (size_t index = 0; index < frame->local_count; ++index) {
            changed |= heap_mark_value(frame->locals[index].value);
        }
        for (size_t index = 0; frame->active_arguments
             && index < frame->argument_count; ++index) {
            changed |= heap_mark_value(frame->active_arguments[index]);
        }
        changed |= heap_mark_value(frame->active_receiver);
        for (size_t index = 0; index < VM_ITERATOR_CAPACITY; ++index) {
            if (frame->iterators[index].active) {
                changed |= heap_mark_value(frame->iterators[index].iterable);
            }
        }
    }

    while (changed) {
        changed = false;
        for (vm_heap_object_t *object = vm->heap_objects; object; object = object->next) {
            if (!object->marked) continue;
            for (size_t index = 0; index < object->item_count; ++index) {
                changed |= heap_mark_value(object->items[index]);
            }
        }
    }

    vm_heap_object_t **link = &vm->heap_objects;
    vm->heap_bytes = 0;
    while (*link) {
        vm_heap_object_t *object = *link;
        if (!object->marked) {
            *link = object->next;
            free(object->items);
            free(object->keys);
            free(object);
            continue;
        }
        object->marked = false;
        vm->heap_bytes += object->allocation_size;
        link = &object->next;
    }
}

static vm_heap_object_t *allocate_array(
    native_bytebeat_vm_t *vm,
    size_t length) {
    if (length > (SIZE_MAX - sizeof(vm_heap_object_t)) / sizeof(vm_value_t)) {
        return NULL;
    }
    for (size_t attempt = 0; attempt < 2; ++attempt) {
        if (attempt || vm->heap_bytes >= VM_HEAP_COLLECTION_THRESHOLD) {
            collect_heap(vm);
        }
        vm_heap_object_t *object =
            (vm_heap_object_t *)calloc(1, sizeof(vm_heap_object_t));
        if (!object) continue;
        if (length) {
            object->items = (vm_value_t *)calloc(length, sizeof(vm_value_t));
            if (!object->items) {
                free(object);
                continue;
            }
        }
        object->item_count = length;
        object->item_capacity = length;
        object->allocation_size = sizeof(*object) + length * sizeof(vm_value_t);
        object->next = vm->heap_objects;
        vm->heap_objects = object;
        vm->heap_bytes += object->allocation_size;
        return object;
    }
    return NULL;
}

static bool ensure_array_capacity(
    native_bytebeat_vm_t *vm,
    vm_heap_object_t *array,
    size_t required_capacity) {
    if (required_capacity <= array->item_capacity) return true;
    if (required_capacity > VM_MAX_ARRAY_LENGTH) return false;
    size_t capacity = array->item_capacity ? array->item_capacity : 4;
    while (capacity < required_capacity) {
        capacity = capacity > VM_MAX_ARRAY_LENGTH / 2
            ? VM_MAX_ARRAY_LENGTH
            : capacity * 2;
    }
    if (capacity > (SIZE_MAX - sizeof(*array)) / sizeof(vm_value_t)) {
        return false;
    }
    vm_value_t *items = (vm_value_t *)calloc(capacity, sizeof(*items));
    if (!items) return false;
    if (array->item_count) {
        memcpy(items, array->items, array->item_count * sizeof(*items));
    }
    const size_t old_allocation_size = array->allocation_size;
    free(array->items);
    array->items = items;
    array->item_capacity = capacity;
    array->allocation_size = sizeof(*array) + capacity * sizeof(vm_value_t);
    vm->heap_bytes -= old_allocation_size;
    vm->heap_bytes += array->allocation_size;
    return true;
}

static vm_value_t value_from_bytecode(const native_bytebeat_value_t *source) {
    vm_value_t value = value_undefined();
    if (!source) return value;
    switch (source->type) {
        case NATIVE_BC_VALUE_NULL:
            value.type = VM_VALUE_NULL;
            break;
        case NATIVE_BC_VALUE_BOOLEAN:
            value.type = VM_VALUE_BOOLEAN;
            value.as.boolean = source->as.boolean;
            break;
        case NATIVE_BC_VALUE_INTEGER:
            value = value_number((double)source->as.integer);
            break;
        case NATIVE_BC_VALUE_NUMBER:
            value = value_number(source->as.number);
            break;
        case NATIVE_BC_VALUE_STRING:
            value.type = VM_VALUE_STRING;
            value.as.string.data = source->as.string.data;
            value.as.string.length = source->as.string.length;
            break;
        default:
            break;
    }
    return value;
}

static bool truthy(vm_value_t value) {
    switch (value.type) {
        case VM_VALUE_UNDEFINED:
        case VM_VALUE_NULL:
            return false;
        case VM_VALUE_BOOLEAN:
            return value.as.boolean;
        case VM_VALUE_NUMBER:
            return value.as.number != 0.0 && !isnan(value.as.number);
        case VM_VALUE_STRING:
            return value.as.string.length != 0;
        default:
            return true;
    }
}

static double to_number(vm_value_t value) {
    switch (value.type) {
        case VM_VALUE_UNDEFINED:
            return NAN;
        case VM_VALUE_NULL:
            return 0.0;
        case VM_VALUE_BOOLEAN:
            return value.as.boolean ? 1.0 : 0.0;
        case VM_VALUE_NUMBER:
            return value.as.number;
        case VM_VALUE_ARRAY:
            if (!value.as.heap_object || value.as.heap_object->item_count == 0) {
                return value.as.heap_object ? 0.0 : NAN;
            }
            if (value.as.heap_object->item_count == 1
                && value.as.heap_object->items[0].type != VM_VALUE_ARRAY) {
                return to_number(value.as.heap_object->items[0]);
            }
            return NAN;
        case VM_VALUE_STRING: {
            char bounded_string[128];
            const char *string_data = value.as.string.data;
            if (value.as.string.length < sizeof(bounded_string)) {
                memcpy(
                    bounded_string,
                    value.as.string.data,
                    value.as.string.length);
                bounded_string[value.as.string.length] = '\0';
                string_data = bounded_string;
            } else if (value.as.string.data[value.as.string.length] != '\0') {
                return NAN;
            }
            char *end = NULL;
            const double number = strtod(string_data, &end);
            if (end == string_data) {
                for (uint32_t index = 0; index < value.as.string.length; ++index) {
                    if (string_data[index] != ' '
                        && string_data[index] != '\t'
                        && string_data[index] != '\n'
                        && string_data[index] != '\r') {
                        return NAN;
                    }
                }
                return 0.0;
            }
            while (*end == ' ' || *end == '\t' || *end == '\n' || *end == '\r') ++end;
            return *end == '\0' ? number : NAN;
        }
        default:
            return NAN;
    }
}

static uint32_t to_uint32(vm_value_t value) {
    const double number = to_number(value);
    if (!isfinite(number) || number == 0.0) return 0;
    double reduced = fmod(trunc(number), UINT32_MODULUS);
    if (reduced < 0.0) reduced += UINT32_MODULUS;
    return (uint32_t)reduced;
}

static int32_t to_int32(vm_value_t value) {
    const uint32_t bits = to_uint32(value);
    if (bits < 0x80000000U) return (int32_t)bits;
    return (int32_t)((int64_t)bits - 0x100000000LL);
}

static bool push(vm_frame_t *frame, vm_value_t value) {
    if (frame->stack_size >= VM_STACK_CAPACITY) return false;
    frame->stack[frame->stack_size++] = value;
    return true;
}

static bool pop(vm_frame_t *frame, vm_value_t *out_value) {
    if (frame->stack_size == 0) return false;
    *out_value = frame->stack[--frame->stack_size];
    return true;
}

static vm_binding_t *find_binding(
    vm_binding_t *bindings,
    size_t count,
    const char *name) {
    for (size_t index = 0; index < count; ++index) {
        if (string_equals(bindings[index].name, name)) return &bindings[index];
    }
    return NULL;
}

static native_bytebeat_vm_result_t store_binding(
    vm_binding_t *bindings,
    size_t *count,
    size_t capacity,
    const char *name,
    vm_value_t value) {
    vm_binding_t *binding = find_binding(bindings, *count, name);
    if (binding) {
        binding->value = value;
        return NATIVE_BYTEBEAT_VM_OK;
    }
    if (*count >= capacity) return NATIVE_BYTEBEAT_VM_NO_MEMORY;
    bindings[*count].name = name;
    bindings[*count].value = value;
    ++*count;
    return NATIVE_BYTEBEAT_VM_OK;
}

static vm_value_t load_binding(
    const vm_binding_t *bindings,
    size_t count,
    const char *name) {
    for (size_t index = 0; index < count; ++index) {
        if (string_equals(bindings[index].name, name)) return bindings[index].value;
    }
    return value_undefined();
}

static bool strict_equal(vm_value_t left, vm_value_t right) {
    if (left.type != right.type) return false;
    switch (left.type) {
        case VM_VALUE_UNDEFINED:
        case VM_VALUE_NULL:
            return true;
        case VM_VALUE_BOOLEAN:
            return left.as.boolean == right.as.boolean;
        case VM_VALUE_NUMBER:
            return left.as.number == right.as.number;
        case VM_VALUE_STRING:
            return left.as.string.length == right.as.string.length
                && memcmp(
                    left.as.string.data,
                    right.as.string.data,
                    left.as.string.length) == 0;
        case VM_VALUE_ARRAY:
        case VM_VALUE_OBJECT:
            return left.as.heap_object == right.as.heap_object;
        default:
            return false;
    }
}

static native_bytebeat_vm_result_t binary_operation(
    const char *operator_name,
    vm_value_t left,
    vm_value_t right,
    vm_value_t *out_value) {
    const double a = to_number(left);
    const double b = to_number(right);
    if (string_equals(operator_name, "+")) {
        if (left.type == VM_VALUE_STRING || right.type == VM_VALUE_STRING) {
            return NATIVE_BYTEBEAT_VM_UNSUPPORTED_OPERATION;
        }
        *out_value = value_number(a + b);
    } else if (string_equals(operator_name, "-")) {
        *out_value = value_number(a - b);
    } else if (string_equals(operator_name, "*")) {
        *out_value = value_number(a * b);
    } else if (string_equals(operator_name, "/")) {
        *out_value = value_number(a / b);
    } else if (string_equals(operator_name, "%")) {
        *out_value = value_number(fmod(a, b));
    } else if (string_equals(operator_name, "**")) {
        *out_value = value_number(pow(a, b));
    } else if (string_equals(operator_name, "&")) {
        *out_value = value_number(to_int32(left) & to_int32(right));
    } else if (string_equals(operator_name, "|")) {
        *out_value = value_number(to_int32(left) | to_int32(right));
    } else if (string_equals(operator_name, "^")) {
        *out_value = value_number(to_int32(left) ^ to_int32(right));
    } else if (string_equals(operator_name, "<<")) {
        const uint32_t bits = (uint32_t)to_int32(left) & 0xFFFFFFFFU;
        *out_value = value_number(to_int32(value_number(
            (int32_t)(bits << (to_uint32(right) & 31U)))));
    } else if (string_equals(operator_name, ">>")) {
        const int32_t integer = to_int32(left);
        const uint32_t shift = to_uint32(right) & 31U;
        int32_t result = integer >> shift;
        if (integer < 0 && shift != 0) {
            result = (int32_t)(((uint32_t)integer >> shift)
                | (0xFFFFFFFFU << (32U - shift)));
        }
        *out_value = value_number(result);
    } else if (string_equals(operator_name, ">>>")) {
        *out_value = value_number(to_uint32(left) >> (to_uint32(right) & 31U));
    } else if (string_equals(operator_name, "===") || string_equals(operator_name, "==")) {
        *out_value = value_boolean(
            string_equals(operator_name, "===")
                ? strict_equal(left, right)
                : (left.type == VM_VALUE_STRING && right.type == VM_VALUE_STRING
                    ? strict_equal(left, right)
                    : a == b));
    } else if (string_equals(operator_name, "!==") || string_equals(operator_name, "!=")) {
        *out_value = value_boolean(
            string_equals(operator_name, "!==")
                ? !strict_equal(left, right)
                : (left.type == VM_VALUE_STRING && right.type == VM_VALUE_STRING
                    ? !strict_equal(left, right)
                    : a != b));
    } else if (string_equals(operator_name, "<")
        || string_equals(operator_name, "<=")
        || string_equals(operator_name, ">")
        || string_equals(operator_name, ">=")) {
        bool result;
        if (left.type == VM_VALUE_STRING && right.type == VM_VALUE_STRING) {
            const int order = strcmp(left.as.string.data, right.as.string.data);
            result = string_equals(operator_name, "<") ? order < 0
                : string_equals(operator_name, "<=") ? order <= 0
                : string_equals(operator_name, ">") ? order > 0
                : order >= 0;
        } else {
            result = string_equals(operator_name, "<") ? a < b
                : string_equals(operator_name, "<=") ? a <= b
                : string_equals(operator_name, ">") ? a > b
                : a >= b;
        }
        *out_value = value_boolean(result);
    } else {
        return NATIVE_BYTEBEAT_VM_UNSUPPORTED_OPERATION;
    }
    return NATIVE_BYTEBEAT_VM_OK;
}

static native_bytebeat_vm_result_t unary_operation(
    const char *operator_name,
    vm_value_t operand,
    vm_value_t *out_value) {
    if (string_equals(operator_name, "+")) {
        *out_value = value_number(to_number(operand));
    } else if (string_equals(operator_name, "-")) {
        *out_value = value_number(-to_number(operand));
    } else if (string_equals(operator_name, "!")) {
        *out_value = value_boolean(!truthy(operand));
    } else if (string_equals(operator_name, "~")) {
        *out_value = value_number(~to_int32(operand));
    } else if (string_equals(operator_name, "typeof")) {
        static const char *const type_names[] = {
            "undefined", "object", "boolean", "number", "string", "object",
            "function", "function", "object", "object", "object", "function",
            "function"
        };
        out_value->type = VM_VALUE_STRING;
        out_value->as.string.data = type_names[operand.type];
        out_value->as.string.length = (uint32_t)strlen(type_names[operand.type]);
    } else {
        return NATIVE_BYTEBEAT_VM_UNSUPPORTED_OPERATION;
    }
    return NATIVE_BYTEBEAT_VM_OK;
}

static double math_constant(const char *name) {
    if (string_equals(name, "PI")) return 3.14159265358979323846;
    if (string_equals(name, "E")) return 2.71828182845904523536;
    if (string_equals(name, "LN2")) return 0.69314718055994530942;
    if (string_equals(name, "LN10")) return 2.30258509299404568402;
    if (string_equals(name, "LOG2E")) return 1.44269504088896340736;
    if (string_equals(name, "LOG10E")) return 0.43429448190325182765;
    if (string_equals(name, "SQRT1_2")) return 0.70710678118654752440;
    if (string_equals(name, "SQRT2")) return 1.41421356237309504880;
    return NAN;
}

static int parse_int_digit(char character) {
    if (character >= '0' && character <= '9') return character - '0';
    if (character >= 'a' && character <= 'z') return character - 'a' + 10;
    if (character >= 'A' && character <= 'Z') return character - 'A' + 10;
    return -1;
}

static double parse_int_value(
    const vm_value_t *arguments,
    size_t count) {
    char buffer[64];
    const char *text = NULL;
    size_t length = 0;
    if (!count) {
        text = "undefined";
        length = sizeof("undefined") - 1;
    } else if (arguments[0].type == VM_VALUE_STRING) {
        text = arguments[0].as.string.data;
        length = arguments[0].as.string.length;
    } else if (arguments[0].type == VM_VALUE_BOOLEAN) {
        text = arguments[0].as.boolean ? "true" : "false";
        length = arguments[0].as.boolean ? sizeof("true") - 1 : sizeof("false") - 1;
    } else if (arguments[0].type == VM_VALUE_NULL) {
        text = "null";
        length = sizeof("null") - 1;
    } else if (arguments[0].type == VM_VALUE_NUMBER
        && isfinite(arguments[0].as.number)) {
        const int written = snprintf(
            buffer,
            sizeof(buffer),
            "%.17g",
            arguments[0].as.number);
        if (written < 0 || (size_t)written >= sizeof(buffer)) return NAN;
        text = buffer;
        length = (size_t)written;
    } else {
        return NAN;
    }

    size_t index = 0;
    while (index < length) {
        const unsigned char character = (unsigned char)text[index];
        if (character == ' ' || (character >= '\t' && character <= '\r')) {
            ++index;
        } else {
            break;
        }
    }
    bool negative = false;
    if (index < length && (text[index] == '+' || text[index] == '-')) {
        negative = text[index] == '-';
        ++index;
    }

    int radix = count > 1 ? to_int32(arguments[1]) : 0;
    if (radix != 0 && (radix < 2 || radix > 36)) return NAN;
    const bool has_hex_prefix =
        index + 1 < length
        && text[index] == '0'
        && (text[index + 1] == 'x' || text[index + 1] == 'X');
    if ((radix == 0 || radix == 16) && has_hex_prefix) {
        index += 2;
        radix = 16;
    } else if (radix == 0) {
        radix = 10;
    }

    double result = 0.0;
    size_t digit_count = 0;
    while (index < length) {
        const int digit = parse_int_digit(text[index]);
        if (digit < 0 || digit >= radix) break;
        result = result * (double)radix + (double)digit;
        ++index;
        ++digit_count;
    }
    if (!digit_count) return NAN;
    return negative ? -result : result;
}

static native_bytebeat_vm_result_t call_builtin(
    native_bytebeat_vm_t *vm,
    const char *name,
    const vm_value_t *arguments,
    size_t count,
    vm_value_t *out_value) {
    const char *function_name = name;
    if (strncmp(name, "Math.", 5) == 0) function_name += 5;
    const double first = count ? to_number(arguments[0]) : NAN;
    const double second = count > 1 ? to_number(arguments[1]) : NAN;
    double result = NAN;

    if (string_equals(function_name, "parseInt")) {
        result = parse_int_value(arguments, count);
    } else if (string_equals(function_name, "isFinite")) {
        *out_value = value_boolean(isfinite(first));
        return NATIVE_BYTEBEAT_VM_OK;
    } else if (string_equals(function_name, "isNaN")) {
        *out_value = value_boolean(isnan(first));
        return NATIVE_BYTEBEAT_VM_OK;
    } else if (string_equals(function_name, "abs")) result = fabs(first);
    else if (string_equals(function_name, "acos")) result = acos(first);
    else if (string_equals(function_name, "acosh")) result = acosh(first);
    else if (string_equals(function_name, "asin")) result = asin(first);
    else if (string_equals(function_name, "asinh")) result = asinh(first);
    else if (string_equals(function_name, "atan")) result = atan(first);
    else if (string_equals(function_name, "atan2")) result = atan2(first, second);
    else if (string_equals(function_name, "atanh")) result = atanh(first);
    else if (string_equals(function_name, "cbrt")) result = cbrt(first);
    else if (string_equals(function_name, "ceil")) result = ceil(first);
    else if (string_equals(function_name, "cos")) result = cos(first);
    else if (string_equals(function_name, "cosh")) result = cosh(first);
    else if (string_equals(function_name, "exp")) result = exp(first);
    else if (string_equals(function_name, "expm1")) result = expm1(first);
    else if (string_equals(function_name, "floor")) result = floor(first);
    else if (string_equals(function_name, "fround")) result = (double)(float)first;
    else if (string_equals(function_name, "hypot")) {
        result = 0.0;
        for (size_t index = 0; index < count; ++index) {
            result = hypot(result, to_number(arguments[index]));
        }
    } else if (string_equals(function_name, "log")) result = log(first);
    else if (string_equals(function_name, "log10")) result = log10(first);
    else if (string_equals(function_name, "log1p")) result = log1p(first);
    else if (string_equals(function_name, "log2")) result = log2(first);
    else if (string_equals(function_name, "max")) {
        result = count ? -INFINITY : -INFINITY;
        for (size_t index = 0; index < count; ++index) {
            const double value = to_number(arguments[index]);
            if (isnan(value)) { result = NAN; break; }
            if (value > result) result = value;
        }
    } else if (string_equals(function_name, "min")) {
        result = count ? INFINITY : INFINITY;
        for (size_t index = 0; index < count; ++index) {
            const double value = to_number(arguments[index]);
            if (isnan(value)) { result = NAN; break; }
            if (value < result) result = value;
        }
    } else if (string_equals(function_name, "pow")) result = pow(first, second);
    else if (string_equals(function_name, "random")) {
        vm->random_state = vm->random_state * 1664525U + 1013904223U;
        result = (double)vm->random_state / 4294967296.0;
    } else if (string_equals(function_name, "round")) result = floor(first + 0.5);
    else if (string_equals(function_name, "sign")) result = first > 0.0 ? 1.0 : first < 0.0 ? -1.0 : first;
    else if (string_equals(function_name, "sin")) result = sin(first);
    else if (string_equals(function_name, "sinh")) result = sinh(first);
    else if (string_equals(function_name, "sqrt")) result = sqrt(first);
    else if (string_equals(function_name, "tan")) result = tan(first);
    else if (string_equals(function_name, "tanh")) result = tanh(first);
    else if (string_equals(function_name, "trunc")) result = trunc(first);
    else if (string_equals(function_name, "int")) result = floor(first);
    else return NATIVE_BYTEBEAT_VM_UNSUPPORTED_OPERATION;

    *out_value = value_number(result);
    return NATIVE_BYTEBEAT_VM_OK;
}

static native_bytebeat_vm_result_t run_instructions(
    native_bytebeat_vm_t *vm,
    const native_bytebeat_instruction_t *instructions,
    uint32_t instruction_count,
    double t,
    double sample_rate,
    size_t depth,
    vm_value_t *out_value);

static bool builtin_supported(const char *name) {
    static const char *const names[] = {
        "abs", "acos", "acosh", "asin", "asinh", "atan", "atan2", "atanh",
        "cbrt", "ceil", "cos", "cosh", "exp", "expm1", "floor", "fround",
        "hypot", "int", "isFinite", "isNaN", "log", "log10", "log1p", "log2", "max", "min",
        "parseInt", "pow", "random", "round", "sign", "sin", "sinh", "sqrt", "tan",
        "tanh", "trunc",
    };
    const char *candidate = strncmp(name, "Math.", 5) == 0 ? name + 5 : name;
    for (size_t index = 0; index < sizeof(names) / sizeof(names[0]); ++index) {
        if (string_equals(candidate, names[index])) return true;
    }
    return false;
}

static bool bytecode_supported(
    const native_bytebeat_instruction_t *instructions,
    uint32_t instruction_count,
    size_t depth) {
    if (!instructions || depth >= VM_CALL_DEPTH) return false;
    for (uint32_t index = 0; index < instruction_count; ++index) {
        const native_bytebeat_instruction_t *instruction = &instructions[index];
        int64_t target;
        const char *name = NULL;
        switch (instruction->opcode) {
            case NATIVE_BC_PUSH_NUMBER:
            case NATIVE_BC_PUSH_STRING:
            case NATIVE_BC_PUSH_BOOLEAN:
            case NATIVE_BC_PUSH_NULL:
            case NATIVE_BC_PUSH_UNDEFINED:
            case NATIVE_BC_LOAD_THIS:
            case NATIVE_BC_LOAD_T:
            case NATIVE_BC_LOAD_SAMPLE_RATE:
            case NATIVE_BC_LOAD_LOCAL:
            case NATIVE_BC_LOAD_GLOBAL:
            case NATIVE_BC_POP:
            case NATIVE_BC_DUP:
            case NATIVE_BC_DUP2:
            case NATIVE_BC_STORE_LOCAL:
            case NATIVE_BC_STORE_GLOBAL:
            case NATIVE_BC_GET_INDEX:
            case NATIVE_BC_GET_PROPERTY:
            case NATIVE_BC_GET_PROPERTY_KEEP_REF:
            case NATIVE_BC_SET_PROPERTY:
            case NATIVE_BC_KEEP_VALUE_DROP_REF:
            case NATIVE_BC_UPDATE_PROPERTY:
            case NATIVE_BC_UPDATE_LOCAL:
            case NATIVE_BC_UPDATE_GLOBAL:
            case NATIVE_BC_CALL_VALUE:
            case NATIVE_BC_RETURN:
                break;
            case NATIVE_BC_CONSTRUCT_ARRAY:
                if (!get_string(
                        instruction,
                        NATIVE_BC_FIELD_CONSTRUCTOR,
                        &name)
                    || !string_equals(name, "Array")
                    || !get_integer(
                        instruction,
                        NATIVE_BC_FIELD_ARGUMENT_COUNT,
                        &target)
                    || target < 0 || target > 1) {
                    return false;
                }
                break;
            case NATIVE_BC_MAKE_ARRAY: {
                int64_t length;
                const native_bytebeat_value_t *spread_flags =
                    find_field(instruction, NATIVE_BC_FIELD_SPREAD_FLAGS);
                if (!get_integer(instruction, NATIVE_BC_FIELD_LENGTH, &length)
                    || length < 0 || length > VM_STACK_CAPACITY
                    || !spread_flags
                    || spread_flags->type != NATIVE_BC_VALUE_LIST
                    || spread_flags->as.list.count != (uint32_t)length) {
                    return false;
                }
                for (uint32_t flag_index = 0;
                     flag_index < spread_flags->as.list.count;
                     ++flag_index) {
                    const native_bytebeat_value_t *flag =
                        &spread_flags->as.list.items[flag_index];
                    if (flag->type != NATIVE_BC_VALUE_BOOLEAN) {
                        return false;
                    }
                }
                break;
            }
            case NATIVE_BC_SPREAD_ARRAY:
                break;
            case NATIVE_BC_MAKE_OBJECT: {
                const native_bytebeat_value_t *keys =
                    find_field(instruction, NATIVE_BC_FIELD_KEYS);
                if (!keys || keys->type != NATIVE_BC_VALUE_LIST
                    || keys->as.list.count > VM_STACK_CAPACITY) {
                    return false;
                }
                for (uint32_t key_index = 0;
                     key_index < keys->as.list.count;
                     ++key_index) {
                    if (keys->as.list.items[key_index].type
                        != NATIVE_BC_VALUE_STRING) {
                        return false;
                    }
                }
                break;
            }
            case NATIVE_BC_ITER_BEGIN:
                {
                    bool keys;
                    if (!get_integer(
                        instruction,
                        NATIVE_BC_FIELD_ITERATOR,
                        &target)
                    || target < 0 || target >= VM_ITERATOR_CAPACITY
                    || !get_boolean(
                        instruction,
                        NATIVE_BC_FIELD_KEYS,
                        &keys)) {
                        return false;
                    }
                }
                break;
            case NATIVE_BC_ITER_NEXT:
                if (!get_integer(
                        instruction,
                        NATIVE_BC_FIELD_ITERATOR,
                        &target)
                    || target < 0 || target >= VM_ITERATOR_CAPACITY
                    || !get_string(
                        instruction,
                        NATIVE_BC_FIELD_NAME,
                        &name)) {
                    return false;
                }
                {
                    bool local;
                    if (!get_boolean(instruction, NATIVE_BC_FIELD_LOCAL, &local)) {
                        return false;
                    }
                }
                break;
            case NATIVE_BC_ITER_END:
                if (!get_integer(
                        instruction,
                        NATIVE_BC_FIELD_ITERATOR,
                        &target)
                    || target < 0 || target >= VM_ITERATOR_CAPACITY) {
                    return false;
                }
                break;
            case NATIVE_BC_BINARY:
                if (!get_string(instruction, NATIVE_BC_FIELD_OPERATOR, &name)
                    || !(string_equals(name, "+") || string_equals(name, "-")
                        || string_equals(name, "*") || string_equals(name, "/")
                        || string_equals(name, "%") || string_equals(name, "**")
                        || string_equals(name, "&") || string_equals(name, "|")
                        || string_equals(name, "^") || string_equals(name, "<<")
                        || string_equals(name, ">>") || string_equals(name, ">>>")
                        || string_equals(name, "==") || string_equals(name, "===")
                        || string_equals(name, "!=") || string_equals(name, "!==")
                        || string_equals(name, "<") || string_equals(name, "<=")
                        || string_equals(name, ">") || string_equals(name, ">="))) {
                    return false;
                }
                break;
            case NATIVE_BC_UNARY:
                if (!get_string(instruction, NATIVE_BC_FIELD_OPERATOR, &name)
                    || !(string_equals(name, "+") || string_equals(name, "-")
                        || string_equals(name, "!") || string_equals(name, "~")
                        || string_equals(name, "typeof"))) {
                    return false;
                }
                break;
            case NATIVE_BC_PUSH_MATH_CONSTANT:
                if (!get_string(instruction, NATIVE_BC_FIELD_NAME, &name)
                    || !(string_equals(name, "PI") || string_equals(name, "E")
                        || string_equals(name, "LN2") || string_equals(name, "LN10")
                        || string_equals(name, "LOG2E") || string_equals(name, "LOG10E")
                        || string_equals(name, "SQRT1_2") || string_equals(name, "SQRT2"))) {
                    return false;
                }
                break;
            case NATIVE_BC_CALL:
                if (!get_string(instruction, NATIVE_BC_FIELD_NAME, &name)
                    || !builtin_supported(name)) {
                    return false;
                }
                break;
            case NATIVE_BC_MAKE_FUNCTION: {
                const native_bytebeat_value_t *code =
                    find_field(instruction, NATIVE_BC_FIELD_INSTRUCTIONS);
                const native_bytebeat_value_t *rest =
                    find_field(instruction, NATIVE_BC_FIELD_REST_INDEX);
                if (!code || code->type != NATIVE_BC_VALUE_INSTRUCTIONS
                    || (rest && rest->type == NATIVE_BC_VALUE_INTEGER
                        && rest->as.integer >= 0)
                    || !bytecode_supported(
                        code->as.instructions.items,
                        code->as.instructions.count,
                        depth + 1)) {
                    return false;
                }
                break;
            }
            case NATIVE_BC_JUMP:
            case NATIVE_BC_JUMP_IF_FALSE_POP:
            case NATIVE_BC_JUMP_IF_FALSE:
            case NATIVE_BC_JUMP_IF_TRUE:
            case NATIVE_BC_JUMP_IF_NULLISH:
            case NATIVE_BC_JUMP_IF_NOT_NULLISH:
            case NATIVE_BC_JUMP_IF_UNDEFINED:
                if (!get_integer(instruction, NATIVE_BC_FIELD_TARGET, &target)
                    || target < 0 || (uint64_t)target >= instruction_count) {
                    return false;
                }
                break;
            default:
                return false;
        }
    }
    return true;
}

static native_bytebeat_vm_result_t call_function(
    native_bytebeat_vm_t *vm,
    vm_value_t function,
    const vm_value_t *arguments,
    size_t argument_count,
    double t,
    double sample_rate,
    size_t depth,
    vm_value_t *out_value) {
    if (function.type != VM_VALUE_FUNCTION || depth + 1 >= VM_CALL_DEPTH) {
        return NATIVE_BYTEBEAT_VM_RUNTIME_ERROR;
    }
    if (function.as.function.rest_index >= 0) {
        return NATIVE_BYTEBEAT_VM_UNSUPPORTED_OPERATION;
    }
    vm_frame_t *frame = &vm->frames[depth + 1];
    frame->stack_size = 0;
    frame->local_count = 0;
    for (uint32_t index = 0; index < function.as.function.parameter_count; ++index) {
        const native_bytebeat_value_t *parameter =
            &function.as.function.parameters[index];
        if (parameter->type != NATIVE_BC_VALUE_STRING) {
            return NATIVE_BYTEBEAT_VM_RUNTIME_ERROR;
        }
        vm_value_t value = index < argument_count
            ? arguments[index]
            : value_undefined();
        native_bytebeat_vm_result_t stored = store_binding(
            frame->locals,
            &frame->local_count,
            VM_LOCAL_CAPACITY,
            parameter->as.string.data,
            value);
        if (stored != NATIVE_BYTEBEAT_VM_OK) return stored;
    }
    return run_instructions(
        vm,
        function.as.function.instructions,
        function.as.function.instruction_count,
        t,
        sample_rate,
        depth + 1,
        out_value);
}

static size_t normalize_array_index(
    vm_value_t value,
    size_t length,
    size_t default_value) {
    if (value.type == VM_VALUE_UNDEFINED) return default_value;
    const double number = to_number(value);
    if (isnan(number)) return 0;
    if (number >= (double)length) return length;
    if (number < 0.0) {
        const double from_end = (double)length + trunc(number);
        return from_end <= 0.0 ? 0 : (size_t)from_end;
    }
    return (size_t)trunc(number);
}

static native_bytebeat_vm_result_t call_array_method(
    native_bytebeat_vm_t *vm,
    vm_value_t method,
    const vm_value_t *arguments,
    size_t argument_count,
    double t,
    double sample_rate,
    size_t depth,
    vm_value_t *out_value) {
    vm_heap_object_t *receiver = method.as.array_method.receiver;
    const char *name = method.as.array_method.name;
    if (!receiver || !name) return NATIVE_BYTEBEAT_VM_RUNTIME_ERROR;

    if (string_equals(name, "fill")) {
        const vm_value_t fill_value = argument_count
            ? arguments[0]
            : value_undefined();
        const size_t start = normalize_array_index(
            argument_count > 1 ? arguments[1] : value_undefined(),
            receiver->item_count,
            0);
        const size_t end = normalize_array_index(
            argument_count > 2 ? arguments[2] : value_undefined(),
            receiver->item_count,
            receiver->item_count);
        for (size_t index = start; index < end; ++index) {
            receiver->items[index] = fill_value;
        }
        *out_value = value_undefined();
        out_value->type = VM_VALUE_ARRAY;
        out_value->as.heap_object = receiver;
        return NATIVE_BYTEBEAT_VM_OK;
    }

    if (string_equals(name, "push")) {
        if (argument_count > VM_MAX_ARRAY_LENGTH - receiver->item_count
            || !ensure_array_capacity(
                vm,
                receiver,
                receiver->item_count + argument_count)) {
            return NATIVE_BYTEBEAT_VM_NO_MEMORY;
        }
        for (size_t index = 0; index < argument_count; ++index) {
            receiver->items[receiver->item_count++] = arguments[index];
        }
        *out_value = value_number((double)receiver->item_count);
        return NATIVE_BYTEBEAT_VM_OK;
    }

    if (string_equals(name, "pop")) {
        *out_value = receiver->item_count
            ? receiver->items[--receiver->item_count]
            : value_undefined();
        if (receiver->item_capacity > receiver->item_count) {
            receiver->items[receiver->item_count] = value_undefined();
        }
        return NATIVE_BYTEBEAT_VM_OK;
    }

    if (string_equals(name, "shift")) {
        *out_value = receiver->item_count
            ? receiver->items[0]
            : value_undefined();
        if (receiver->item_count) {
            --receiver->item_count;
            memmove(
                receiver->items,
                receiver->items + 1,
                receiver->item_count * sizeof(*receiver->items));
            receiver->items[receiver->item_count] = value_undefined();
        }
        return NATIVE_BYTEBEAT_VM_OK;
    }

    if (string_equals(name, "unshift")) {
        if (argument_count > VM_MAX_ARRAY_LENGTH - receiver->item_count
            || !ensure_array_capacity(
                vm,
                receiver,
                receiver->item_count + argument_count)) {
            return NATIVE_BYTEBEAT_VM_NO_MEMORY;
        }
        if (receiver->item_count) {
            memmove(
                receiver->items + argument_count,
                receiver->items,
                receiver->item_count * sizeof(*receiver->items));
        }
        if (argument_count) {
            memcpy(
                receiver->items,
                arguments,
                argument_count * sizeof(*arguments));
        }
        receiver->item_count += argument_count;
        *out_value = value_number((double)receiver->item_count);
        return NATIVE_BYTEBEAT_VM_OK;
    }

    if (string_equals(name, "reverse")) {
        for (size_t index = 0; index < receiver->item_count / 2; ++index) {
            const size_t opposite = receiver->item_count - index - 1;
            const vm_value_t value = receiver->items[index];
            receiver->items[index] = receiver->items[opposite];
            receiver->items[opposite] = value;
        }
        out_value->type = VM_VALUE_ARRAY;
        out_value->as.heap_object = receiver;
        return NATIVE_BYTEBEAT_VM_OK;
    }

    if (string_equals(name, "slice")) {
        const size_t start = normalize_array_index(
            argument_count ? arguments[0] : value_undefined(),
            receiver->item_count,
            0);
        const size_t end = normalize_array_index(
            argument_count > 1 ? arguments[1] : value_undefined(),
            receiver->item_count,
            receiver->item_count);
        const size_t length = end > start ? end - start : 0;
        vm_heap_object_t *copy = allocate_array(vm, length);
        if (!copy) return NATIVE_BYTEBEAT_VM_NO_MEMORY;
        if (length) {
            memcpy(copy->items, receiver->items + start, length * sizeof(*copy->items));
        }
        out_value->type = VM_VALUE_ARRAY;
        out_value->as.heap_object = copy;
        return NATIVE_BYTEBEAT_VM_OK;
    }

    if (string_equals(name, "includes") || string_equals(name, "indexOf")) {
        const vm_value_t sought = argument_count
            ? arguments[0]
            : value_undefined();
        const size_t start = normalize_array_index(
            argument_count > 1 ? arguments[1] : value_undefined(),
            receiver->item_count,
            0);
        for (size_t index = start; index < receiver->item_count; ++index) {
            const vm_value_t item = receiver->items[index];
            const bool equal = strict_equal(item, sought)
                || (string_equals(name, "includes")
                    && item.type == VM_VALUE_NUMBER
                    && sought.type == VM_VALUE_NUMBER
                    && isnan(item.as.number)
                    && isnan(sought.as.number));
            if (equal) {
                *out_value = string_equals(name, "includes")
                    ? value_boolean(true)
                    : value_number((double)index);
                return NATIVE_BYTEBEAT_VM_OK;
            }
        }
        *out_value = string_equals(name, "includes")
            ? value_boolean(false)
            : value_number(-1.0);
        return NATIVE_BYTEBEAT_VM_OK;
    }

    if (string_equals(name, "map") || string_equals(name, "forEach")
        || string_equals(name, "filter") || string_equals(name, "find")
        || string_equals(name, "findIndex") || string_equals(name, "some")
        || string_equals(name, "every")) {
        if (!argument_count || arguments[0].type != VM_VALUE_FUNCTION) {
            return NATIVE_BYTEBEAT_VM_RUNTIME_ERROR;
        }
        vm_value_t mapped = value_undefined();
        if (string_equals(name, "map") || string_equals(name, "filter")) {
            vm_heap_object_t *output = allocate_array(vm, receiver->item_count);
            if (!output) return NATIVE_BYTEBEAT_VM_NO_MEMORY;
            mapped.type = VM_VALUE_ARRAY;
            mapped.as.heap_object = output;
            if (string_equals(name, "filter")) output->item_count = 0;
            if (!push(&vm->frames[depth], mapped)) {
                return NATIVE_BYTEBEAT_VM_STACK_ERROR;
            }
        }
        for (size_t index = 0; index < receiver->item_count; ++index) {
            const vm_value_t callback_arguments[] = {
                receiver->items[index],
                value_number((double)index),
                {
                    .type = VM_VALUE_ARRAY,
                    .as.heap_object = receiver,
                },
            };
            vm_value_t callback_result = value_undefined();
            native_bytebeat_vm_result_t status = call_function(
                vm,
                arguments[0],
                callback_arguments,
                sizeof(callback_arguments) / sizeof(callback_arguments[0]),
                t,
                sample_rate,
                depth,
                &callback_result);
            if (status != NATIVE_BYTEBEAT_VM_OK) {
                if (string_equals(name, "map") || string_equals(name, "filter")) {
                    --vm->frames[depth].stack_size;
                }
                return status;
            }
            if (string_equals(name, "map")) {
                mapped.as.heap_object->items[index] = callback_result;
            } else if (string_equals(name, "filter")
                && truthy(callback_result)) {
                mapped.as.heap_object->items[mapped.as.heap_object->item_count++] =
                    receiver->items[index];
            } else if (string_equals(name, "find") && truthy(callback_result)) {
                *out_value = receiver->items[index];
                return NATIVE_BYTEBEAT_VM_OK;
            } else if (string_equals(name, "findIndex")
                && truthy(callback_result)) {
                *out_value = value_number((double)index);
                return NATIVE_BYTEBEAT_VM_OK;
            } else if (string_equals(name, "some") && truthy(callback_result)) {
                *out_value = value_boolean(true);
                return NATIVE_BYTEBEAT_VM_OK;
            } else if (string_equals(name, "every") && !truthy(callback_result)) {
                *out_value = value_boolean(false);
                return NATIVE_BYTEBEAT_VM_OK;
            }
        }
        if (string_equals(name, "map") || string_equals(name, "filter")) {
            if (string_equals(name, "filter")) {
                mapped.as.heap_object->allocation_size =
                    sizeof(*mapped.as.heap_object)
                    + mapped.as.heap_object->item_capacity * sizeof(vm_value_t);
            }
            --vm->frames[depth].stack_size;
            *out_value = mapped;
        } else if (string_equals(name, "find")) {
            *out_value = value_undefined();
        } else if (string_equals(name, "findIndex")) {
            *out_value = value_number(-1.0);
        } else if (string_equals(name, "some")) {
            *out_value = value_boolean(false);
        } else if (string_equals(name, "every")) {
            *out_value = value_boolean(true);
        } else {
            *out_value = value_undefined();
        }
        return NATIVE_BYTEBEAT_VM_OK;
    }

    if (string_equals(name, "reduce")) {
        if (!argument_count || arguments[0].type != VM_VALUE_FUNCTION) {
            return NATIVE_BYTEBEAT_VM_RUNTIME_ERROR;
        }
        size_t index = 0;
        vm_value_t accumulator;
        if (argument_count > 1) {
            accumulator = arguments[1];
        } else {
            if (!receiver->item_count) return NATIVE_BYTEBEAT_VM_RUNTIME_ERROR;
            accumulator = receiver->items[0];
            index = 1;
        }
        for (; index < receiver->item_count; ++index) {
            const vm_value_t callback_arguments[] = {
                accumulator,
                receiver->items[index],
                value_number((double)index),
                {
                    .type = VM_VALUE_ARRAY,
                    .as.heap_object = receiver,
                },
            };
            native_bytebeat_vm_result_t status = call_function(
                vm,
                arguments[0],
                callback_arguments,
                sizeof(callback_arguments) / sizeof(callback_arguments[0]),
                t,
                sample_rate,
                depth,
                &accumulator);
            if (status != NATIVE_BYTEBEAT_VM_OK) return status;
        }
        *out_value = accumulator;
        return NATIVE_BYTEBEAT_VM_OK;
    }

    return NATIVE_BYTEBEAT_VM_UNSUPPORTED_OPERATION;
}

static bool property_index(vm_value_t key, size_t *out_index) {
    if (key.type == VM_VALUE_NUMBER) {
        const double index = key.as.number;
        if (!isfinite(index) || index < 0.0 || trunc(index) != index
            || index >= (double)SIZE_MAX) {
            return false;
        }
        *out_index = (size_t)index;
        return true;
    }
    if (key.type != VM_VALUE_STRING || key.as.string.length == 0
        || (key.as.string.length > 1 && key.as.string.data[0] == '0')) {
        return false;
    }
    size_t index = 0;
    for (uint32_t offset = 0; offset < key.as.string.length; ++offset) {
        const char digit = key.as.string.data[offset];
        if (digit < '0' || digit > '9'
            || index > (SIZE_MAX - (size_t)(digit - '0')) / 10U) {
            return false;
        }
        index = index * 10U + (size_t)(digit - '0');
    }
    *out_index = index;
    return true;
}

static native_bytebeat_vm_result_t get_property(
    native_bytebeat_vm_t *vm,
    vm_value_t target,
    vm_value_t key,
    vm_value_t *out_value) {
    if (target.type == VM_VALUE_THIS && key.type == VM_VALUE_STRING) {
        *out_value = load_binding(
            vm->globals,
            vm->global_count,
            key.as.string.data);
        return NATIVE_BYTEBEAT_VM_OK;
    }
    if (target.type == VM_VALUE_ARRAY && target.as.heap_object) {
        if (key.type == VM_VALUE_STRING
            && string_equals(key.as.string.data, "length")) {
            *out_value = value_number((double)target.as.heap_object->item_count);
            return NATIVE_BYTEBEAT_VM_OK;
        }
        if (key.type == VM_VALUE_STRING
            && (string_equals(key.as.string.data, "fill")
                || string_equals(key.as.string.data, "map")
                || string_equals(key.as.string.data, "forEach")
                || string_equals(key.as.string.data, "filter")
                || string_equals(key.as.string.data, "find")
                || string_equals(key.as.string.data, "findIndex")
                || string_equals(key.as.string.data, "some")
                || string_equals(key.as.string.data, "every")
                || string_equals(key.as.string.data, "reduce")
                || string_equals(key.as.string.data, "push")
                || string_equals(key.as.string.data, "pop")
                || string_equals(key.as.string.data, "shift")
                || string_equals(key.as.string.data, "unshift")
                || string_equals(key.as.string.data, "reverse")
                || string_equals(key.as.string.data, "slice")
                || string_equals(key.as.string.data, "includes")
                || string_equals(key.as.string.data, "indexOf"))) {
            out_value->type = VM_VALUE_ARRAY_METHOD;
            out_value->as.array_method.receiver = target.as.heap_object;
            out_value->as.array_method.name = key.as.string.data;
            return NATIVE_BYTEBEAT_VM_OK;
        }
        size_t index = 0;
        if (!property_index(key, &index)
            || index >= target.as.heap_object->item_count) {
            *out_value = value_undefined();
            return NATIVE_BYTEBEAT_VM_OK;
        }
        *out_value = target.as.heap_object->items[index];
        return NATIVE_BYTEBEAT_VM_OK;
    }
    if (target.type == VM_VALUE_OBJECT && target.as.heap_object
        && key.type == VM_VALUE_STRING) {
        vm_heap_object_t *object = target.as.heap_object;
        for (size_t index = object->item_count; index > 0; --index) {
            if (string_equals(object->keys[index - 1], key.as.string.data)) {
                *out_value = object->items[index - 1];
                return NATIVE_BYTEBEAT_VM_OK;
            }
        }
        *out_value = value_undefined();
        return NATIVE_BYTEBEAT_VM_OK;
    }
    if (target.type == VM_VALUE_STRING) {
        if (key.type == VM_VALUE_STRING
            && string_equals(key.as.string.data, "length")) {
            *out_value = value_number((double)target.as.string.length);
            return NATIVE_BYTEBEAT_VM_OK;
        }
        if (key.type == VM_VALUE_STRING
            && string_equals(key.as.string.data, "charCodeAt")) {
            out_value->type = VM_VALUE_STRING_METHOD;
            out_value->as.string_method.receiver_data = target.as.string.data;
            out_value->as.string_method.receiver_length = target.as.string.length;
            out_value->as.string_method.name = key.as.string.data;
            return NATIVE_BYTEBEAT_VM_OK;
        }
        size_t index = 0;
        if (!property_index(key, &index)
            || index >= target.as.string.length) {
            *out_value = value_undefined();
            return NATIVE_BYTEBEAT_VM_OK;
        }
        out_value->type = VM_VALUE_STRING;
        out_value->as.string.data = target.as.string.data + index;
        out_value->as.string.length = 1;
        return NATIVE_BYTEBEAT_VM_OK;
    }
    return NATIVE_BYTEBEAT_VM_UNSUPPORTED_OPERATION;
}

static uint32_t decode_utf8_code_point(
    const char *data,
    size_t length,
    size_t *consumed) {
    const unsigned char first = (unsigned char)data[0];
    if (first < 0x80U) {
        *consumed = 1;
        return first;
    }
    size_t width;
    uint32_t code_point;
    if (first >= 0xC2U && first <= 0xDFU) {
        width = 2;
        code_point = first & 0x1FU;
    } else if (first >= 0xE0U && first <= 0xEFU) {
        width = 3;
        code_point = first & 0x0FU;
    } else if (first >= 0xF0U && first <= 0xF4U) {
        width = 4;
        code_point = first & 0x07U;
    } else {
        *consumed = 1;
        return 0xFFFDU;
    }
    if (width > length) {
        *consumed = 1;
        return 0xFFFDU;
    }
    for (size_t index = 1; index < width; ++index) {
        const unsigned char continuation = (unsigned char)data[index];
        if ((continuation & 0xC0U) != 0x80U) {
            *consumed = 1;
            return 0xFFFDU;
        }
        code_point = (code_point << 6) | (continuation & 0x3FU);
    }
    if ((width == 3 && code_point < 0x800U)
        || (width == 4 && code_point < 0x10000U)
        || code_point > 0x10FFFFU
        || (code_point >= 0xD800U && code_point <= 0xDFFFU)) {
        *consumed = 1;
        return 0xFFFDU;
    }
    *consumed = width;
    return code_point;
}

static native_bytebeat_vm_result_t call_string_method(
    vm_value_t method,
    const vm_value_t *arguments,
    size_t argument_count,
    vm_value_t *out_value) {
    if (!string_equals(method.as.string_method.name, "charCodeAt")) {
        return NATIVE_BYTEBEAT_VM_UNSUPPORTED_OPERATION;
    }
    const double requested_index = argument_count
        ? to_number(arguments[0])
        : 0.0;
    const double index_value = isnan(requested_index)
        ? 0.0
        : trunc(requested_index);
    if (index_value < 0.0
        || !isfinite(index_value)
        || index_value >= (double)method.as.string_method.receiver_length) {
        *out_value = value_number(NAN);
        return NATIVE_BYTEBEAT_VM_OK;
    }
    const size_t target_index = (size_t)index_value;
    size_t byte_offset = 0;
    size_t code_unit_index = 0;
    while (byte_offset < method.as.string_method.receiver_length) {
        size_t consumed = 0;
        const uint32_t code_point = decode_utf8_code_point(
            method.as.string_method.receiver_data + byte_offset,
            method.as.string_method.receiver_length - byte_offset,
            &consumed);
        if (code_point > 0xFFFFU) {
            const uint32_t adjusted = code_point - 0x10000U;
            const uint32_t high_surrogate = 0xD800U + (adjusted >> 10);
            const uint32_t low_surrogate = 0xDC00U + (adjusted & 0x3FFU);
            if (code_unit_index == target_index) {
                *out_value = value_number((double)high_surrogate);
                return NATIVE_BYTEBEAT_VM_OK;
            }
            if (code_unit_index + 1 == target_index) {
                *out_value = value_number((double)low_surrogate);
                return NATIVE_BYTEBEAT_VM_OK;
            }
            code_unit_index += 2;
        } else {
            if (code_unit_index == target_index) {
                *out_value = value_number((double)code_point);
                return NATIVE_BYTEBEAT_VM_OK;
            }
            ++code_unit_index;
        }
        byte_offset += consumed;
    }
    *out_value = value_number(NAN);
    return NATIVE_BYTEBEAT_VM_OK;
}

static native_bytebeat_vm_result_t set_property(
    native_bytebeat_vm_t *vm,
    vm_value_t target,
    vm_value_t key,
    vm_value_t value) {
    if (target.type == VM_VALUE_THIS && key.type == VM_VALUE_STRING) {
        return store_binding(
            vm->globals,
            &vm->global_count,
            VM_GLOBAL_CAPACITY,
            key.as.string.data,
            value);
    }
    if (target.type == VM_VALUE_ARRAY && target.as.heap_object) {
        size_t index = 0;
        if (!property_index(key, &index)
            || index >= target.as.heap_object->item_count) {
            return NATIVE_BYTEBEAT_VM_UNSUPPORTED_OPERATION;
        }
        target.as.heap_object->items[index] = value;
        return NATIVE_BYTEBEAT_VM_OK;
    }
    if (target.type == VM_VALUE_OBJECT && target.as.heap_object
        && key.type == VM_VALUE_STRING) {
        vm_heap_object_t *object = target.as.heap_object;
        for (size_t index = object->item_count; index > 0; --index) {
            if (string_equals(object->keys[index - 1], key.as.string.data)) {
                object->items[index - 1] = value;
                return NATIVE_BYTEBEAT_VM_OK;
            }
        }
        if (object->item_count >= VM_MAX_ARRAY_LENGTH
            || object->item_count == SIZE_MAX
            || (object->item_count + 1)
                > (SIZE_MAX - sizeof(*object)) / (sizeof(vm_value_t) + sizeof(char *))) {
            return NATIVE_BYTEBEAT_VM_NO_MEMORY;
        }
        if (object->item_count == object->item_capacity) {
            const size_t capacity = object->item_capacity == 0
                ? 4
                : object->item_capacity > VM_MAX_ARRAY_LENGTH / 2
                    ? VM_MAX_ARRAY_LENGTH
                    : object->item_capacity * 2;
            vm_value_t *items = (vm_value_t *)calloc(capacity, sizeof(*items));
            const char **keys = (const char **)malloc(capacity * sizeof(*keys));
            if (!items || !keys) {
                free(items);
                free(keys);
                return NATIVE_BYTEBEAT_VM_NO_MEMORY;
            }
            if (object->item_count) {
                memcpy(items, object->items, object->item_count * sizeof(*items));
                memcpy(keys, object->keys, object->item_count * sizeof(*keys));
            }
            free(object->items);
            free(object->keys);
            object->items = items;
            object->keys = keys;
            object->item_capacity = capacity;
        }
        object->keys[object->item_count] = key.as.string.data;
        object->items[object->item_count] = value;
        object->item_count++;
        const size_t old_allocation_size = object->allocation_size;
        object->allocation_size = sizeof(*object)
            + object->item_capacity * (sizeof(vm_value_t) + sizeof(char *));
        vm->heap_bytes -= old_allocation_size;
        vm->heap_bytes += object->allocation_size;
        return NATIVE_BYTEBEAT_VM_OK;
    }
    return NATIVE_BYTEBEAT_VM_UNSUPPORTED_OPERATION;
}

static native_bytebeat_vm_result_t run_instructions(
    native_bytebeat_vm_t *vm,
    const native_bytebeat_instruction_t *instructions,
    uint32_t instruction_count,
    double t,
    double sample_rate,
    size_t depth,
    vm_value_t *out_value) {
    if (depth >= VM_CALL_DEPTH) return NATIVE_BYTEBEAT_VM_RUNTIME_ERROR;
    vm_frame_t *frame = &vm->frames[depth];
    frame->stack_size = 0;
    memset(frame->iterators, 0, sizeof(frame->iterators));
    uint32_t pc = 0;
    uint32_t steps = 0;

    while (pc < instruction_count) {
        if (++steps > VM_STEP_LIMIT) return NATIVE_BYTEBEAT_VM_STEP_LIMIT;
        const native_bytebeat_instruction_t *instruction = &instructions[pc];
        const native_bytebeat_value_t *field = NULL;
        const char *name = NULL;
        int64_t integer = 0;
        bool boolean = false;
        vm_value_t left;
        vm_value_t right;
        vm_value_t result;
        native_bytebeat_vm_result_t status = NATIVE_BYTEBEAT_VM_OK;

        switch (instruction->opcode) {
            case NATIVE_BC_PUSH_NUMBER:
                field = find_field(instruction, NATIVE_BC_FIELD_VALUE);
                if (!field || (field->type != NATIVE_BC_VALUE_NUMBER
                    && field->type != NATIVE_BC_VALUE_INTEGER)) {
                    return NATIVE_BYTEBEAT_VM_RUNTIME_ERROR;
                }
                result = value_from_bytecode(field);
                if (!push(frame, result)) return NATIVE_BYTEBEAT_VM_STACK_ERROR;
                break;
            case NATIVE_BC_PUSH_STRING:
                field = find_field(instruction, NATIVE_BC_FIELD_VALUE);
                if (!field || field->type != NATIVE_BC_VALUE_STRING) {
                    return NATIVE_BYTEBEAT_VM_RUNTIME_ERROR;
                }
                if (!push(frame, value_from_bytecode(field))) {
                    return NATIVE_BYTEBEAT_VM_STACK_ERROR;
                }
                break;
            case NATIVE_BC_PUSH_BOOLEAN:
                if (!get_boolean(instruction, NATIVE_BC_FIELD_VALUE, &boolean)) {
                    return NATIVE_BYTEBEAT_VM_RUNTIME_ERROR;
                }
                if (!push(frame, value_boolean(boolean))) {
                    return NATIVE_BYTEBEAT_VM_STACK_ERROR;
                }
                break;
            case NATIVE_BC_PUSH_NULL:
                result = value_undefined();
                result.type = VM_VALUE_NULL;
                if (!push(frame, result)) return NATIVE_BYTEBEAT_VM_STACK_ERROR;
                break;
            case NATIVE_BC_PUSH_UNDEFINED:
                if (!push(frame, value_undefined())) {
                    return NATIVE_BYTEBEAT_VM_STACK_ERROR;
                }
                break;
            case NATIVE_BC_LOAD_T:
                if (!push(frame, value_number(t))) return NATIVE_BYTEBEAT_VM_STACK_ERROR;
                break;
            case NATIVE_BC_LOAD_SAMPLE_RATE:
                if (!push(frame, value_number(sample_rate))) {
                    return NATIVE_BYTEBEAT_VM_STACK_ERROR;
                }
                break;
            case NATIVE_BC_LOAD_THIS:
                result = value_undefined();
                result.type = VM_VALUE_THIS;
                if (!push(frame, result)) return NATIVE_BYTEBEAT_VM_STACK_ERROR;
                break;
            case NATIVE_BC_LOAD_LOCAL:
            case NATIVE_BC_LOAD_GLOBAL: {
                if (!get_string(instruction, NATIVE_BC_FIELD_NAME, &name)) {
                    return NATIVE_BYTEBEAT_VM_RUNTIME_ERROR;
                }
                result = instruction->opcode == NATIVE_BC_LOAD_LOCAL
                    ? load_binding(frame->locals, frame->local_count, name)
                    : load_binding(vm->globals, vm->global_count, name);
                if (instruction->opcode == NATIVE_BC_LOAD_GLOBAL
                    && result.type == VM_VALUE_UNDEFINED) {
                    if (string_equals(name, "NaN")) result = value_number(NAN);
                    else if (string_equals(name, "Infinity")) result = value_number(INFINITY);
                    else if (builtin_supported(name)) {
                        result.type = VM_VALUE_BUILTIN_FUNCTION;
                        result.as.builtin_function = name;
                    }
                }
                if (!push(frame, result)) return NATIVE_BYTEBEAT_VM_STACK_ERROR;
                break;
            }
            case NATIVE_BC_STORE_LOCAL:
            case NATIVE_BC_STORE_GLOBAL:
                if (!get_string(instruction, NATIVE_BC_FIELD_NAME, &name)
                    || !pop(frame, &result)) {
                    return NATIVE_BYTEBEAT_VM_STACK_ERROR;
                }
                status = instruction->opcode == NATIVE_BC_STORE_LOCAL
                    ? store_binding(
                        frame->locals,
                        &frame->local_count,
                        VM_LOCAL_CAPACITY,
                        name,
                        result)
                    : store_binding(
                        vm->globals,
                        &vm->global_count,
                        VM_GLOBAL_CAPACITY,
                        name,
                        result);
                if (status != NATIVE_BYTEBEAT_VM_OK) return status;
                break;
            case NATIVE_BC_DUP:
                if (frame->stack_size == 0
                    || !push(frame, frame->stack[frame->stack_size - 1])) {
                    return NATIVE_BYTEBEAT_VM_STACK_ERROR;
                }
                break;
            case NATIVE_BC_DUP2:
                if (frame->stack_size < 2) return NATIVE_BYTEBEAT_VM_STACK_ERROR;
                left = frame->stack[frame->stack_size - 2];
                right = frame->stack[frame->stack_size - 1];
                if (!push(frame, left) || !push(frame, right)) {
                    return NATIVE_BYTEBEAT_VM_STACK_ERROR;
                }
                break;
            case NATIVE_BC_POP:
                if (!pop(frame, &result)) return NATIVE_BYTEBEAT_VM_STACK_ERROR;
                break;
            case NATIVE_BC_BINARY:
                if (!get_string(instruction, NATIVE_BC_FIELD_OPERATOR, &name)
                    || !pop(frame, &right)
                    || !pop(frame, &left)) {
                    return NATIVE_BYTEBEAT_VM_STACK_ERROR;
                }
                status = binary_operation(name, left, right, &result);
                if (status != NATIVE_BYTEBEAT_VM_OK) return status;
                if (!push(frame, result)) return NATIVE_BYTEBEAT_VM_STACK_ERROR;
                break;
            case NATIVE_BC_UNARY:
                if (!get_string(instruction, NATIVE_BC_FIELD_OPERATOR, &name)
                    || !pop(frame, &left)) {
                    return NATIVE_BYTEBEAT_VM_STACK_ERROR;
                }
                status = unary_operation(name, left, &result);
                if (status != NATIVE_BYTEBEAT_VM_OK) return status;
                if (!push(frame, result)) return NATIVE_BYTEBEAT_VM_STACK_ERROR;
                break;
            case NATIVE_BC_TO_STRING:
                if (!pop(frame, &result)) return NATIVE_BYTEBEAT_VM_STACK_ERROR;
                if (result.type != VM_VALUE_STRING) {
                    return NATIVE_BYTEBEAT_VM_UNSUPPORTED_OPERATION;
                }
                if (!push(frame, result)) return NATIVE_BYTEBEAT_VM_STACK_ERROR;
                break;
            case NATIVE_BC_PUSH_MATH_CONSTANT:
                if (!get_string(instruction, NATIVE_BC_FIELD_NAME, &name)) {
                    return NATIVE_BYTEBEAT_VM_RUNTIME_ERROR;
                }
                result = value_number(math_constant(name));
                if (!push(frame, result)) return NATIVE_BYTEBEAT_VM_STACK_ERROR;
                break;
            case NATIVE_BC_CALL: {
                if (!get_string(instruction, NATIVE_BC_FIELD_NAME, &name)
                    || !get_integer(instruction, NATIVE_BC_FIELD_ARGUMENT_COUNT, &integer)
                    || integer < 0
                    || (uint64_t)integer > frame->stack_size
                    || integer > VM_STACK_CAPACITY) {
                    return NATIVE_BYTEBEAT_VM_STACK_ERROR;
                }
                const size_t count = (size_t)integer;
                vm_value_t *arguments = frame->arguments;
                const bool heap_arguments = count > VM_INLINE_ARGUMENT_CAPACITY;
                if (heap_arguments) {
                    arguments = (vm_value_t *)malloc(count * sizeof(*arguments));
                    if (!arguments) return NATIVE_BYTEBEAT_VM_NO_MEMORY;
                }
                for (size_t index = 0; index < count; ++index) {
                    arguments[count - index - 1] = frame->stack[--frame->stack_size];
                }
                frame->active_arguments = arguments;
                frame->argument_count = count;
                status = call_builtin(vm, name, arguments, count, &result);
                frame->active_arguments = NULL;
                frame->argument_count = 0;
                if (heap_arguments) free(arguments);
                if (status != NATIVE_BYTEBEAT_VM_OK) return status;
                if (!push(frame, result)) return NATIVE_BYTEBEAT_VM_STACK_ERROR;
                break;
            }
            case NATIVE_BC_MAKE_FUNCTION: {
                const native_bytebeat_value_t *code =
                    find_field(instruction, NATIVE_BC_FIELD_INSTRUCTIONS);
                const native_bytebeat_value_t *parameters =
                    find_field(instruction, NATIVE_BC_FIELD_PARAMETERS);
                const native_bytebeat_value_t *rest =
                    find_field(instruction, NATIVE_BC_FIELD_REST_INDEX);
                if (!code || code->type != NATIVE_BC_VALUE_INSTRUCTIONS
                    || !parameters || parameters->type != NATIVE_BC_VALUE_LIST) {
                    return NATIVE_BYTEBEAT_VM_RUNTIME_ERROR;
                }
                result = value_undefined();
                result.type = VM_VALUE_FUNCTION;
                result.as.function.instructions = code->as.instructions.items;
                result.as.function.instruction_count = code->as.instructions.count;
                result.as.function.parameters = parameters->as.list.items;
                result.as.function.parameter_count = parameters->as.list.count;
                result.as.function.rest_index =
                    rest && rest->type == NATIVE_BC_VALUE_INTEGER
                        ? rest->as.integer
                        : -1;
                if (!push(frame, result)) return NATIVE_BYTEBEAT_VM_STACK_ERROR;
                break;
            }
            case NATIVE_BC_MAKE_ARRAY: {
                const native_bytebeat_value_t *spread_flags =
                    find_field(instruction, NATIVE_BC_FIELD_SPREAD_FLAGS);
                if (!get_integer(instruction, NATIVE_BC_FIELD_LENGTH, &integer)
                    || integer < 0 || integer > VM_STACK_CAPACITY
                    || (uint64_t)integer > frame->stack_size
                    || !spread_flags
                    || spread_flags->type != NATIVE_BC_VALUE_LIST
                    || spread_flags->as.list.count != (uint32_t)integer) {
                    return NATIVE_BYTEBEAT_VM_STACK_ERROR;
                }
                size_t output_length = 0;
                const size_t input_count = (size_t)integer;
                for (size_t index = 0; index < input_count; ++index) {
                    const native_bytebeat_value_t *spread =
                        &spread_flags->as.list.items[index];
                    const vm_value_t item =
                        frame->stack[frame->stack_size - input_count + index];
                    if (spread->type != NATIVE_BC_VALUE_BOOLEAN) {
                        return NATIVE_BYTEBEAT_VM_RUNTIME_ERROR;
                    }
                    if (!spread->as.boolean) {
                        output_length++;
                    } else if (item.type == VM_VALUE_ARRAY_SPREAD
                        && item.as.heap_object) {
                        if (item.as.heap_object->item_count
                            > VM_MAX_ARRAY_LENGTH - output_length) {
                            return NATIVE_BYTEBEAT_VM_NO_MEMORY;
                        }
                        output_length += item.as.heap_object->item_count;
                    } else {
                        return NATIVE_BYTEBEAT_VM_UNSUPPORTED_OPERATION;
                    }
                    if (output_length > VM_MAX_ARRAY_LENGTH) {
                        return NATIVE_BYTEBEAT_VM_NO_MEMORY;
                    }
                }
                vm_heap_object_t *array = allocate_array(vm, output_length);
                if (!array) return NATIVE_BYTEBEAT_VM_NO_MEMORY;
                size_t output_index = 0;
                for (size_t index = 0; index < input_count; ++index) {
                    const bool spread =
                        spread_flags->as.list.items[index].as.boolean;
                    const vm_value_t item =
                        frame->stack[frame->stack_size - input_count + index];
                    if (!spread) {
                        array->items[output_index++] = item;
                    } else {
                        const vm_heap_object_t *source = item.as.heap_object;
                        memcpy(
                            array->items + output_index,
                            source->items,
                            source->item_count * sizeof(*array->items));
                        output_index += source->item_count;
                    }
                }
                frame->stack_size -= input_count;
                result = value_undefined();
                result.type = VM_VALUE_ARRAY;
                result.as.heap_object = array;
                if (!push(frame, result)) return NATIVE_BYTEBEAT_VM_STACK_ERROR;
                break;
            }
            case NATIVE_BC_SPREAD_ARRAY: {
                if (!frame->stack_size) return NATIVE_BYTEBEAT_VM_STACK_ERROR;
                left = frame->stack[frame->stack_size - 1];
                if (left.type == VM_VALUE_ARRAY && left.as.heap_object) {
                    result.type = VM_VALUE_ARRAY_SPREAD;
                    result.as.heap_object = left.as.heap_object;
                    frame->stack[frame->stack_size - 1] = result;
                    break;
                }
                if (left.type != VM_VALUE_STRING
                    || left.as.string.length > VM_MAX_ARRAY_LENGTH) {
                    return NATIVE_BYTEBEAT_VM_UNSUPPORTED_OPERATION;
                }
                vm_heap_object_t *characters =
                    allocate_array(vm, left.as.string.length);
                if (!characters) return NATIVE_BYTEBEAT_VM_NO_MEMORY;
                for (size_t index = 0; index < characters->item_count; ++index) {
                    characters->items[index].type = VM_VALUE_STRING;
                    characters->items[index].as.string.data =
                        left.as.string.data + index;
                    characters->items[index].as.string.length = 1;
                }
                result.type = VM_VALUE_ARRAY_SPREAD;
                result.as.heap_object = characters;
                frame->stack[frame->stack_size - 1] = result;
                break;
            }
            case NATIVE_BC_MAKE_OBJECT: {
                const native_bytebeat_value_t *keys =
                    find_field(instruction, NATIVE_BC_FIELD_KEYS);
                if (!keys || keys->type != NATIVE_BC_VALUE_LIST
                    || keys->as.list.count > VM_STACK_CAPACITY
                    || keys->as.list.count > frame->stack_size) {
                    return NATIVE_BYTEBEAT_VM_STACK_ERROR;
                }
                vm_heap_object_t *object = allocate_array(vm, keys->as.list.count);
                if (!object) return NATIVE_BYTEBEAT_VM_NO_MEMORY;
                if (object->item_count) {
                    object->keys =
                        (const char **)malloc(object->item_count * sizeof(*object->keys));
                    if (!object->keys) return NATIVE_BYTEBEAT_VM_NO_MEMORY;
                    for (size_t index = 0; index < object->item_count; ++index) {
                        const native_bytebeat_value_t *key = &keys->as.list.items[index];
                        if (key->type != NATIVE_BC_VALUE_STRING) {
                            return NATIVE_BYTEBEAT_VM_RUNTIME_ERROR;
                        }
                        object->keys[index] = key->as.string.data;
                        object->items[object->item_count - index - 1] =
                            frame->stack[--frame->stack_size];
                    }
                    const size_t old_allocation_size = object->allocation_size;
                    object->allocation_size = sizeof(*object)
                        + object->item_capacity
                            * (sizeof(vm_value_t) + sizeof(char *));
                    vm->heap_bytes -= old_allocation_size;
                    vm->heap_bytes += object->allocation_size;
                }
                result = value_undefined();
                result.type = VM_VALUE_OBJECT;
                result.as.heap_object = object;
                if (!push(frame, result)) return NATIVE_BYTEBEAT_VM_STACK_ERROR;
                break;
            }
            case NATIVE_BC_ITER_BEGIN: {
                if (!get_integer(
                        instruction,
                        NATIVE_BC_FIELD_ITERATOR,
                        &integer)
                    || integer < 0 || integer >= VM_ITERATOR_CAPACITY
                    || !get_boolean(
                        instruction,
                        NATIVE_BC_FIELD_KEYS,
                        &boolean)
                    || !pop(frame, &left)) {
                    return NATIVE_BYTEBEAT_VM_STACK_ERROR;
                }
                if ((!boolean && left.type != VM_VALUE_ARRAY
                        && left.type != VM_VALUE_STRING)
                    || (boolean && left.type != VM_VALUE_ARRAY
                        && left.type != VM_VALUE_STRING
                        && left.type != VM_VALUE_OBJECT)) {
                    return NATIVE_BYTEBEAT_VM_UNSUPPORTED_OPERATION;
                }
                vm_iterator_t *iterator = &frame->iterators[integer];
                iterator->iterable = left;
                iterator->index = 0;
                iterator->keys = boolean;
                iterator->active = true;
                break;
            }
            case NATIVE_BC_ITER_NEXT: {
                if (!get_integer(
                        instruction,
                        NATIVE_BC_FIELD_ITERATOR,
                        &integer)
                    || integer < 0 || integer >= VM_ITERATOR_CAPACITY
                    || !get_string(instruction, NATIVE_BC_FIELD_NAME, &name)
                    || !get_boolean(
                        instruction,
                        NATIVE_BC_FIELD_LOCAL,
                        &boolean)) {
                    return NATIVE_BYTEBEAT_VM_RUNTIME_ERROR;
                }
                vm_iterator_t *iterator = &frame->iterators[integer];
                if (!iterator->active) return NATIVE_BYTEBEAT_VM_RUNTIME_ERROR;
                vm_value_t iterable = iterator->iterable;
                size_t length = 0;
                if (iterable.type == VM_VALUE_ARRAY
                    || iterable.type == VM_VALUE_OBJECT) {
                    if (!iterable.as.heap_object) {
                        return NATIVE_BYTEBEAT_VM_RUNTIME_ERROR;
                    }
                    length = iterable.as.heap_object->item_count;
                } else {
                    length = iterable.as.string.length;
                }
                if (iterator->index < length) {
                    vm_value_t item = value_undefined();
                    if (iterator->keys) {
                        if (iterable.type == VM_VALUE_OBJECT) {
                            const char *key =
                                iterable.as.heap_object->keys[iterator->index];
                            item.type = VM_VALUE_STRING;
                            item.as.string.data = key;
                            item.as.string.length = (uint32_t)strlen(key);
                        } else {
                            item = value_number((double)iterator->index);
                        }
                    } else if (iterable.type == VM_VALUE_ARRAY) {
                        item = iterable.as.heap_object->items[iterator->index];
                    } else {
                        item.type = VM_VALUE_STRING;
                        item.as.string.data =
                            iterable.as.string.data + iterator->index;
                        item.as.string.length = 1;
                    }
                    vm_binding_t *bindings = boolean
                        ? frame->locals
                        : vm->globals;
                    size_t *binding_count = boolean
                        ? &frame->local_count
                        : &vm->global_count;
                    const size_t capacity = boolean
                        ? VM_LOCAL_CAPACITY
                        : VM_GLOBAL_CAPACITY;
                    status = store_binding(
                        bindings,
                        binding_count,
                        capacity,
                        name,
                        item);
                    if (status != NATIVE_BYTEBEAT_VM_OK) return status;
                    iterator->index++;
                    if (!push(frame, value_boolean(true))) {
                        return NATIVE_BYTEBEAT_VM_STACK_ERROR;
                    }
                } else if (!push(frame, value_boolean(false))) {
                    return NATIVE_BYTEBEAT_VM_STACK_ERROR;
                }
                break;
            }
            case NATIVE_BC_ITER_END:
                if (!get_integer(
                        instruction,
                        NATIVE_BC_FIELD_ITERATOR,
                        &integer)
                    || integer < 0 || integer >= VM_ITERATOR_CAPACITY) {
                    return NATIVE_BYTEBEAT_VM_RUNTIME_ERROR;
                }
                frame->iterators[integer].active = false;
                frame->iterators[integer].iterable = value_undefined();
                break;
            case NATIVE_BC_CONSTRUCT_ARRAY: {
                if (!get_integer(
                        instruction,
                        NATIVE_BC_FIELD_ARGUMENT_COUNT,
                        &integer)
                    || integer < 0 || integer > 1
                    || (uint64_t)integer > frame->stack_size) {
                    return NATIVE_BYTEBEAT_VM_STACK_ERROR;
                }
                size_t length = 0;
                bool copy_argument = false;
                vm_value_t argument = value_undefined();
                if (integer == 1) {
                    argument = frame->stack[frame->stack_size - 1];
                    if (argument.type == VM_VALUE_NUMBER) {
                        const double requested_length = argument.as.number;
                        if (!isfinite(requested_length)
                            || requested_length < 0.0
                            || trunc(requested_length) != requested_length
                            || requested_length > VM_MAX_ARRAY_LENGTH) {
                            return NATIVE_BYTEBEAT_VM_RUNTIME_ERROR;
                        }
                        length = (size_t)requested_length;
                    } else {
                        length = 1;
                        copy_argument = true;
                    }
                }
                vm_heap_object_t *array = allocate_array(vm, length);
                if (!array) return NATIVE_BYTEBEAT_VM_NO_MEMORY;
                if (integer == 1) --frame->stack_size;
                if (copy_argument) array->items[0] = argument;
                result = value_undefined();
                result.type = VM_VALUE_ARRAY;
                result.as.heap_object = array;
                if (!push(frame, result)) return NATIVE_BYTEBEAT_VM_STACK_ERROR;
                break;
            }
            case NATIVE_BC_CALL_VALUE: {
                if (!get_integer(instruction, NATIVE_BC_FIELD_ARGUMENT_COUNT, &integer)
                    || integer < 0
                    || (uint64_t)integer > frame->stack_size
                    || integer >= VM_STACK_CAPACITY) {
                    return NATIVE_BYTEBEAT_VM_STACK_ERROR;
                }
                const size_t count = (size_t)integer;
                vm_value_t *arguments = frame->arguments;
                const bool heap_arguments = count > VM_INLINE_ARGUMENT_CAPACITY;
                if (heap_arguments) {
                    arguments = (vm_value_t *)malloc(count * sizeof(*arguments));
                    if (!arguments) return NATIVE_BYTEBEAT_VM_NO_MEMORY;
                }
                for (size_t index = 0; index < count; ++index) {
                    arguments[count - index - 1] = frame->stack[--frame->stack_size];
                }
                frame->active_arguments = arguments;
                frame->argument_count = count;
                if (!pop(frame, &left)) {
                    frame->active_arguments = NULL;
                    frame->argument_count = 0;
                    if (heap_arguments) free(arguments);
                    return NATIVE_BYTEBEAT_VM_STACK_ERROR;
                }
                if (left.type != VM_VALUE_FUNCTION
                    && left.type != VM_VALUE_BUILTIN_FUNCTION
                    && left.type != VM_VALUE_ARRAY_METHOD
                    && left.type != VM_VALUE_STRING_METHOD) {
                    frame->active_arguments = NULL;
                    frame->argument_count = 0;
                    if (heap_arguments) free(arguments);
                    return NATIVE_BYTEBEAT_VM_RUNTIME_ERROR;
                }
                frame->active_receiver = left;
                if (left.type == VM_VALUE_FUNCTION) {
                    status = call_function(
                        vm,
                        left,
                        arguments,
                        count,
                        t,
                        sample_rate,
                        depth,
                        &result);
                } else if (left.type == VM_VALUE_BUILTIN_FUNCTION) {
                    status = call_builtin(
                        vm,
                        left.as.builtin_function,
                        arguments,
                        count,
                        &result);
                } else if (left.type == VM_VALUE_ARRAY_METHOD) {
                    status = call_array_method(
                        vm,
                        left,
                        arguments,
                        count,
                        t,
                        sample_rate,
                        depth,
                        &result);
                } else {
                    status = call_string_method(
                        left,
                        arguments,
                        count,
                        &result);
                }
                frame->active_receiver = value_undefined();
                frame->active_arguments = NULL;
                frame->argument_count = 0;
                if (heap_arguments) free(arguments);
                if (status != NATIVE_BYTEBEAT_VM_OK) return status;
                if (!push(frame, result)) return NATIVE_BYTEBEAT_VM_STACK_ERROR;
                break;
            }
            case NATIVE_BC_GET_PROPERTY:
            case NATIVE_BC_GET_INDEX:
                if (!pop(frame, &right) || !pop(frame, &left)) {
                    return NATIVE_BYTEBEAT_VM_STACK_ERROR;
                }
                status = get_property(vm, left, right, &result);
                if (status != NATIVE_BYTEBEAT_VM_OK) return status;
                if (!push(frame, result)) return NATIVE_BYTEBEAT_VM_STACK_ERROR;
                break;
            case NATIVE_BC_GET_PROPERTY_KEEP_REF:
                if (frame->stack_size < 2) return NATIVE_BYTEBEAT_VM_STACK_ERROR;
                left = frame->stack[frame->stack_size - 2];
                right = frame->stack[frame->stack_size - 1];
                status = get_property(vm, left, right, &result);
                if (status != NATIVE_BYTEBEAT_VM_OK) return status;
                if (!push(frame, result)) return NATIVE_BYTEBEAT_VM_STACK_ERROR;
                break;
            case NATIVE_BC_SET_PROPERTY:
                if (!pop(frame, &result)
                    || !pop(frame, &right)
                    || !pop(frame, &left)) {
                    return NATIVE_BYTEBEAT_VM_STACK_ERROR;
                }
                status = set_property(vm, left, right, result);
                if (status != NATIVE_BYTEBEAT_VM_OK) return status;
                if (!push(frame, result)) return NATIVE_BYTEBEAT_VM_STACK_ERROR;
                break;
            case NATIVE_BC_KEEP_VALUE_DROP_REF:
                if (!pop(frame, &result)
                    || !pop(frame, &right)
                    || !pop(frame, &left)
                    || !push(frame, result)) {
                    return NATIVE_BYTEBEAT_VM_STACK_ERROR;
                }
                break;
            case NATIVE_BC_UPDATE_GLOBAL:
            case NATIVE_BC_UPDATE_LOCAL: {
                if (!get_string(instruction, NATIVE_BC_FIELD_NAME, &name)
                    || !get_string(instruction, NATIVE_BC_FIELD_OPERATOR, &name)) {
                    return NATIVE_BYTEBEAT_VM_RUNTIME_ERROR;
                }
                const char *binding_name = NULL;
                if (!get_string(instruction, NATIVE_BC_FIELD_NAME, &binding_name)
                    || !get_boolean(instruction, NATIVE_BC_FIELD_PREFIX, &boolean)) {
                    return NATIVE_BYTEBEAT_VM_RUNTIME_ERROR;
                }
                vm_binding_t *bindings =
                    instruction->opcode == NATIVE_BC_UPDATE_LOCAL
                        ? frame->locals
                        : vm->globals;
                size_t *binding_count =
                    instruction->opcode == NATIVE_BC_UPDATE_LOCAL
                        ? &frame->local_count
                        : &vm->global_count;
                const size_t capacity =
                    instruction->opcode == NATIVE_BC_UPDATE_LOCAL
                        ? VM_LOCAL_CAPACITY
                        : VM_GLOBAL_CAPACITY;
                vm_value_t old = load_binding(bindings, *binding_count, binding_name);
                double old_number = to_number(old);
                const double next = old_number
                    + (string_equals(name, "++") ? 1.0 : -1.0);
                status = store_binding(
                    bindings,
                    binding_count,
                    capacity,
                    binding_name,
                    value_number(next));
                if (status != NATIVE_BYTEBEAT_VM_OK
                    || !push(frame, value_number(boolean ? next : old_number))) {
                    return status == NATIVE_BYTEBEAT_VM_OK
                        ? NATIVE_BYTEBEAT_VM_STACK_ERROR
                        : status;
                }
                break;
            }
            case NATIVE_BC_UPDATE_PROPERTY:
                if (!get_string(instruction, NATIVE_BC_FIELD_OPERATOR, &name)
                    || !get_boolean(instruction, NATIVE_BC_FIELD_PREFIX, &boolean)
                    || !pop(frame, &right)
                    || !pop(frame, &left)) {
                    return NATIVE_BYTEBEAT_VM_STACK_ERROR;
                }
                status = get_property(vm, left, right, &result);
                if (status != NATIVE_BYTEBEAT_VM_OK) return status;
                {
                    const double old_number = to_number(result);
                    const double next = old_number
                        + (string_equals(name, "++") ? 1.0 : -1.0);
                    status = set_property(vm, left, right, value_number(next));
                    if (status != NATIVE_BYTEBEAT_VM_OK
                        || !push(frame, value_number(boolean ? next : old_number))) {
                        return status == NATIVE_BYTEBEAT_VM_OK
                            ? NATIVE_BYTEBEAT_VM_STACK_ERROR
                            : status;
                    }
                }
                break;
            default:
                if (instruction->opcode == NATIVE_BC_JUMP) {
                    if (!get_integer(instruction, NATIVE_BC_FIELD_TARGET, &integer)
                        || integer < 0 || (uint64_t)integer >= instruction_count) {
                        return NATIVE_BYTEBEAT_VM_RUNTIME_ERROR;
                    }
                    pc = (uint32_t)integer;
                    continue;
                }
                if (instruction->opcode == NATIVE_BC_JUMP_IF_FALSE_POP) {
                    if (!get_integer(instruction, NATIVE_BC_FIELD_TARGET, &integer)
                        || !pop(frame, &left)) {
                        return NATIVE_BYTEBEAT_VM_STACK_ERROR;
                    }
                    if (!truthy(left)) {
                        if (integer < 0 || (uint64_t)integer >= instruction_count) {
                            return NATIVE_BYTEBEAT_VM_RUNTIME_ERROR;
                        }
                        pc = (uint32_t)integer;
                        continue;
                    }
                    break;
                }
                if (instruction->opcode == NATIVE_BC_JUMP_IF_FALSE
                    || instruction->opcode == NATIVE_BC_JUMP_IF_TRUE
                    || instruction->opcode == NATIVE_BC_JUMP_IF_NULLISH
                    || instruction->opcode == NATIVE_BC_JUMP_IF_NOT_NULLISH
                    || instruction->opcode == NATIVE_BC_JUMP_IF_UNDEFINED) {
                    if (!get_integer(instruction, NATIVE_BC_FIELD_TARGET, &integer)
                        || frame->stack_size == 0) {
                        return NATIVE_BYTEBEAT_VM_STACK_ERROR;
                    }
                    left = frame->stack[frame->stack_size - 1];
                    const bool take =
                        (instruction->opcode == NATIVE_BC_JUMP_IF_FALSE && !truthy(left))
                        || (instruction->opcode == NATIVE_BC_JUMP_IF_TRUE && truthy(left))
                        || (instruction->opcode == NATIVE_BC_JUMP_IF_NULLISH
                            && (left.type == VM_VALUE_NULL || left.type == VM_VALUE_UNDEFINED))
                        || (instruction->opcode == NATIVE_BC_JUMP_IF_NOT_NULLISH
                            && left.type != VM_VALUE_NULL && left.type != VM_VALUE_UNDEFINED)
                        || (instruction->opcode == NATIVE_BC_JUMP_IF_UNDEFINED
                            && left.type == VM_VALUE_UNDEFINED);
                    if (take) {
                        if (integer < 0 || (uint64_t)integer >= instruction_count) {
                            return NATIVE_BYTEBEAT_VM_RUNTIME_ERROR;
                        }
                        pc = (uint32_t)integer;
                        continue;
                    }
                    break;
                }
                if (instruction->opcode == NATIVE_BC_RETURN) {
                    *out_value = frame->stack_size
                        ? frame->stack[frame->stack_size - 1]
                        : value_undefined();
                    return NATIVE_BYTEBEAT_VM_OK;
                }
                if (instruction->opcode == NATIVE_BC_THROW) {
                    return NATIVE_BYTEBEAT_VM_RUNTIME_ERROR;
                }
                return NATIVE_BYTEBEAT_VM_UNSUPPORTED_OPERATION;
        }
        ++pc;
    }
    return NATIVE_BYTEBEAT_VM_RUNTIME_ERROR;
}

native_bytebeat_vm_result_t native_bytebeat_vm_create(
    const native_bytebeat_program_t *program,
    native_bytebeat_vm_t **out_vm) {
    if (!program || !out_vm || !program->instructions || program->instruction_count == 0) {
        return NATIVE_BYTEBEAT_VM_INVALID_ARGUMENT;
    }
    *out_vm = NULL;
    if (!bytecode_supported(program->instructions, program->instruction_count, 0)) {
        return NATIVE_BYTEBEAT_VM_UNSUPPORTED_OPERATION;
    }
    *out_vm = (native_bytebeat_vm_t *)calloc(1, sizeof(native_bytebeat_vm_t));
    if (!*out_vm) return NATIVE_BYTEBEAT_VM_NO_MEMORY;
    (*out_vm)->program = program;
    (*out_vm)->random_state = 0x6D2B79F5U;
    double initial_sample = 0.0;
    const native_bytebeat_vm_result_t initial_result =
        native_bytebeat_vm_evaluate(*out_vm, 0.0, &initial_sample);
    if (initial_result != NATIVE_BYTEBEAT_VM_OK) {
        native_bytebeat_vm_destroy(*out_vm);
        *out_vm = NULL;
        return initial_result;
    }
    native_bytebeat_vm_reset(*out_vm);
    return NATIVE_BYTEBEAT_VM_OK;
}

void native_bytebeat_vm_destroy(native_bytebeat_vm_t *vm) {
    if (!vm) return;
    vm_heap_object_t *object = vm->heap_objects;
    while (object) {
        vm_heap_object_t *next = object->next;
        free(object->items);
        free(object->keys);
        free(object);
        object = next;
    }
    free(vm);
}

void native_bytebeat_vm_reset(native_bytebeat_vm_t *vm) {
    if (!vm) return;
    memset(vm->globals, 0, sizeof(vm->globals));
    memset(vm->frames, 0, sizeof(vm->frames));
    vm->global_count = 0;
    vm->funcbeat = value_undefined();
    vm->random_state = 0x6D2B79F5U;
    vm->initialized = false;
}

native_bytebeat_vm_result_t native_bytebeat_vm_evaluate(
    native_bytebeat_vm_t *vm,
    double source_time,
    double *out_sample) {
    if (!vm || !out_sample) return NATIVE_BYTEBEAT_VM_INVALID_ARGUMENT;
    for (size_t index = 0; index < VM_CALL_DEPTH; ++index) {
        vm->frames[index].stack_size = 0;
        vm->frames[index].local_count = 0;
        vm->frames[index].active_arguments = NULL;
        vm->frames[index].argument_count = 0;
    }
    if (vm->heap_bytes >= VM_HEAP_COLLECTION_THRESHOLD) collect_heap(vm);
    const double sample_rate = vm->program->sample_rate;
    const double t = vm->program->mode == NATIVE_BYTEBEAT_MODE_FUNCBEAT
        ? source_time / sample_rate
        : source_time;
    vm_value_t result = value_undefined();
    native_bytebeat_vm_result_t status;

    if (!vm->initialized) {
        if (vm->program->mode == NATIVE_BYTEBEAT_MODE_FUNCBEAT) {
            status = run_instructions(
                vm,
                vm->program->instructions,
                vm->program->instruction_count,
                0.0,
                sample_rate,
                0,
                &result);
            if (status != NATIVE_BYTEBEAT_VM_OK) return status;
            if (result.type != VM_VALUE_FUNCTION) {
                return NATIVE_BYTEBEAT_VM_RUNTIME_ERROR;
            }
            vm->funcbeat = result;
            const vm_value_t startup_arguments[] = {
                value_number(0.0),
                value_number(sample_rate),
            };
            status = call_function(
                vm,
                vm->funcbeat,
                startup_arguments,
                2,
                0.0,
                sample_rate,
                0,
                &result);
            if (status != NATIVE_BYTEBEAT_VM_OK) return status;
        }
        vm->initialized = true;
    }

    if (vm->program->mode == NATIVE_BYTEBEAT_MODE_FUNCBEAT) {
        const vm_value_t arguments[] = {
            value_number(t),
            value_number(sample_rate),
        };
        status = call_function(
            vm,
            vm->funcbeat,
            arguments,
            2,
            t,
            sample_rate,
            0,
            &result);
    } else {
        status = run_instructions(
            vm,
            vm->program->instructions,
            vm->program->instruction_count,
            t,
            sample_rate,
            0,
            &result);
    }
    if (status != NATIVE_BYTEBEAT_VM_OK) return status;
    if (result.type == VM_VALUE_ARRAY) {
        vm_heap_object_t *stereo = result.as.heap_object;
        if (!stereo || stereo->item_count != 2) {
            return NATIVE_BYTEBEAT_VM_RUNTIME_ERROR;
        }
        for (size_t channel = 0; channel < stereo->item_count; ++channel) {
            const vm_value_type_t type = stereo->items[channel].type;
            if (type != VM_VALUE_NUMBER
                && type != VM_VALUE_BOOLEAN
                && type != VM_VALUE_NULL) {
                return NATIVE_BYTEBEAT_VM_RUNTIME_ERROR;
            }
        }
        *out_sample = (to_number(stereo->items[0])
            + to_number(stereo->items[1])) * 0.5;
        return NATIVE_BYTEBEAT_VM_OK;
    }
    if (result.type != VM_VALUE_NUMBER
        && result.type != VM_VALUE_BOOLEAN
        && result.type != VM_VALUE_NULL) {
        return NATIVE_BYTEBEAT_VM_RUNTIME_ERROR;
    }
    *out_sample = to_number(result);
    return NATIVE_BYTEBEAT_VM_OK;
}
