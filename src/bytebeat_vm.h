#pragma once

#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

// Opcode definitions for the Bytebeat VM
typedef enum {
    OP_END = 0,
    OP_PUSH_T,       // Push variable 't' onto stack
    OP_PUSH_CONST,   // Push a 32-bit constant onto stack
    OP_PUSH_FLOAT,   // Push a floating-point constant onto stack
    OP_ADD,
    OP_SUB,
    OP_MUL,
    OP_DIV,
    OP_MOD,
    OP_AND,
    OP_OR,
    OP_XOR,
    OP_SHL,
    OP_SHR
} bytebeat_op_t;

// A single instruction in the bytecode
typedef struct {
    bytebeat_op_t op;
    uint32_t value; // Only used for OP_PUSH_CONST
    float float_value; // Only used for OP_PUSH_FLOAT
} bytebeat_inst_t;

#define MAX_BYTECODE_LEN 128

// The compiled program structure
typedef struct {
    bytebeat_inst_t instructions[MAX_BYTECODE_LEN];
    uint32_t length;
} bytebeat_program_t;

/**
 * @brief Compiles an infix equation string to RPN bytecode.
 * 
 * Implements a simplified Shunting-yard algorithm.
 * 
 * @param equation The string to parse.
 * @param out_program Pointer to the program struct to fill.
 * @return true if successful, false on parse error (e.g., mismatched parenthesis).
 */
bool bytebeat_compile(const char *equation, bytebeat_program_t *out_program);

#include "esp_attr.h"

/**
 * @brief Executes the compiled bytecode for a given time 't'.
 * 
 * @param program The compiled program.
 * @param t The current time variable.
 * @return uint8_t The resulting 8-bit audio sample.
 */
uint8_t IRAM_ATTR bytebeat_execute(const bytebeat_program_t *program, uint32_t t);

float bytebeat_execute_float(const bytebeat_program_t *program, uint32_t t);

#ifdef __cplusplus
}
#endif
