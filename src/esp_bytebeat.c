#include "esp_bytebeat.h"
#include "bytebeat_vm.h"
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <freertos/queue.h>
#include <freertos/semphr.h>
#include <string.h>

#define AUDIO_FRAME_SAMPLES 128
#define BYTEBEAT_NATIVE_RATE 8000

typedef struct {
    bytebeat_program_t program;
    uint32_t duration_ms;
} bytebeat_msg_t;

static esp_bytebeat_config_t g_config;
static TaskHandle_t g_task_handle = NULL;
static QueueHandle_t g_queue = NULL;
static SemaphoreHandle_t g_task_stopped = NULL;
static bool g_running = false;

static void bytebeat_task(void *arg) {
    int16_t out_buffer[AUDIO_FRAME_SAMPLES];
    bytebeat_msg_t current_msg;
    
    // Internal playback state
    bool playing = false;
    uint32_t t_counter = 0;
    uint32_t samples_played = 0;
    uint32_t samples_total = 0;
    
    // Upsampling accumulator
    // t_inc is how much we increment t per output sample (Q16 fixed point)
    uint32_t t_inc_q16 = (BYTEBEAT_NATIVE_RATE << 16) / g_config.target_sample_rate;
    uint32_t t_accum_q16 = 0;

    // Fading parameters in Q15 fixed-point (1.0 = 32768)
    int32_t volume_q15 = 0;
    const int32_t fade_speed_q15 = 164; // Approx 0.005 * 32768

    while (g_running) {
        // Wait for a new equation if we are not playing
        TickType_t wait_ticks = playing ? 0 : pdMS_TO_TICKS(100);
        if (xQueueReceive(g_queue, &current_msg, wait_ticks) == pdTRUE) {
            if (current_msg.duration_ms == 0xFFFFFFFF) {
                // Special STOP message
                playing = false;
            } else {
                playing = true;
                t_counter = 0;
                t_accum_q16 = 0;
                samples_played = 0;
                if (current_msg.duration_ms == 0) {
                    samples_total = 0xFFFFFFFF; // Infinite
                } else {
                    samples_total = (current_msg.duration_ms * g_config.target_sample_rate) / 1000;
                }
            }
        }

        if (playing) {
            for (int i = 0; i < AUDIO_FRAME_SAMPLES; i++) {
                // Generate 8-bit sample using VM
                uint8_t sample_8 = bytebeat_execute(&current_msg.program, t_counter);
                
                // Convert 0-255 to signed 16-bit PCM (-32768 to 32767)
                int16_t sample_16 = (sample_8 - 128) * 256;
                
                // Fade logic (Anti-Click) in fixed-point
                if (samples_total != 0xFFFFFFFF && samples_played > samples_total - 200) {
                    // Fade out last 200 samples
                    volume_q15 -= fade_speed_q15;
                    if (volume_q15 < 0) volume_q15 = 0;
                } else {
                    // Fade in
                    if (volume_q15 < 32768) {
                        volume_q15 += fade_speed_q15;
                        if (volume_q15 > 32768) volume_q15 = 32768;
                    }
                }
                
                // Scale final volume by internal software setting
                int32_t final_vol_q15 = (volume_q15 * g_config.software_volume_q15) >> 15;
                out_buffer[i] = (int16_t)((sample_16 * final_vol_q15) >> 15);
                
                // Upsampling step
                t_accum_q16 += t_inc_q16;
                if (t_accum_q16 >= 0x10000) {
                    uint32_t steps = t_accum_q16 >> 16;
                    t_counter += steps;
                    t_accum_q16 &= 0xFFFF; // Keep fractional part
                }
                
                samples_played++;
                if (samples_total != 0xFFFFFFFF && samples_played >= samples_total) {
                    playing = false;
                    volume_q15 = 0;
                    // Fill rest of buffer with zeros
                    for (int j = i + 1; j < AUDIO_FRAME_SAMPLES; j++) out_buffer[j] = 0;
                    break;
                }
            }
            
            // Output to the user's callback
            if (g_config.write_cb) {
                g_config.write_cb(out_buffer, AUDIO_FRAME_SAMPLES * sizeof(int16_t));
            }
        }
    }
    
    xSemaphoreGive(g_task_stopped);
    vTaskDelete(NULL);
}

esp_err_t esp_bytebeat_init(const esp_bytebeat_config_t *config) {
    if (!config || !config->write_cb || config->target_sample_rate == 0) return ESP_ERR_INVALID_ARG;
    
    if (g_running) return ESP_ERR_INVALID_STATE;
    
    g_config = *config;
    g_queue = xQueueCreate(5, sizeof(bytebeat_msg_t));
    if (!g_queue) return ESP_ERR_NO_MEM;

    g_task_stopped = xSemaphoreCreateBinary();
    if (!g_task_stopped) {
        vQueueDelete(g_queue);
        g_queue = NULL;
        return ESP_ERR_NO_MEM;
    }
    
    g_running = true;
    
    BaseType_t core = config->task_core == 0xFFFFFFFF ? tskNO_AFFINITY : config->task_core;
    
    if (xTaskCreatePinnedToCore(bytebeat_task, "bytebeat_task", 4096, NULL, 
                                config->task_priority, &g_task_handle, core) != pdPASS) {
        vQueueDelete(g_queue);
        vSemaphoreDelete(g_task_stopped);
        g_task_stopped = NULL;
        g_queue = NULL;
        g_running = false;
        return ESP_FAIL;
    }
    
    return ESP_OK;
}

esp_err_t esp_bytebeat_deinit(void) {
    if (!g_running) return ESP_ERR_INVALID_STATE;
    
    g_running = false;
    if (g_task_stopped && xSemaphoreTake(g_task_stopped, pdMS_TO_TICKS(1000)) != pdTRUE) {
        return ESP_ERR_TIMEOUT;
    }
    
    if (g_task_handle) {
        g_task_handle = NULL;
    }
    if (g_queue) {
        vQueueDelete(g_queue);
        g_queue = NULL;
    }
    if (g_task_stopped) {
        vSemaphoreDelete(g_task_stopped);
        g_task_stopped = NULL;
    }
    
    return ESP_OK;
}

esp_err_t esp_bytebeat_play_string(const char* equation, uint32_t duration_ms) {
    if (!g_running) return ESP_ERR_INVALID_STATE;
    if (!equation) return ESP_ERR_INVALID_ARG;
    
    bytebeat_msg_t msg;
    msg.duration_ms = duration_ms;
    
    if (!bytebeat_compile(equation, &msg.program)) {
        return ESP_ERR_INVALID_ARG; // Parsing failed
    }
    
    if (xQueueSend(g_queue, &msg, 0) != pdTRUE) {
        return ESP_FAIL; // Queue full
    }
    
    return ESP_OK;
}

esp_err_t esp_bytebeat_stop(void) {
    if (!g_running) return ESP_ERR_INVALID_STATE;
    
    bytebeat_msg_t msg;
    msg.duration_ms = 0xFFFFFFFF; // Stop signal
    
    xQueueSend(g_queue, &msg, portMAX_DELAY);
    return ESP_OK;
}
