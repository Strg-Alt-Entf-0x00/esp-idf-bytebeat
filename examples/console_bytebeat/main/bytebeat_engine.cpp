/**
 * @file bytebeat_engine_c.cpp
 * @brief Pure C-based Bytebeat Engine - NO JavaScript/QuickJS!
 * 
 * This replaces the QuickJS-based engine with a native C bytecode engine.
 * Supports: Math functions (sin, cos, sqrt, etc.), stereo output, float/bytebeat modes
 */

#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "esp_timer.h"
#include "esp_heap_caps.h"
#include "audio_manager.h"
#include "bytebeat_engine.h"

extern "C" {
    #include "bytebeat_vm.h"
}

// ---------------------------------------------------------------------------
// Internal State
// ---------------------------------------------------------------------------
static volatile float s_speed_mult = 1.0f;
static volatile int s_mode = 1; // Default Floatbeat
static char s_formula[32768] = "0";
static volatile bool s_flag_reload = false;
static bytebeat_program_t s_program = {};

// ---------------------------------------------------------------------------
// Job Queue Structure
// ---------------------------------------------------------------------------
struct BytebeatJob {
    double start_t;
    int16_t* out_buf;
    bool is_reload;
};

static QueueHandle_t q_worker_in[2];
static QueueHandle_t q_worker_out[2];

// ---------------------------------------------------------------------------
// Formula compilation helper (Pure C!)
// ---------------------------------------------------------------------------
static bool compile_formula(bool report_errors) {
    if (strlen(s_formula) == 0 || (s_formula[0] == '0' && s_formula[1] == '\0')) {
        // Silent formula
        s_program.length = 0;
        return true;
    }
    
    bool success = bytebeat_compile(s_formula, &s_program);
    
    if (!success) {
        if (report_errors) {
            printf("Formula could not be compiled. Audio output is muted.\n");
        }
        s_program.length = 0;
        return false;
    }
    
    return true;
}

// ---------------------------------------------------------------------------
// Native C Worker Task (NO QuickJS!)
// ---------------------------------------------------------------------------
static void bytebeat_worker_task(void *pvParameter) {
    int core_id = (int)(intptr_t)pvParameter;
    const int CHUNK_SIZE = CONFIG_DAW_CHUNK_SIZE;

    // Compile initial formula
    compile_formula(core_id == 0);

    static volatile int s_sample_skip = 1;
    static volatile int s_chunk_counter = 0;

    while (1) {
        BytebeatJob job;
        xQueueReceive(q_worker_in[core_id], &job, portMAX_DELAY);

        if (job.is_reload) {
            compile_formula(core_id == 0);
            xQueueSend(q_worker_out[core_id], &job, portMAX_DELAY);
            continue;
        }

        s_chunk_counter = s_chunk_counter + 1;
        int current_chunk = s_chunk_counter;
        int skip = s_sample_skip;

        int64_t t1 = esp_timer_get_time();

        // Generate audio chunk - Pure C execution!
        if (s_program.length == 0) {
            // Silent
            memset(job.out_buf, 0, CHUNK_SIZE * 2 * sizeof(int16_t));
        } else {
            for (int i = 0; i < CHUNK_SIZE; i += skip) {
                uint32_t t = (uint32_t)(job.start_t + (i * s_speed_mult));
                
                float left = 0.0f, right = 0.0f;
                
                if (s_mode == 0) {
                    // Unsigned bytebeat (0..255)
                    uint8_t val = bytebeat_execute(&s_program, t);
                    int iv = (int)val;
                    iv = (iv - 128) * 256;
                    left = right = (float)iv;
                } else if (s_mode == 1) {
                    // Floatbeat (-1.0..1.0)
                    float val = bytebeat_execute_float(&s_program, t);
                    left = right = val * 32767.0f;
                } else if (s_mode == 2) {
                    // Signed bytebeat (-128..127)
                    uint8_t val = bytebeat_execute(&s_program, t);
                    int8_t iv = (int8_t)val;
                    left = right = (float)(iv * 256);
                }
                
                // Clamp
                if (left > 32767.0f) left = 32767.0f;
                if (left < -32768.0f) left = -32768.0f;
                if (right > 32767.0f) right = 32767.0f;
                if (right < -32768.0f) right = -32768.0f;
                
                // Fill samples (handle skip)
                for (int j = 0; j < skip && (i + j) < CHUNK_SIZE; j++) {
                    job.out_buf[(i + j) * 2] = (int16_t)left;
                    job.out_buf[(i + j) * 2 + 1] = (int16_t)right;
                }
            }
        }

        // Prevent IDLE task starvation
        if (current_chunk % 10 == 0) {
            vTaskDelay(pdMS_TO_TICKS(1));
        }

        int64_t t2 = esp_timer_get_time();
        int duration_ms = (int)((t2 - t1) / 1000);

        // DRS (Dynamic Rate Scaling) via Exponential Moving Average
        static float avg_duration = 10.0f;
        avg_duration = (avg_duration * 19.0f + (float)duration_ms) / 20.0f;

        if (avg_duration > 20.0f) {
            int ns = s_sample_skip + 4;
            s_sample_skip = (ns > 128) ? 128 : ns;
        } else if (avg_duration > 15.0f) {
            int ns = s_sample_skip + 1;
            s_sample_skip = (ns > 128) ? 128 : ns;
        } else if (avg_duration < 12.0f && s_sample_skip > 1) {
            s_sample_skip = s_sample_skip - 1;
        }

        xQueueSend(q_worker_out[core_id], &job, portMAX_DELAY);
    }
}

// ---------------------------------------------------------------------------
// Audio Coordinator Task (feeds I2S from worker results)
// ---------------------------------------------------------------------------
static void coordinator_task(void *pvParameter) {
    const int CHUNK_SIZE = CONFIG_DAW_CHUNK_SIZE;
    const int NUM_BUFFERS = CONFIG_DAW_NUM_BUFFERS;
    double t = 0.0;

    // Pre-allocate stereo buffers in PSRAM
    for (int i = 0; i < NUM_BUFFERS; i++) {
        int16_t* b = (int16_t*)heap_caps_malloc(CHUNK_SIZE * 2 * sizeof(int16_t), MALLOC_CAP_SPIRAM);
        if (b == NULL) {
            printf("Failed to allocate PSRAM buffer %d\n", i);
            vTaskDelay(portMAX_DELAY);
        }
        memset(b, 0, CHUNK_SIZE * 2 * sizeof(int16_t));
        BytebeatJob j = {t, b, false};
        t += CHUNK_SIZE * s_speed_mult;
        xQueueSend(q_worker_in[i % 2], &j, portMAX_DELAY);
    }

    bool needs_reload_core0 = false;
    bool needs_reload_core1 = false;
    int worker_idx = 0;

    while (1) {
        if (s_flag_reload) {
            s_flag_reload = false;
            t = 0.0;
            needs_reload_core0 = true;
            needs_reload_core1 = true;
        }

        BytebeatJob done_job;

        // Wait with timeout to prevent I2S DMA underflow buzzing
        while (xQueueReceive(q_worker_out[worker_idx], &done_job, pdMS_TO_TICKS(10)) != pdTRUE) {
            static int16_t silence_buf[1024] = {0};
            audio_manager_write_stereo(silence_buf, CHUNK_SIZE, portMAX_DELAY);
        }

        if (!done_job.is_reload) {
            audio_manager_write_stereo(done_job.out_buf, CHUNK_SIZE, portMAX_DELAY);
        }

        done_job.start_t = t;
        t += CHUNK_SIZE * s_speed_mult;

        if (worker_idx == 0 && needs_reload_core0) {
            done_job.is_reload = true;
            needs_reload_core0 = false;
        } else if (worker_idx == 1 && needs_reload_core1) {
            done_job.is_reload = true;
            needs_reload_core1 = false;
        } else {
            done_job.is_reload = false;
        }

        xQueueSend(q_worker_in[worker_idx], &done_job, portMAX_DELAY);
        worker_idx = (worker_idx + 1) % 2;
    }
}

// ---------------------------------------------------------------------------
// Public API
// ---------------------------------------------------------------------------
void bytebeat_engine_init(void) {
    const int NUM_BUFFERS = CONFIG_DAW_NUM_BUFFERS;

    q_worker_in[0]  = xQueueCreate(NUM_BUFFERS / 2, sizeof(BytebeatJob));
    q_worker_out[0] = xQueueCreate(NUM_BUFFERS / 2, sizeof(BytebeatJob));
    q_worker_in[1]  = xQueueCreate(NUM_BUFFERS / 2, sizeof(BytebeatJob));
    q_worker_out[1] = xQueueCreate(NUM_BUFFERS / 2, sizeof(BytebeatJob));

    xTaskCreatePinnedToCore(bytebeat_worker_task, "bb_worker_0", 16384,
                            (void*)0, 4, NULL, CONFIG_DAW_JS_CORE_ID);
    xTaskCreatePinnedToCore(bytebeat_worker_task, "bb_worker_1", 16384,
                            (void*)1, 4, NULL, 1 - CONFIG_DAW_JS_CORE_ID);

    xTaskCreatePinnedToCore(coordinator_task, "bb_coord", 4096,
                            NULL, 5, NULL, CONFIG_DAW_COORD_CORE_ID);
    
}

void bytebeat_engine_set_formula(const char* formula) {
    if (formula == NULL) return;
    strncpy(s_formula, formula, 32767);
    s_formula[32767] = '\0';
    s_flag_reload = true;
}

void bytebeat_engine_set_mode(int mode) {
    s_mode = mode;
}

void bytebeat_engine_set_speed(float speed) {
    s_speed_mult = speed;
}
