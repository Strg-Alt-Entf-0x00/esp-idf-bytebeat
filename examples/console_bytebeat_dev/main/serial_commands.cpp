#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_heap_caps.h"
#include "audio_manager.h"
#include "bytebeat_engine.h"
#include "serial_commands.h"

#define SERIAL_BUF_SIZE 8192
#define MAX_NATIVE_BYTECODE_SIZE (8U * 1024U * 1024U)

static uint8_t *native_transfer_buffer = NULL;
static size_t native_transfer_expected = 0;
static size_t native_transfer_received = 0;

static void clear_native_transfer(void) {
    if (native_transfer_buffer) heap_caps_free(native_transfer_buffer);
    native_transfer_buffer = NULL;
    native_transfer_expected = 0;
    native_transfer_received = 0;
}

static bool parse_decimal_size(const char *text, size_t *out_size) {
    if (!text || !*text || !out_size) return false;
    char *end = NULL;
    unsigned long value = strtoul(text, &end, 10);
    if (end == text || *end != '\0' || value == 0 || value > MAX_NATIVE_BYTECODE_SIZE) {
        return false;
    }
    *out_size = (size_t)value;
    return true;
}

static int hex_value(char character) {
    if (character >= '0' && character <= '9') return character - '0';
    if (character >= 'a' && character <= 'f') return character - 'a' + 10;
    if (character >= 'A' && character <= 'F') return character - 'A' + 10;
    return -1;
}

static void serial_listener_task(void *arg) {
    char* line = (char*)malloc(SERIAL_BUF_SIZE);
    if (line == NULL) {
        printf("Failed to allocate serial buffer\n");
        vTaskDelete(NULL);
        return;
    }
    int pos = 0;

    while (1) {
        int c = fgetc(stdin);
        if (c != EOF && c >= 0) {
            if (c == '\n') {
                line[pos] = '\0';
                // Commands: V/D/S/M/P/T, FORMULA, BBLOAD/BBDATA/BBEND, STOP
                if (strncmp(line, "V:", 2) == 0) {
                    int vol = atoi(line + 2);
                    audio_manager_set_speaker_vol((uint8_t)vol);
                    printf("[SERIAL] Volume set to %d\n", vol);
                } else if (strncmp(line, "D:", 2) == 0) {
                    int skip = atoi(line + 2);
                    bytebeat_engine_set_sample_skip(skip);
                    printf("[SERIAL] Sample skip set to %d\n", skip);
                } else if (strncmp(line, "S:", 2) == 0) {
                    float speed = (float)atof(line + 2);
                    bytebeat_engine_set_speed(speed);
                    printf("[SERIAL] Speed set to %.4f\n", speed);
                } else if (strncmp(line, "M:", 2) == 0) {
                    int mode = atoi(line + 2);
                    bytebeat_engine_set_mode(mode);
                    printf("[SERIAL] Mode set to %d\n", mode);
                } else if (strncmp(line, "T:", 2) == 0) {
                    // T:duration (seconds, 0 = infinite)
                    uint32_t duration = (uint32_t)atoi(line + 2);
                    bytebeat_engine_set_duration(duration);
                    printf("[SERIAL] Safety timer set to %lu seconds\n", (unsigned long)duration);
                } else if (strncmp(line, "P:", 2) == 0) {
                    // P:0 = pause, P:1 = resume
                    int pause = (atoi(line + 2) == 0);
                    bytebeat_engine_set_paused(pause);
                    printf("[SERIAL] Playback %s\n", pause ? "PAUSED" : "RESUMED");
                } else if (strcmp(line, "STOP") == 0) {
                    // STOP = pause playback
                    bytebeat_engine_set_paused(true);
                    printf("[SERIAL] Playback STOPPED\n");
                } else if (strncmp(line, "BBLOAD:", 7) == 0) {
                    size_t expected = 0;
                    clear_native_transfer();
                    bytebeat_engine_set_paused(true);
                    if (!parse_decimal_size(line + 7, &expected)) {
                        printf("BYTEBEAT:BB:ERR:INVALID_SIZE\n");
                    } else {
                        native_transfer_buffer = (uint8_t *)heap_caps_malloc(
                            expected,
                            MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
                        if (!native_transfer_buffer) {
                            printf("BYTEBEAT:BB:ERR:NO_MEMORY\n");
                        } else {
                            native_transfer_expected = expected;
                            printf("BYTEBEAT:BB:READY:%lu\n", (unsigned long)expected);
                        }
                    }
                } else if (strncmp(line, "BBDATA:", 7) == 0) {
                    char *separator = strchr(line + 7, ':');
                    if (!native_transfer_buffer || !separator) {
                        printf("BYTEBEAT:BB:ERR:NO_TRANSFER\n");
                    } else {
                        *separator = '\0';
                        size_t offset = 0;
                        const size_t hex_length = strlen(separator + 1);
                        char *end = NULL;
                        unsigned long parsed_offset = strtoul(line + 7, &end, 10);
                        if (end == line + 7 || *end != '\0'
                            || parsed_offset != native_transfer_received
                            || hex_length == 0 || (hex_length & 1U) != 0
                            || hex_length / 2 > native_transfer_expected - native_transfer_received) {
                            clear_native_transfer();
                            printf("BYTEBEAT:BB:ERR:INVALID_CHUNK\n");
                        } else {
                            offset = (size_t)parsed_offset;
                            const size_t chunk_length = hex_length / 2;
                            bool valid = true;
                            for (size_t index = 0; index < chunk_length; ++index) {
                                const int high = hex_value(separator[1 + index * 2]);
                                const int low = hex_value(separator[2 + index * 2]);
                                if (high < 0 || low < 0) {
                                    valid = false;
                                    break;
                                }
                                native_transfer_buffer[offset + index] =
                                    (uint8_t)((high << 4) | low);
                            }
                            if (!valid) {
                                clear_native_transfer();
                                printf("BYTEBEAT:BB:ERR:INVALID_HEX\n");
                            } else {
                                native_transfer_received += chunk_length;
                                printf(
                                    "BYTEBEAT:BB:DATA_OK:%lu\n",
                                    (unsigned long)native_transfer_received);
                            }
                        }
                    }
                } else if (strcmp(line, "BBEND") == 0) {
                    if (!native_transfer_buffer
                        || native_transfer_received != native_transfer_expected) {
                        clear_native_transfer();
                        printf("BYTEBEAT:BB:ERR:INCOMPLETE\n");
                    } else {
                        const bytebeat_native_load_result_t load_result =
                            bytebeat_engine_set_native_bytecode(
                                native_transfer_buffer,
                                native_transfer_expected);
                        clear_native_transfer();
                        if (load_result == BYTEBEAT_NATIVE_LOAD_OK) {
                            bytebeat_engine_set_paused(false);
                            printf("BYTEBEAT:BB:OK\n");
                        } else {
                            const char *reason = "UNKNOWN";
                            switch (load_result) {
                                case BYTEBEAT_NATIVE_LOAD_INVALID_ARGUMENT:
                                    reason = "INVALID_ARGUMENT";
                                    break;
                                case BYTEBEAT_NATIVE_LOAD_NOT_READY:
                                    reason = "ENGINE_NOT_READY";
                                    break;
                                case BYTEBEAT_NATIVE_LOAD_NO_MEMORY:
                                    reason = "NO_MEMORY";
                                    break;
                                case BYTEBEAT_NATIVE_LOAD_INVALID_BYTECODE:
                                    reason = "INVALID_BYTECODE";
                                    break;
                                case BYTEBEAT_NATIVE_LOAD_VM_UNSUPPORTED:
                                    reason = "VM_UNSUPPORTED";
                                    break;
                                case BYTEBEAT_NATIVE_LOAD_PREFLIGHT_FAILED:
                                    reason = "VM_PREFLIGHT_FAILED";
                                    break;
                                case BYTEBEAT_NATIVE_LOAD_QUEUE_FULL:
                                    reason = "QUEUE_FULL";
                                    break;
                                case BYTEBEAT_NATIVE_LOAD_OK:
                                    break;
                            }
                            printf("BYTEBEAT:BB:ERR:%s\n", reason);
                        }
                    }
                } else if (strncmp(line, "FORMULA:", 8) == 0) {
                    printf("[SERIAL] Loading formula: %s\n", line + 8);
                    bytebeat_engine_set_formula(line + 8);
                    // Auto-resume when loading a formula
                    bytebeat_engine_set_paused(false);
                    printf("[SERIAL] Playback STARTED\n");
                } else if (strncmp(line, "C:", 2) == 0) {
                    // Legacy support (old "C:" command)
                    printf("[SERIAL] Loading formula (legacy): %s\n", line + 2);
                    bytebeat_engine_set_formula(line + 2);
                    bytebeat_engine_set_paused(false);
                    printf("[SERIAL] Playback STARTED\n");
                } else if (strcmp(line, "TOOL_ID") == 0) {
                    printf("TOOL_ID:esp-idf-bytebeat:console_bytebeat_dev:2\n");
                } else if (strncmp(line, "VLD:", 4) == 0) {
                    if (bytebeat_engine_validate_formula(line + 4)) {
                        printf("BYTEBEAT:FORMULA:VALID\n");
                    } else {
                        printf("BYTEBEAT:FORMULA:INVALID\n");
                    }
                } else if (strcmp(line, "VERSION") == 0 || strcmp(line, "VERSION?") == 0) {
                    // VERSION command - for tool handshake
                    printf("BYTEBEAT:ESP32-P4:v2.1.0\n");
                } else if (strcmp(line, "INFO") == 0 || strcmp(line, "INFO?") == 0) {
                    // INFO command - detailed capabilities
                    printf("BYTEBEAT:ESP32-P4:v2.1.0:8000Hz:modes=U,F,S:bytecode=1\n");
                }
                pos = 0;
            } else if (c != '\r') {
                if (pos < SERIAL_BUF_SIZE - 1) {
                    line[pos++] = (char)c;
                }
            }
        } else {
            vTaskDelay(pdMS_TO_TICKS(1));
        }
    }
}

void serial_commands_init(void) {
    xTaskCreate(serial_listener_task, "serial_cmd", 16384, NULL, 5, NULL);
}
