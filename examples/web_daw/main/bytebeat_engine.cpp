#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "esp_timer.h"
#include "esp_heap_caps.h"
#include "quickjs.h"
#include "audio_manager.h"
#include "bytebeat_engine.h"

// ---------------------------------------------------------------------------
// Internal State
// ---------------------------------------------------------------------------
static volatile int s_manual_sample_skip = 0;
static volatile float s_speed_mult = 1.0f;
static volatile int s_mode = 1; // Default Floatbeat
static char s_formula[32768] = "0";
static volatile bool s_flag_reload = false;

// ---------------------------------------------------------------------------
// Stringify helper for Kconfig integer values
// ---------------------------------------------------------------------------
#define STRINGIFY_HELPER(x) #x
#define STRINGIFY(x) STRINGIFY_HELPER(x)

// ---------------------------------------------------------------------------
// QuickJS Wrapper Script
// ---------------------------------------------------------------------------
static const char* wrapper_script =
"Object.getOwnPropertyNames(Math).forEach(k => globalThis[k] = Math[k]);\n"
"globalThis.int = Math.floor;\n"
"globalThis.window = globalThis;\n"
"var CHUNK_SIZE = " STRINGIFY(CONFIG_DAW_CHUNK_SIZE) ";\n"
"let __out_buffer = new Float64Array(CHUNK_SIZE * 2);\n"
"function __generate_chunk(start_t, skip, speed_mult, mode) {\n"
"    for (let i = 0; i < CHUNK_SIZE; i += skip) {\n"
"        let t = start_t + (i * speed_mult);\n"
"        if (mode === 0 || mode === 2) t = Math.floor(t);\n"
"        let v = audio(t);\n"
"        let l = 0, r = 0;\n"
"        if (Array.isArray(v)) {\n"
"            l = (typeof v[0] === 'number' && !isNaN(v[0])) ? v[0] : 0;\n"
"            r = (typeof v[1] === 'number' && !isNaN(v[1])) ? v[1] : 0;\n"
"        } else {\n"
"            l = typeof v === 'number' && !isNaN(v) ? v : 0;\n"
"            r = l;\n"
"        }\n"
"        for (let j = 0; j < skip && (i + j) < CHUNK_SIZE; j++) {\n"
"            __out_buffer[(i + j) * 2] = l;\n"
"            __out_buffer[(i + j) * 2 + 1] = r;\n"
"        }\n"
"    }\n"
"    return __out_buffer.buffer;\n"
"}\n";

// ---------------------------------------------------------------------------
// Job Queue Structure
// ---------------------------------------------------------------------------
struct JSJob {
    double start_t;
    int16_t* out_buf;
    bool is_reload;
};

static QueueHandle_t q_worker_in[2];
static QueueHandle_t q_worker_out[2];

// ---------------------------------------------------------------------------
// JS Interrupt Handler (prevents TWDT on heavy formulas)
// ---------------------------------------------------------------------------
static int js_interrupt_handler(JSRuntime *rt, void *opaque) {
    return 0;
}

// ---------------------------------------------------------------------------
// Formula compilation helper
// ---------------------------------------------------------------------------
static void compile_formula(JSContext *ctx, int core_id) {
    size_t full_len = strlen(s_formula) + 256;
    char* full_script = (char*)malloc(full_len);
    if (full_script == NULL) {
        printf("[Core %d] Failed to allocate script buffer\n", core_id);
        return;
    }

    if (strstr(s_formula, "function audio") != NULL ||
        strstr(s_formula, "audio = function") != NULL) {
        snprintf(full_script, full_len, "%s", s_formula);
    } else {
        bool is_funcbeat = (strstr(s_formula, "return ") != NULL ||
                            strstr(s_formula, ";") != NULL);
        if (is_funcbeat) {
            snprintf(full_script, full_len, "audio = (function() {\n%s\n})();", s_formula);
        } else {
            snprintf(full_script, full_len, "function audio(t) { return (\n%s\n); }", s_formula);
        }
    }

    JSValue result = JS_Eval(ctx, full_script, strlen(full_script), "<eval>", JS_EVAL_TYPE_GLOBAL);
    if (JS_IsException(result)) {
        JSValue exc = JS_GetException(ctx);
        const char* exc_str = JS_ToCString(ctx, exc);
        printf("[Core %d] JS compile error: %s\n", core_id, exc_str ? exc_str : "Unknown");
        if (exc_str) JS_FreeCString(ctx, exc_str);
        JS_FreeValue(ctx, exc);
    }
    JS_FreeValue(ctx, result);
    free(full_script);
}

// ---------------------------------------------------------------------------
// QuickJS Worker Task (runs on each core)
// ---------------------------------------------------------------------------
static void quickjs_worker_task(void *pvParameter) {
    int core_id = (int)(intptr_t)pvParameter;
    const int CHUNK_SIZE = CONFIG_DAW_CHUNK_SIZE;

    JSRuntime *rt = JS_NewRuntime();
    JS_SetInterruptHandler(rt, js_interrupt_handler, NULL);
    JSContext *ctx = JS_NewContext(rt);

    // Initialize wrapper (Math aliases, chunk generator)
    JSValue wr = JS_Eval(ctx, wrapper_script, strlen(wrapper_script), "<wrapper>", JS_EVAL_TYPE_GLOBAL);
    JS_FreeValue(ctx, wr);

    // Compile initial formula
    compile_formula(ctx, core_id);

    JSValue global_obj = JS_GetGlobalObject(ctx);
    JSValue gen_func = JS_GetPropertyStr(ctx, global_obj, "__generate_chunk");

    static volatile int s_sample_skip = 1;
    static volatile int s_chunk_counter = 0;

    while (1) {
        JSJob job;
        xQueueReceive(q_worker_in[core_id], &job, portMAX_DELAY);

        if (job.is_reload) {
            printf("[Core %d] Compiling new formula...\n", core_id);
            compile_formula(ctx, core_id);
            xQueueSend(q_worker_out[core_id], &job, portMAX_DELAY);
            continue;
        }

        s_chunk_counter = s_chunk_counter + 1;
        int current_chunk = s_chunk_counter;
        int skip = (s_manual_sample_skip > 0) ? s_manual_sample_skip : s_sample_skip;

        JSValue args[4];
        args[0] = JS_NewFloat64(ctx, job.start_t);
        args[1] = JS_NewInt32(ctx, skip);
        args[2] = JS_NewFloat64(ctx, s_speed_mult);
        args[3] = JS_NewInt32(ctx, s_mode);

        int64_t t1 = esp_timer_get_time();
        JSValue ret_buffer = JS_Call(ctx, gen_func, global_obj, 4, args);

        // Prevent IDLE task starvation
        if (current_chunk % 10 == 0) {
            vTaskDelay(pdMS_TO_TICKS(1));
        }
        // Periodic GC
        if (current_chunk % 20 == 0) {
            JS_RunGC(rt);
        }

        int64_t t2 = esp_timer_get_time();
        int duration_ms = (int)((t2 - t1) / 1000);

        JS_FreeValue(ctx, args[0]);
        JS_FreeValue(ctx, args[1]);
        JS_FreeValue(ctx, args[2]);
        JS_FreeValue(ctx, args[3]);

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

        // Convert JS output to I2S samples
        if (!JS_IsException(ret_buffer)) {
            size_t size;
            uint8_t *buf_ptr = JS_GetArrayBuffer(ctx, &size, ret_buffer);
            if (buf_ptr && size == (size_t)(CHUNK_SIZE * 2 * sizeof(double))) {
                double *d_buf = (double*)buf_ptr;
                for (int i = 0; i < CHUNK_SIZE * 2; i++) {
                    double val = d_buf[i];
                    if (s_mode == 0) {
                        int iv = (int)val;
                        iv = iv & 255;
                        val = (iv - 128) * 256.0;
                    } else if (s_mode == 1) {
                        val = val * 32767.0;
                    } else if (s_mode == 2) {
                        int iv = (int)val;
                        iv = (int8_t)(iv & 255);
                        val = iv * 256.0;
                    }
                    if (val > 32767.0) val = 32767.0;
                    if (val < -32768.0) val = -32768.0;
                    job.out_buf[i] = (int16_t)val;
                }
            }
        } else {
            JSValue exc = JS_GetException(ctx);
            const char* exc_str = JS_ToCString(ctx, exc);
            printf("[Core %d] JS runtime error: %s\n", core_id, exc_str ? exc_str : "Unknown");
            if (exc_str) JS_FreeCString(ctx, exc_str);
            JS_FreeValue(ctx, exc);
            memset(job.out_buf, 0, CHUNK_SIZE * 2 * sizeof(int16_t));
        }

        JS_FreeValue(ctx, ret_buffer);
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
        JSJob j = {t, b, false};
        t += CHUNK_SIZE * s_speed_mult;
        xQueueSend(q_worker_in[i % 2], &j, portMAX_DELAY);
    }

    printf("Coordinator: %d PSRAM buffers allocated, rendering started\n", NUM_BUFFERS);

    bool needs_reload_core0 = false;
    bool needs_reload_core1 = false;
    int worker_idx = 0;

    while (1) {
        if (s_flag_reload) {
            s_flag_reload = false;
            printf("Coordinator: reloading formula\n");
            t = 0.0;
            needs_reload_core0 = true;
            needs_reload_core1 = true;
        }

        JSJob done_job;

        // Wait with timeout to prevent I2S DMA underflow buzzing
        while (xQueueReceive(q_worker_out[worker_idx], &done_job, pdMS_TO_TICKS(10)) != pdTRUE) {
            static int16_t silence_buf[1024] = {0};
            audio_manager_write_stereo(silence_buf, CHUNK_SIZE, portMAX_DELAY);
        }

        if (!done_job.is_reload) {
            // Force true silence when formula is "0" (prevents DC offset in Bytebeat mode)
            if (s_formula[0] == '0' && s_formula[1] == '\0') {
                memset(done_job.out_buf, 0, CHUNK_SIZE * 2 * sizeof(int16_t));
            }
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

    q_worker_in[0]  = xQueueCreate(NUM_BUFFERS / 2, sizeof(JSJob));
    q_worker_out[0] = xQueueCreate(NUM_BUFFERS / 2, sizeof(JSJob));
    q_worker_in[1]  = xQueueCreate(NUM_BUFFERS / 2, sizeof(JSJob));
    q_worker_out[1] = xQueueCreate(NUM_BUFFERS / 2, sizeof(JSJob));

    xTaskCreatePinnedToCore(quickjs_worker_task, "js_worker_0", 65536,
                            (void*)0, 4, NULL, CONFIG_DAW_JS_CORE_ID);
    xTaskCreatePinnedToCore(quickjs_worker_task, "js_worker_1", 65536,
                            (void*)1, 4, NULL, 1 - CONFIG_DAW_JS_CORE_ID);

    xTaskCreatePinnedToCore(coordinator_task, "js_coord", 4096,
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

void bytebeat_engine_set_sample_skip(int skip) {
    s_manual_sample_skip = skip;
}
