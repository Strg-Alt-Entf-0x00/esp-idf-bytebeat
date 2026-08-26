#include "bytebeat_vm.h"
#include <ctype.h>
#include <string.h>
#include <stdlib.h>
#include <stdio.h>

// Precedences
static int get_precedence(char op) {
    switch (op) {
        case '|': return 1;
        case '^': return 2;
        case '&': return 3;
        case '<': case '>': return 4;
        case '+': case '-': return 5;
        case '*': case '/': case '%': return 6;
        default: return 0;
    }
}

static bytebeat_op_t char_to_op(char op) {
    switch (op) {
        case '+': return OP_ADD;
        case '-': return OP_SUB;
        case '*': return OP_MUL;
        case '/': return OP_DIV;
        case '%': return OP_MOD;
        case '&': return OP_AND;
        case '|': return OP_OR;
        case '^': return OP_XOR;
        case '<': return OP_SHL;
        case '>': return OP_SHR;
        default: return OP_END;
    }
}

bool bytebeat_compile(const char *equation, bytebeat_program_t *out_program) {
    if (!equation || !out_program || !*equation) return false;

    out_program->length = 0;
    
    char op_stack[64];
    int op_top = -1;
    bool expect_operand = true;
    unsigned int open_parens = 0;
    
    const char *p = equation;
    while (*p) {
        if (isspace((unsigned char)*p)) {
            p++;
            continue;
        }
        
        if (isdigit((unsigned char)*p)) {
            uint32_t val = 0;
            while (isdigit((unsigned char)*p)) {
                val = val * 10 + (*p - '0');
                p++;
            }
            if (!expect_operand || out_program->length >= MAX_BYTECODE_LEN) return false;
            out_program->instructions[out_program->length].op = OP_PUSH_CONST;
            out_program->instructions[out_program->length].value = val;
            out_program->length++;
            expect_operand = false;
            continue; // Skip the p++ at the end of loop
        }
        
        if (*p == 't' || *p == 'T') {
            if (!expect_operand || out_program->length >= MAX_BYTECODE_LEN) return false;
            out_program->instructions[out_program->length].op = OP_PUSH_T;
            out_program->length++;
            expect_operand = false;
            p++;
            continue;
        }
        
        if (*p == '(') {
            if (!expect_operand || op_top >= 63) return false;
            op_stack[++op_top] = '(';
            open_parens++;
            p++;
            continue;
        }
        
        if (*p == ')') {
            if (expect_operand || open_parens == 0) return false;
            while (op_top >= 0 && op_stack[op_top] != '(') {
                if (out_program->length >= MAX_BYTECODE_LEN) return false;
                out_program->instructions[out_program->length].op = char_to_op(op_stack[op_top]);
                out_program->length++;
                op_top--;
            }
            if (op_top < 0 || op_stack[op_top] != '(') return false;
            op_top--;
            open_parens--;
            p++;
            continue;
        }
        
        // Handle operators
        char op = *p;
        if (op == '<' || op == '>') {
            // Check for << or >>
            if (*(p+1) == op) {
                p++; // skip second char
            } else {
                return false; // single < or > not supported in bytebeat usually
            }
        }
        
        if (get_precedence(op) > 0) {
            if (expect_operand) return false;
            while (op_top >= 0 && op_stack[op_top] != '(' &&
                   get_precedence(op_stack[op_top]) >= get_precedence(op)) {
                if (out_program->length >= MAX_BYTECODE_LEN) return false;
                out_program->instructions[out_program->length].op = char_to_op(op_stack[op_top]);
                out_program->length++;
                op_top--;
            }
            if (op_top >= 63) return false;
            op_stack[++op_top] = op;
            expect_operand = true;
            p++;
            continue;
        }
        
        // Unknown character
        return false;
    }
    
    // Pop remaining operators
    if (expect_operand || open_parens != 0) return false;
    while (op_top >= 0) {
        if (op_stack[op_top] == '(' || out_program->length >= MAX_BYTECODE_LEN) return false;
        out_program->instructions[out_program->length].op = char_to_op(op_stack[op_top]);
        out_program->length++;
        op_top--;
    }
    
    return true;
}

uint8_t bytebeat_execute(const bytebeat_program_t *program, uint32_t t) {
    uint32_t stack[64];
    int sp = -1;
    
    for (uint32_t i = 0; i < program->length; i++) {
        bytebeat_op_t op = program->instructions[i].op;
        
        if (op == OP_PUSH_T) {
            if (sp < 63) stack[++sp] = t;
        } else if (op == OP_PUSH_CONST) {
            if (sp < 63) stack[++sp] = program->instructions[i].value;
        } else {
            // Binary operators
            if (sp < 1) continue; // Stack underflow
            uint32_t b = stack[sp--];
            uint32_t a = stack[sp--];
            uint32_t res = 0;
            
            switch (op) {
                case OP_ADD: res = a + b; break;
                case OP_SUB: res = a - b; break;
                case OP_MUL: res = a * b; break;
                case OP_DIV: res = (b != 0) ? (a / b) : 0; break;
                case OP_MOD: res = (b != 0) ? (a % b) : 0; break;
                case OP_AND: res = a & b; break;
                case OP_OR:  res = a | b; break;
                case OP_XOR: res = a ^ b; break;
                case OP_SHL: res = a << b; break;
                case OP_SHR: res = a >> b; break;
                default: break;
            }
            stack[++sp] = res;
        }
    }
    
    if (sp >= 0) {
        return (uint8_t)(stack[sp] & 0xFF);
    }
    return 0;
}
