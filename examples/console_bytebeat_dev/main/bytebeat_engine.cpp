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
#include <math.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "esp_timer.h"
#include "esp_heap_caps.h"
#include "audio_manager.h"
#include "bytebeat_engine.h"

extern "C" {
    #include "bytebeat_vm.h"
    #include "bytebeat_vm_pie.h"
    #include "native_bytebeat_bytecode.h"
    #include "native_bytebeat_vm.h"
}

// ---------------------------------------------------------------------------
// Internal State
// ---------------------------------------------------------------------------
static volatile int s_manual_sample_skip = 0;
static volatile float s_speed_mult = 1.0f;
static volatile int s_mode = 0; // Default to Unsigned Bytebeat for classic formulas
static char s_formula[32768] = "0"; // Default to SILENCE - wait for commands
static volatile bool s_flag_reload = false;
static volatile bool s_paused = true; // Start PAUSED
static volatile uint32_t s_duration_sec = 5; // Default: 5 seconds safety limit
static volatile int64_t s_start_time_us = 0; // Playback start time
static bytebeat_program_t s_program = {};  // Zero-initialize with empty braces (C++ compliant)
static QueueHandle_t s_native_program_queue = NULL;
static volatile bool s_native_playback_enabled = false;
static bool s_native_task_started = false;

struct NativeBytebeatSong {
    native_bytebeat_program_t *program;
    native_bytebeat_vm_t *vm;
};

// ---------------------------------------------------------------------------
// Stringify helper for Kconfig integer values
// ---------------------------------------------------------------------------
#define STRINGIFY_HELPER(x) #x
#define STRINGIFY(x) STRINGIFY_HELPER(x)

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

static void free_native_song(NativeBytebeatSong *song) {
    if (!song) return;
    native_bytebeat_vm_destroy(song->vm);
    native_bytebeat_program_free(song->program);
    free(song);
}

static int16_t native_sample_to_pcm(double sample, uint8_t mode) {
    if (!isfinite(sample)) sample = 0.0;
    if (mode == NATIVE_BYTEBEAT_MODE_FLOATBEAT
        || mode == NATIVE_BYTEBEAT_MODE_FUNCBEAT) {
        if (sample > 1.0) sample = 1.0;
        if (sample < -1.0) sample = -1.0;
        return (int16_t)(sample * 32767.0);
    }
    double value = fmod(trunc(sample), 256.0);
    if (value < 0.0) value += 256.0;
    const int byte_value = (int)value;
    if (mode == NATIVE_BYTEBEAT_MODE_SIGNED) {
        return (int16_t)((int8_t)byte_value * 256);
    }
    return (int16_t)((byte_value - 128) * 256);
}

static void native_bytebeat_task(void *pvParameter) {
    (void)pvParameter;
    const int chunk_size = CONFIG_DAW_CHUNK_SIZE;
    int16_t output[CONFIG_DAW_CHUNK_SIZE * 2];
    NativeBytebeatSong *current_song = NULL;
    double source_time = 0.0;

    while (1) {
        NativeBytebeatSong *next_song = NULL;
        if (s_native_program_queue
            && xQueueReceive(s_native_program_queue, &next_song, 0) == pdTRUE) {
            free_native_song(current_song);
            current_song = next_song;
            source_time = 0.0;
            s_mode = current_song->program->mode;
            printf(
                "[BYTECODE] Loaded native program: mode=%d rate=%.6f\n",
                s_mode,
                current_song->program->sample_rate);
        }

        if (!s_native_playback_enabled) {
            free_native_song(current_song);
            current_song = NULL;
            vTaskDelay(pdMS_TO_TICKS(50));
            continue;
        }
        if (!current_song || s_paused) {
            vTaskDelay(pdMS_TO_TICKS(10));
            continue;
        }
        if (s_duration_sec > 0 && s_start_time_us > 0) {
            const int64_t elapsed_us = esp_timer_get_time() - s_start_time_us;
            if (elapsed_us >= (int64_t)s_duration_sec * 1000000LL) {
                printf(
                    "[BYTECODE] Safety timer expired (%lu seconds)\n",
                    (unsigned long)s_duration_sec);
                s_paused = true;
                s_start_time_us = 0;
                continue;
            }
        }

        memset(output, 0, sizeof(output));
        bool execution_failed = false;
        for (int index = 0; index < chunk_size; ++index) {
            double sample = 0.0;
            native_bytebeat_vm_result_t result =
                native_bytebeat_vm_evaluate(current_song->vm, source_time, &sample);
            source_time += s_speed_mult;
            if (result != NATIVE_BYTEBEAT_VM_OK) {
                printf("[BYTECODE] Native VM execution failed: %d\n", result);
                s_paused = true;
                s_start_time_us = 0;
                execution_failed = true;
                break;
            }
            const int16_t pcm = native_sample_to_pcm(
                sample,
                current_song->program->mode);
            output[index * 2] = pcm;
            output[index * 2 + 1] = pcm;
        }
        audio_manager_write_stereo(output, chunk_size, portMAX_DELAY);
        if (execution_failed) vTaskDelay(pdMS_TO_TICKS(50));
    }
}

// ---------------------------------------------------------------------------
// Formula compilation helper (Pure C!)
// ---------------------------------------------------------------------------
static bool compile_formula(int core_id) {
    printf("[Core %d] Compiling formula: %s\n", core_id, s_formula);
    
    if (strlen(s_formula) == 0 || (s_formula[0] == '0' && s_formula[1] == '\0')) {
        // Silent formula
        s_program.length = 0;
        printf("[Core %d] Silent formula (length=0)\n", core_id);
        return true;
    }
    
    bool success = bytebeat_compile(s_formula, &s_program);
    
    if (!success) {
        printf("[Core %d] Compile error!\n", core_id);
        s_program.length = 0;
        return false;
    }
    
    printf("[Core %d] Compiled successfully! Instructions: %lu\n", core_id, s_program.length);
    return true;
}

// ---------------------------------------------------------------------------
// Native C Worker Task (NO QuickJS!)
// ---------------------------------------------------------------------------
static void bytebeat_worker_task(void *pvParameter) {
    int core_id = (int)(intptr_t)pvParameter;
    const int CHUNK_SIZE = CONFIG_DAW_CHUNK_SIZE;

    // Compile initial formula
    compile_formula(core_id);

    static volatile int s_sample_skip = 1;
    static volatile int s_chunk_counter = 0;

    while (1) {
        if (s_native_playback_enabled) {
            vTaskDelay(pdMS_TO_TICKS(50));
            continue;
        }

        // Check safety timer - auto-stop after duration
        if (!s_paused && s_duration_sec > 0 && s_start_time_us > 0) {
            int64_t elapsed_us = esp_timer_get_time() - s_start_time_us;
            int64_t limit_us = (int64_t)s_duration_sec * 1000000LL;
            if (elapsed_us >= limit_us) {
                printf("[Core %d] Safety timer expired (%lu seconds) - auto-stopping\n", 
                       core_id, (unsigned long)s_duration_sec);
                s_paused = true;
                s_start_time_us = 0;
            }
        }
        
        // Sleep when paused
        if (s_paused) {
            vTaskDelay(pdMS_TO_TICKS(100));
            continue;
        }
        
        BytebeatJob job;
        xQueueReceive(q_worker_in[core_id], &job, portMAX_DELAY);

        if (job.is_reload) {
            printf("[Core %d] Recompiling formula...\n", core_id);
            compile_formula(core_id);
            xQueueSend(q_worker_out[core_id], &job, portMAX_DELAY);
            continue;
        }

        s_chunk_counter = s_chunk_counter + 1;
        int current_chunk = s_chunk_counter;
        int skip = (s_manual_sample_skip > 0) ? s_manual_sample_skip : s_sample_skip;

        int64_t t1 = esp_timer_get_time();

        // Generate audio chunk - Pure C execution!
        if (s_program.length == 0) {
            // Silent - fill with zeros
            memset(job.out_buf, 0, CHUNK_SIZE * 2 * sizeof(int16_t));
            if (current_chunk == 1) {
                printf("[Core %d] Generating SILENCE (program.length=0)\n", core_id);
            }
        } else {
            if (current_chunk == 1) {
                printf("[Core %d] Generating AUDIO mode=%d, formula=%s\n", core_id, s_mode, s_formula);
            }
            
            for (int i = 0; i < CHUNK_SIZE; i += skip) {
                uint32_t t = (uint32_t)(job.start_t + (i * s_speed_mult));
                
                float left = 0.0f, right = 0.0f;
                
                if (s_mode == 0) {
                    // Unsigned bytebeat (0..255)
                    uint8_t val = bytebeat_execute(&s_program, t);
                    int iv = (int)val;
                    iv = (iv - 128) * 256;
                    left = right = (float)iv;
                    
                    // Debug first sample
                    if (current_chunk == 1 && i == 0) {
                        printf("[Core %d] First sample: t=%lu, bytebeat=%u, audio=%d\n", 
                               core_id, (unsigned long)t, (unsigned)val, (int)left);
                    }
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

        if (core_id == 0 && current_chunk % 20 == 0) {
            printf("[Core 0] chunk %d: %d ms, skip=%d\n", current_chunk, duration_ms, s_sample_skip);
        }

        if (s_manual_sample_skip == 0) {
            if (avg_duration > 20.0f) {
                int ns = s_sample_skip + 4;
                s_sample_skip = (ns > 128) ? 128 : ns;
            } else if (avg_duration > 15.0f) {
                int ns = s_sample_skip + 1;
                s_sample_skip = (ns > 128) ? 128 : ns;
            } else if (avg_duration < 12.0f && s_sample_skip > 1) {
                s_sample_skip = s_sample_skip - 1;
            }
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

    printf("Coordinator: %d PSRAM buffers allocated, rendering started\n", NUM_BUFFERS);

    bool needs_reload_core0 = false;
    bool needs_reload_core1 = false;
    int worker_idx = 0;

    while (1) {
        if (s_native_playback_enabled) {
            vTaskDelay(pdMS_TO_TICKS(20));
            continue;
        }

        // NEW: Check if paused
        if (s_paused) {
            vTaskDelay(pdMS_TO_TICKS(100)); // Sleep when paused
            continue;
        }
        
        if (s_flag_reload) {
            s_flag_reload = false;
            printf("Coordinator: reloading formula\n");
            t = 0.0;
            needs_reload_core0 = true;
            needs_reload_core1 = true;
        }

        BytebeatJob done_job;

        // Wait with timeout to prevent I2S DMA underflow buzzing
        while (xQueueReceive(q_worker_out[worker_idx], &done_job, pdMS_TO_TICKS(10)) != pdTRUE) {
            // NEW: Don't send silence if paused
            if (s_paused) {
                vTaskDelay(pdMS_TO_TICKS(100));
                continue;
            }
            static int16_t silence_buf[1024] = {0};
            audio_manager_write_stereo(silence_buf, CHUNK_SIZE, portMAX_DELAY);
        }

        if (!done_job.is_reload && !s_paused) {
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

    s_native_program_queue = xQueueCreate(1, sizeof(NativeBytebeatSong *));
    if (!s_native_program_queue) {
        printf("[BYTECODE] Failed to create native program queue\n");
    }

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
    if (s_native_program_queue) {
        const BaseType_t task_result = xTaskCreatePinnedToCore(
            native_bytebeat_task,
            "bb_native",
            8192,
            NULL,
            5,
            NULL,
            CONFIG_DAW_COORD_CORE_ID);
        s_native_task_started = task_result == pdPASS;
        if (!s_native_task_started) {
            printf("[BYTECODE] Failed to start native playback task\n");
        }
    }
    
    printf("Bytebeat Engine initialized (Pure C - No JavaScript!)\n");
    printf("Chip: %s | PIE hardware: %s (bytebeat PIE kernel: not implemented)\n",
           bytebeat_get_chip_name(), 
           bytebeat_pie_available() ? "Available" : "Not available");
           
#ifdef CONFIG_ESP_BYTEBEAT_USE_DSP
    printf("ESP-DSP: Enabled (square-root helper; target performance unmeasured)\n");
#else
    printf("ESP-DSP: Disabled (Standard math)\n");
#endif
}

void bytebeat_engine_set_formula(const char* formula) {
    if (formula == NULL) return;
    s_native_playback_enabled = false;
    strncpy(s_formula, formula, 32767);
    s_formula[32767] = '\0';
    s_flag_reload = true;
}

bytebeat_native_load_result_t bytebeat_engine_set_native_bytecode(
    const uint8_t *data,
    size_t length) {
    if (!data || length == 0) {
        return BYTEBEAT_NATIVE_LOAD_INVALID_ARGUMENT;
    }
    if (!s_native_program_queue || !s_native_task_started) {
        return BYTEBEAT_NATIVE_LOAD_NOT_READY;
    }

    NativeBytebeatSong *song =
        (NativeBytebeatSong *)calloc(1, sizeof(NativeBytebeatSong));
    if (!song) return BYTEBEAT_NATIVE_LOAD_NO_MEMORY;

    native_bytebeat_error_t load_result =
        native_bytebeat_program_load(data, length, &song->program);
    if (load_result != NATIVE_BC_ERROR_NONE) {
        printf("[BYTECODE] Program validation failed: %d\n", load_result);
        free_native_song(song);
        return BYTEBEAT_NATIVE_LOAD_INVALID_BYTECODE;
    }
    native_bytebeat_vm_result_t vm_result =
        native_bytebeat_vm_create(song->program, &song->vm);
    if (vm_result != NATIVE_BYTEBEAT_VM_OK) {
        printf("[BYTECODE] VM creation failed: %d\n", vm_result);
        free_native_song(song);
        if (vm_result == NATIVE_BYTEBEAT_VM_UNSUPPORTED_OPERATION) {
            return BYTEBEAT_NATIVE_LOAD_VM_UNSUPPORTED;
        }
        if (vm_result == NATIVE_BYTEBEAT_VM_NO_MEMORY) {
            return BYTEBEAT_NATIVE_LOAD_NO_MEMORY;
        }
        return BYTEBEAT_NATIVE_LOAD_PREFLIGHT_FAILED;
    }

    NativeBytebeatSong *replaced_song = NULL;
    if (xQueueReceive(s_native_program_queue, &replaced_song, 0) == pdTRUE) {
        free_native_song(replaced_song);
    }
    if (xQueueSend(s_native_program_queue, &song, 0) != pdTRUE) {
        free_native_song(song);
        return BYTEBEAT_NATIVE_LOAD_QUEUE_FULL;
    }

    s_native_playback_enabled = true;
    return BYTEBEAT_NATIVE_LOAD_OK;
}

void bytebeat_engine_set_mode(int mode) {
    s_mode = mode;
}

void bytebeat_engine_set_speed(float speed) {
    s_speed_mult = speed;
}

void bytebeat_engine_set_sample_skip(int skip) {
    s_manual_sample_skip = skip;
}

void bytebeat_engine_set_paused(bool paused) {
    s_paused = paused;
    if (paused) {
        printf("[BYTEBEAT] Engine PAUSED - no more audio generation\n");
        s_start_time_us = 0; // Reset timer
    } else {
        printf("[BYTEBEAT] Engine RESUMED");
        if (s_duration_sec > 0) {
            printf(" (auto-stop in %lu seconds)\n", (unsigned long)s_duration_sec);
            s_start_time_us = esp_timer_get_time(); // Start timer
        } else {
            printf(" (infinite playback - no auto-stop)\n");
            s_start_time_us = 0;
        }
    }
}

void bytebeat_engine_set_duration(uint32_t duration_sec) {
    s_duration_sec = duration_sec;
    printf("[BYTEBEAT] Safety timer set to %lu seconds (0 = infinite)\n", (unsigned long)duration_sec);
}
