#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "audio_manager.h"
#include "bytebeat_engine.h"
#include "web_serial.h"

#define SERIAL_BUF_SIZE 8192

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
                if (strncmp(line, "V:", 2) == 0) {
                    int vol = atoi(line + 2);
                    audio_manager_set_speaker_vol((uint8_t)vol);
                } else if (strncmp(line, "D:", 2) == 0) {
                    bytebeat_engine_set_sample_skip(atoi(line + 2));
                } else if (strncmp(line, "S:", 2) == 0) {
                    bytebeat_engine_set_speed((float)atof(line + 2));
                } else if (strncmp(line, "M:", 2) == 0) {
                    bytebeat_engine_set_mode(atoi(line + 2));
                } else if (strncmp(line, "C:", 2) == 0) {
                    bytebeat_engine_set_formula(line + 2);
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

void web_serial_init(void) {
    xTaskCreate(serial_listener_task, "serial_cmd", 16384, NULL, 5, NULL);
}
