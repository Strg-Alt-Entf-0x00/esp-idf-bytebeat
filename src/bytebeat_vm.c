#include "bytebeat_vm.h"
#include <ctype.h>
#include <string.h>
#include <stdlib.h>
#include <stdio.h>
#include <math.h>

// ESP-DSP provides the optional square-root helper; other math operations use
// the standard C math library and the target's configured math implementation.
#ifdef CONFIG_ESP_BYTEBEAT_USE_DSP
    #include "dsps_math.h"
    // Use ESP-DSP optimized functions where available
    #define FAST_SQRT(x)   dsps_sqrtf_f32(x)
    #define USE_DSP 1
#else
    #define FAST_SQRT(x)   sqrtf(x)
    #define USE_DSP 0
#endif

// Trigonometric functions use the standard C math library.
#define FAST_SIN(x)    sinf(x)
#define FAST_COS(x)    cosf(x)
#define FAST_ATAN2(y,x) atan2f(y, x)

// Helper: Check if string starts with given prefix
static bool starts_with(const char *str, const char *prefix) {
    size_t len = strlen(prefix);
    return strncmp(str, prefix, len) == 0;
}

// Helper: Skip whitespace
static const char* skip_whitespace(const char *p) {
    while (*p && isspace((unsigned char)*p)) p++;
    return p;
}

// Precedences
static int get_precedence(char op) {
    switch (op) {
        case '?': return 1;
        case '|': return 2;
        case '^': return 3;
        case '&': return 4;
        case '=': case '!': return 5;  // ==, !=
        case '<': case '>': return 6;  // <, >, <=, >=
        case '+': case '-': return 7;
        case '*': case '/': case '%': return 8;
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

// Parse math functions
static bool try_parse_function(const char **p, bytebeat_program_t *out_program, 
                               char *op_stack, int *op_top) {
    const char *start = *p;
    
    // List of supported functions
    struct {
        const char *name;
        bytebeat_op_t op;
        int args; // 1 = unary, 2 = binary
    } functions[] = {
        {"sin", OP_SIN, 1},
        {"cos", OP_COS, 1},
        {"tan", OP_TAN, 1},
        {"sqrt", OP_SQRT, 1},
        {"abs", OP_ABS, 1},
        {"floor", OP_FLOOR, 1},
        {"ceil", OP_CEIL, 1},
        {"round", OP_ROUND, 1},
        {"log", OP_LOG, 1},
        {"exp", OP_EXP, 1},
        {"pow", OP_POW, 2},
        {"min", OP_MIN, 2},
        {"max", OP_MAX, 2},
        {NULL, OP_END, 0}
    };
    
    for (int i = 0; functions[i].name != NULL; i++) {
        if (starts_with(start, functions[i].name)) {
            size_t len = strlen(functions[i].name);
            *p = start + len;
            *p = skip_whitespace(*p);
            
            if (**p != '(') {
                // Not a function call, might be a variable
                *p = start;
                return false;
            }
            
            // It's a function! Mark it with a special marker
            if (*op_top >= 125) return false;
            op_stack[++(*op_top)] = 'F'; // Function marker
            op_stack[++(*op_top)] = (char)functions[i].op; // Store opcode
            op_stack[++(*op_top)] = '(';
            (*p)++; // Skip '('
            return true;
        }
    }
    
    return false;
}

bool bytebeat_compile(const char *equation, bytebeat_program_t *out_program) {
    if (!equation || !out_program || !*equation) return false;

    out_program->length = 0;
    
    char op_stack[128];
    int op_top = -1;
    bool expect_operand = true;
    unsigned int open_parens = 0;
    
    const char *p = equation;
    
    while (*p) {
        p = skip_whitespace(p);
        if (!*p) break;
        
        // Try to parse functions (sin, cos, etc.)
        if (isalpha((unsigned char)*p) && expect_operand) {
            const char *func_start = p;
            if (try_parse_function(&p, out_program, op_stack, &op_top)) {
                open_parens++;
                expect_operand = true;
                continue;
            }
            p = func_start; // Reset if not a function
        }
        
        // Parse numbers
        if (isdigit((unsigned char)*p) || 
            (*p == '-' && expect_operand && isdigit((unsigned char)*(p + 1)))) {
            const char *number_start = p;
            bool negative = (*p == '-');
            if (negative) p++;
            
            uint32_t val = 0;
            while (isdigit((unsigned char)*p)) {
                val = val * 10 + (*p - '0');
                p++;
            }
            
            bool is_float = false;
            if (*p == '.') {
                is_float = true;
                p++;
                while (isdigit((unsigned char)*p)) p++;
            }
            
            if (!expect_operand || out_program->length >= MAX_BYTECODE_LEN) return false;
            
            if (is_float) {
                char *number_end = NULL;
                out_program->instructions[out_program->length].op = OP_PUSH_FLOAT;
                out_program->instructions[out_program->length].float_value = strtof(number_start, &number_end);
                if (number_end != p) return false;
            } else {
                out_program->instructions[out_program->length].op = OP_PUSH_CONST;
                out_program->instructions[out_program->length].value = negative ? (0U - val) : val;
            }
            out_program->length++;
            expect_operand = false;
            continue;
        }
        
        // Parse variable 't'
        if ((*p == 't' || *p == 'T') && expect_operand) {
            if (out_program->length >= MAX_BYTECODE_LEN) return false;
            out_program->instructions[out_program->length].op = OP_PUSH_T;
            out_program->length++;
            expect_operand = false;
            p++;
            continue;
        }
        
        // Parse PI constant
        if (starts_with(p, "PI") && expect_operand) {
            if (out_program->length >= MAX_BYTECODE_LEN) return false;
            out_program->instructions[out_program->length].op = OP_PUSH_FLOAT;
            out_program->instructions[out_program->length].float_value = 3.14159265359f;
            out_program->length++;
            expect_operand = false;
            p += 2;
            continue;
        }
        
        // Opening parenthesis
        if (*p == '(') {
            if (!expect_operand || op_top >= 127) return false;
            op_stack[++op_top] = '(';
            open_parens++;
            p++;
            continue;
        }
        
        // Closing parenthesis
        if (*p == ')') {
            if (expect_operand || open_parens == 0) return false;
            
            // Pop until we find '('
            while (op_top >= 0 && op_stack[op_top] != '(') {
                if (out_program->length >= MAX_BYTECODE_LEN) return false;
                
                char top_op = op_stack[op_top];
                if (top_op == 'F') {
                    // Skip function marker, get actual opcode
                    op_top--;
                    if (op_top >= 0) {
                        bytebeat_op_t func_op = (bytebeat_op_t)op_stack[op_top];
                        out_program->instructions[out_program->length].op = func_op;
                        out_program->length++;
                        op_top--;
                    }
                } else {
                    out_program->instructions[out_program->length].op = char_to_op(top_op);
                    out_program->length++;
                    op_top--;
                }
            }
            
            if (op_top < 0 || op_stack[op_top] != '(') return false;
            op_top--; // Pop '('
            open_parens--;
            
            // Check if this closes a function call
            if (op_top >= 1 && op_stack[op_top] == 'F') {
                // This was a function - the opcode is on the stack
                op_top--; // Skip 'F'
                if (op_top >= 0) {
                    bytebeat_op_t func_op = (bytebeat_op_t)op_stack[op_top];
                    out_program->instructions[out_program->length].op = func_op;
                    out_program->length++;
                    op_top--;
                }
            }
            
            expect_operand = false;
            p++;
            continue;
        }
        
        // Comma operator (for stereo or multiple arguments)
        if (*p == ',') {
            if (expect_operand) return false;
            // Just continue - we'll handle this in execution
            p++;
            expect_operand = true;
            continue;
        }
        
        // Handle operators
        char op = *p;
        
        // Shift operators
        if ((op == '<' || op == '>') && *(p+1) == op) {
            if (expect_operand) return false;
            
            // Shunting yard
            while (op_top >= 0 && op_stack[op_top] != '(' &&
                   get_precedence(op_stack[op_top]) >= get_precedence(op)) {
                if (out_program->length >= MAX_BYTECODE_LEN) return false;
                out_program->instructions[out_program->length].op = char_to_op(op_stack[op_top]);
                out_program->length++;
                op_top--;
            }
            if (op_top >= 127) return false;
            op_stack[++op_top] = op;
            expect_operand = true;
            p += 2;
            continue;
        }
        
        // Standard binary operators
        if (get_precedence(op) > 0) {
            if (expect_operand) return false;
            
            while (op_top >= 0 && op_stack[op_top] != '(' &&
                   get_precedence(op_stack[op_top]) >= get_precedence(op)) {
                if (out_program->length >= MAX_BYTECODE_LEN) return false;
                out_program->instructions[out_program->length].op = char_to_op(op_stack[op_top]);
                out_program->length++;
                op_top--;
            }
            if (op_top >= 127) return false;
            op_stack[++op_top] = op;
            expect_operand = true;
            p++;
            continue;
        }
        
        // Unknown character
        printf("Unknown char: '%c' at pos %ld\n", *p, (long)(p - equation));
        return false;
    }
    
    // Pop remaining operators
    if (expect_operand || open_parens != 0) return false;
    
    while (op_top >= 0) {
        if (op_stack[op_top] == '(' || out_program->length >= MAX_BYTECODE_LEN) return false;
        
        char top_op = op_stack[op_top];
        if (top_op == 'F') {
            op_top--;
            if (op_top >= 0) {
                bytebeat_op_t func_op = (bytebeat_op_t)op_stack[op_top];
                out_program->instructions[out_program->length].op = func_op;
                out_program->length++;
                op_top--;
            }
        } else {
            out_program->instructions[out_program->length].op = char_to_op(top_op);
            out_program->length++;
            op_top--;
        }
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
        } else if (op == OP_PUSH_FLOAT) {
            if (sp < 63) stack[++sp] = (uint32_t)program->instructions[i].float_value;
        } else if (op >= OP_SIN && op <= OP_EXP) {
            // Unary math functions (with ESP-DSP acceleration if enabled)
            if (sp < 0) continue;
            float val = (float)stack[sp];
            float result = 0.0f;
            
            switch (op) {
                case OP_SIN: result = FAST_SIN(val); break;
                case OP_COS: result = FAST_COS(val); break;
                case OP_TAN: result = tanf(val); break;
                case OP_SQRT: result = FAST_SQRT(val); break;
                case OP_ABS: result = fabsf(val); break;
                case OP_FLOOR: result = floorf(val); break;
                case OP_CEIL: result = ceilf(val); break;
                case OP_ROUND: result = roundf(val); break;
                case OP_LOG: result = logf(val); break;
                case OP_EXP: result = expf(val); break;
                default: break;
            }
            stack[sp] = (uint32_t)result;
        } else if (op == OP_POW || op == OP_MIN || op == OP_MAX) {
            // Binary math functions
            if (sp < 1) continue;
            float b = (float)stack[sp--];
            float a = (float)stack[sp];
            float result = 0.0f;
            
            switch (op) {
                case OP_POW: result = powf(a, b); break;
                case OP_MIN: result = (a < b) ? a : b; break;
                case OP_MAX: result = (a > b) ? a : b; break;
                default: break;
            }
            stack[sp] = (uint32_t)result;
        } else {
            // Binary operators
            if (sp < 1) continue;
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
                case OP_SHL: res = a << (b & 31); break;
                case OP_SHR: res = a >> (b & 31); break;
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

float bytebeat_execute_float(const bytebeat_program_t *program, uint32_t t) {
    float stack[64];
    int sp = -1;

    for (uint32_t i = 0; i < program->length; i++) {
        bytebeat_op_t op = program->instructions[i].op;
        
        if (op == OP_PUSH_T) {
            if (sp < 63) stack[++sp] = (float)t;
        } else if (op == OP_PUSH_CONST) {
            if (sp < 63) stack[++sp] = (float)program->instructions[i].value;
        } else if (op == OP_PUSH_FLOAT) {
            if (sp < 63) stack[++sp] = program->instructions[i].float_value;
        } else if (op >= OP_SIN && op <= OP_EXP) {
            // Unary math functions (with ESP-DSP acceleration if enabled)
            if (sp < 0) return 0.0f;
            float val = stack[sp];
            float result = 0.0f;
            
            switch (op) {
                case OP_SIN: result = FAST_SIN(val); break;
                case OP_COS: result = FAST_COS(val); break;
                case OP_TAN: result = tanf(val); break;  // No DSP version yet
                case OP_SQRT: result = FAST_SQRT(val); break;
                case OP_ABS: result = fabsf(val); break;
                case OP_FLOOR: result = floorf(val); break;
                case OP_CEIL: result = ceilf(val); break;
                case OP_ROUND: result = roundf(val); break;
                case OP_LOG: result = logf(val); break;
                case OP_EXP: result = expf(val); break;
                default: break;
            }
            stack[sp] = result;
        } else if (op == OP_POW || op == OP_MIN || op == OP_MAX) {
            // Binary math functions
            if (sp < 1) return 0.0f;
            float b = stack[sp--];
            float a = stack[sp];
            float result = 0.0f;
            
            switch (op) {
                case OP_POW: result = powf(a, b); break;
                case OP_MIN: result = (a < b) ? a : b; break;
                case OP_MAX: result = (a > b) ? a : b; break;
                default: break;
            }
            stack[sp] = result;
        } else {
            // Binary operators
            if (sp < 1) return 0.0f;
            float b = stack[sp--];
            float a = stack[sp--];
            float result = 0.0f;
            
            switch (op) {
                case OP_ADD: result = a + b; break;
                case OP_SUB: result = a - b; break;
                case OP_MUL: result = a * b; break;
                case OP_DIV: result = (b != 0.0f) ? a / b : 0.0f; break;
                case OP_MOD: result = (b != 0.0f) ? fmodf(a, b) : 0.0f; break;
                case OP_AND: result = (float)((uint32_t)a & (uint32_t)b); break;
                case OP_OR:  result = (float)((uint32_t)a | (uint32_t)b); break;
                case OP_XOR: result = (float)((uint32_t)a ^ (uint32_t)b); break;
                case OP_SHL: result = (float)((uint32_t)a << ((uint32_t)b & 31)); break;
                case OP_SHR: result = (float)((uint32_t)a >> ((uint32_t)b & 31)); break;
                default: return 0.0f;
            }
            stack[++sp] = result;
        }
    }
    
    return sp >= 0 ? stack[sp] : 0.0f;
}

void bytebeat_execute_stereo(const bytebeat_program_t *program, uint32_t t, float *left, float *right) {
    // For now, execute once and duplicate to both channels
    // Later we can add comma operator support for true stereo
    float mono = bytebeat_execute_float(program, t);
    *left = mono;
    *right = mono;
}
