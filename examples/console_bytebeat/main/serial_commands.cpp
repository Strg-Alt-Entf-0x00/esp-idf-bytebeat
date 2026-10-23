#include <stdio.h>
#include <errno.h>
#include <math.h>
#include <stdlib.h>
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "audio_manager.h"
#include "bytebeat_engine.h"
#include "serial_commands.h"

#define SERIAL_BUF_SIZE 8192

static bool parse_integer(const char *text, long minimum, long maximum, long *value) {
    char *end = NULL;
    errno = 0;
    long parsed = strtol(text, &end, 10);
    if (errno == ERANGE || end == text || *end != '\0' || parsed < minimum || parsed > maximum) {
        return false;
    }
    *value = parsed;
    return true;
}

static bool parse_speed(const char *text, float *value) {
    char *end = NULL;
    errno = 0;
    float parsed = strtof(text, &end);
    if (errno == ERANGE || end == text || *end != '\0' || !isfinite(parsed)
        || parsed < 0.01f || parsed > 16.0f) {
        return false;
    }
    *value = parsed;
    return true;
}

static void print_help(void) {
    printf("Commands:\n");
    printf("  FORMULA:<expression>  Play a formula, e.g. FORMULA:(t*5)&(t>>7)\n");
    printf("  V:<0-100>             Set speaker volume in percent\n");
    printf("  S:<0.01-16>           Set playback speed multiplier\n");
    printf("  M:<0|1|2>             Set mode: bytebeat, floatbeat, signed bytebeat\n");
    printf("  HELP                  Show this help\n");
}

static void serial_listener_task(void *arg) {
    char* line = (char*)malloc(SERIAL_BUF_SIZE);
    if (line == NULL) {
        printf("Failed to allocate serial buffer\n");
        vTaskDelete(NULL);
        return;
    }
    int pos = 0;
    bool line_too_long = false;

    while (1) {
        int c = fgetc(stdin);
        if (c != EOF && c >= 0) {
            if (c == '\n') {
                line[pos] = '\0';
                if (line_too_long) {
                    printf("Command is too long; maximum length is %d characters.\n", SERIAL_BUF_SIZE - 1);
                } else if (strcmp(line, "HELP") == 0) {
                    print_help();
                } else if (strncmp(line, "V:", 2) == 0) {
                    long volume = 0;
                    if (!parse_integer(line + 2, 0, 100, &volume)) {
                        printf("Invalid volume. Use V:0 through V:100.\n");
                    } else if (audio_manager_set_speaker_vol((uint8_t)volume) != ESP_OK) {
                        printf("Could not set speaker volume.\n");
                    } else {
                        printf("Volume set to %ld%%.\n", volume);
                    }
                } else if (strncmp(line, "S:", 2) == 0) {
                    float speed = 0.0f;
                    if (!parse_speed(line + 2, &speed)) {
                        printf("Invalid speed. Use a number from 0.01 through 16.\n");
                    } else {
                        bytebeat_engine_set_speed(speed);
                        printf("Playback speed set to %.2fx.\n", (double)speed);
                    }
                } else if (strncmp(line, "M:", 2) == 0) {
                    long mode = 0;
                    if (!parse_integer(line + 2, 0, 2, &mode)) {
                        printf("Invalid mode. Use M:0, M:1, or M:2.\n");
                    } else {
                        bytebeat_engine_set_mode((int)mode);
                        const char *mode_name = mode == 0
                            ? "unsigned bytebeat"
                            : (mode == 1 ? "floatbeat" : "signed bytebeat");
                        printf("Playback mode set to %s.\n", mode_name);
                    }
                } else if (strncmp(line, "FORMULA:", 8) == 0) {
                    if (line[8] == '\0') {
                        printf("Formula cannot be empty.\n");
                    } else {
                        bytebeat_engine_set_formula(line + 8);
                        printf("Formula received; compiling.\n");
                    }
                } else if (line[0] != '\0') {
                    printf("Unknown command. Send HELP to list available commands.\n");
                }
                pos = 0;
                line_too_long = false;
            } else if (c != '\r') {
                if (pos < SERIAL_BUF_SIZE - 1) {
                    line[pos++] = (char)c;
                } else {
                    line_too_long = true;
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
