#include "audio_manager.h"
#include <stdio.h>
#include <string.h>
#include "esp_log.h"
#include "esp_err.h"
#include "driver/i2c_master.h"
#include "driver/i2s_std.h"
#include "driver/gpio.h"
#include "esp_codec_dev.h"
#include "esp_codec_dev_defaults.h"
#include "es8311_codec.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/ringbuf.h"
#include <math.h>

static const char* TAG = "AUDIO_MANAGER";

static bool g_initialized = false;
static bool g_playback_running = false;

#define TX_RINGBUF_SIZE  32000
static RingbufHandle_t g_tx_ringbuf = NULL;
static TaskHandle_t g_tx_task_handle = NULL;

// I2C Master Bus Handle
static i2c_master_bus_handle_t g_i2c_bus_handle = NULL;

// I2S Handles
static i2s_chan_handle_t g_i2s_tx_handle = NULL;

// Codec Interfaces
static const audio_codec_ctrl_if_t *g_codec_ctrl_if = NULL;
static const audio_codec_data_if_t *g_codec_data_if = NULL;
static const audio_codec_gpio_if_t *g_codec_gpio_if = NULL;
static const audio_codec_if_t *g_codec_if = NULL;

static esp_err_t audio_i2c_init(void) {
    i2c_master_bus_config_t i2c_mst_config = {};
    i2c_mst_config.clk_source = I2C_CLK_SRC_DEFAULT;
    i2c_mst_config.i2c_port = I2C_NUM_0;
    i2c_mst_config.scl_io_num = (gpio_num_t)CONFIG_DAW_I2C_SCL_GPIO;
    i2c_mst_config.sda_io_num = (gpio_num_t)CONFIG_DAW_I2C_SDA_GPIO;
    i2c_mst_config.glitch_ignore_cnt = 7;
    i2c_mst_config.flags.enable_internal_pullup = true;
    
    ESP_LOGI(TAG, "Initializing I2C Master: SCL=%d, SDA=%d, Freq=400000Hz", 
             CONFIG_DAW_I2C_SCL_GPIO, CONFIG_DAW_I2C_SDA_GPIO);
             
    return i2c_new_master_bus(&i2c_mst_config, &g_i2c_bus_handle);
}

static esp_err_t audio_i2s_init(void) {
    ESP_LOGI(TAG, "Initializing I2S STD: MCLK=%d, BCLK=%d, WS=%d, DOUT=%d",
             CONFIG_DAW_I2S_MCLK_GPIO, CONFIG_DAW_I2S_BCLK_GPIO, 
             CONFIG_DAW_I2S_WS_GPIO, CONFIG_DAW_I2S_DOUT_GPIO);

    i2s_chan_config_t chan_cfg = I2S_CHANNEL_DEFAULT_CONFIG(I2S_NUM_0, I2S_ROLE_MASTER);
    chan_cfg.auto_clear = true;
    
    // Allocate TX channel only
    esp_err_t ret = i2s_new_channel(&chan_cfg, &g_i2s_tx_handle, NULL);
    if (ret != ESP_OK) return ret;
    
    i2s_std_config_t std_cfg = {
        .clk_cfg = I2S_STD_CLK_DEFAULT_CONFIG(CONFIG_DAW_SAMPLE_RATE),
        .slot_cfg = I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG((i2s_data_bit_width_t)16, I2S_SLOT_MODE_STEREO),
        .gpio_cfg = {
            .mclk = (gpio_num_t)CONFIG_DAW_I2S_MCLK_GPIO,
            .bclk = (gpio_num_t)CONFIG_DAW_I2S_BCLK_GPIO,
            .ws   = (gpio_num_t)CONFIG_DAW_I2S_WS_GPIO,
            .dout = (gpio_num_t)CONFIG_DAW_I2S_DOUT_GPIO,
            .din  = I2S_GPIO_UNUSED,
            .invert_flags = {
                .mclk_inv = false,
                .bclk_inv = false,
                .ws_inv   = false,
            },
        },
    };
    
    std_cfg.clk_cfg.clk_src = I2S_CLK_SRC_APLL; // High quality clock, required for stable MCLK
    
    // Initialize TX channel
    ret = i2s_channel_init_std_mode(g_i2s_tx_handle, &std_cfg);    
    return ret;
}

static void audio_tx_task(void *pvParameters) {
    while (1) {
        size_t size = 0;
        // Always drain the ringbuffer, regardless of g_playback_running,
        // so stop_playback() can safely wait for the buffer to empty.
        int16_t *data = (int16_t *)xRingbufferReceiveUpTo(g_tx_ringbuf, &size, portMAX_DELAY, 4000);
        if (data) {
            int samples = size / sizeof(int16_t);
            int16_t* stereo_buf = (int16_t*)malloc(samples * 2 * sizeof(int16_t));
            if (stereo_buf) {
                for (int i = 0; i < samples; i++) {
                    stereo_buf[i * 2] = data[i];
                    stereo_buf[i * 2 + 1] = data[i];
                }
                size_t bytes_written = 0;
                i2s_channel_write(g_i2s_tx_handle, stereo_buf, samples * 2 * sizeof(int16_t), &bytes_written, portMAX_DELAY);
                free(stereo_buf);
            }
            vRingbufferReturnItem(g_tx_ringbuf, (void *)data);
        }
    }
}


static esp_err_t audio_codec_init(void) {
    ESP_LOGI(TAG, "Initializing ES8311 codec");
    
    audio_codec_i2c_cfg_t i2c_cfg = {};
    i2c_cfg.port = I2C_NUM_0;
    i2c_cfg.addr = ES8311_CODEC_DEFAULT_ADDR;
    i2c_cfg.bus_handle = g_i2c_bus_handle;
    
    g_codec_ctrl_if = audio_codec_new_i2c_ctrl(&i2c_cfg);
    if (!g_codec_ctrl_if) return ESP_FAIL;
    
    audio_codec_i2s_cfg_t i2s_cfg = {};
    i2s_cfg.port = I2S_NUM_0;
    i2s_cfg.rx_handle = NULL;
    i2s_cfg.tx_handle = g_i2s_tx_handle;
    
    g_codec_data_if = audio_codec_new_i2s_data(&i2s_cfg);
    if (!g_codec_data_if) return ESP_FAIL;
    
    if (CONFIG_DAW_PA_PIN_GPIO >= 0) {
        g_codec_gpio_if = audio_codec_new_gpio();
    }
    
    es8311_codec_cfg_t es8311_cfg = {};
    es8311_cfg.ctrl_if = g_codec_ctrl_if;
    es8311_cfg.gpio_if = g_codec_gpio_if;
    es8311_cfg.codec_mode = ESP_CODEC_DEV_WORK_MODE_DAC;
    es8311_cfg.pa_pin = CONFIG_DAW_PA_PIN_GPIO;
    es8311_cfg.pa_reverted = false;
    es8311_cfg.master_mode = false;
    es8311_cfg.use_mclk = true;
    es8311_cfg.digital_mic = false;
    es8311_cfg.invert_mclk = false;
    es8311_cfg.invert_sclk = false;
    es8311_cfg.hw_gain.pa_voltage = 5.0;
    es8311_cfg.hw_gain.codec_dac_voltage = 3.3;
    
    g_codec_if = es8311_codec_new(&es8311_cfg);
    if (!g_codec_if) return ESP_FAIL;
    
    esp_codec_dev_sample_info_t fs = {};
    fs.bits_per_sample = 16;
    fs.channel = 2; // Match I2S Stereo mode
    fs.sample_rate = CONFIG_DAW_SAMPLE_RATE;
    
    // PERMANENTLY enable I2S channels to keep the master clock running at all times!
    // This prevents the 4-second DMA stall on the ESP32-P4.
    i2s_channel_enable(g_i2s_tx_handle);
    
    esp_err_t ret = g_codec_if->set_fs(g_codec_if, &fs);
    
    
    // Set speaker volume to default 50%
    audio_manager_set_speaker_vol(50);
    
    if (ret != ESP_OK) return ret;
    
    ESP_LOGI(TAG, "ES8311 codec initialized successfully (PLAYBACK mode)");
    return ESP_OK;
}

bool audio_manager_is_initialized(void) {
    return g_initialized;
}

esp_err_t audio_manager_init(void) {
    if (g_initialized) return ESP_OK;
    
    esp_err_t ret = audio_i2c_init();
    if (ret != ESP_OK) return ret;
    
    ret = audio_i2s_init();
    if (ret != ESP_OK) return ret;
    

    ret = audio_codec_init();
    if (ret != ESP_OK) return ret;
    
    g_tx_ringbuf = xRingbufferCreate(32000, RINGBUF_TYPE_BYTEBUF);
    if (!g_tx_ringbuf) {
        ESP_LOGE(TAG, "Failed to create TX ringbuf");
        return ESP_FAIL;
    }
    
    xTaskCreatePinnedToCore(audio_tx_task, "audio_tx", 4096, NULL, 5, &g_tx_task_handle, 1);
    
    g_initialized = true;
    ESP_LOGI(TAG, "=== Audio Manager Initialized Successfully ===");
    return ESP_OK;
}

esp_err_t audio_manager_deinit(void) {
    if (!g_initialized) return ESP_OK;
    
    if (g_codec_if) {
        g_codec_if->close(g_codec_if);
        g_codec_if = NULL;
    }
    if (g_codec_gpio_if) {
        audio_codec_delete_gpio_if(g_codec_gpio_if);
        g_codec_gpio_if = NULL;
    }
    if (g_codec_data_if) {
        audio_codec_delete_data_if(g_codec_data_if);
        g_codec_data_if = NULL;
    }
    if (g_codec_ctrl_if) {
        audio_codec_delete_ctrl_if(g_codec_ctrl_if);
        g_codec_ctrl_if = NULL;
    }

    if (g_i2s_tx_handle) {
        i2s_del_channel(g_i2s_tx_handle);
        g_i2s_tx_handle = NULL;
    }
    if (g_i2c_bus_handle) {
        i2c_del_master_bus(g_i2c_bus_handle);
        g_i2c_bus_handle = NULL;
    }
    
    g_initialized = false;
    ESP_LOGI(TAG, "Audio manager deinitialized");
    return ESP_OK;
}



esp_err_t audio_manager_set_speaker_vol(uint8_t vol_level) {
    if (vol_level > 100) return ESP_ERR_INVALID_ARG;
    if (!g_codec_if) return ESP_FAIL;

    // Match esp_codec_dev's native default curve: 0% is mute, 100% is 0 dB.
    // The ES8311 driver applies the configured fixed hardware-route gain.
    float db_val = (vol_level == 0) ? -96.0f : (-50.0f + (vol_level * 0.5f));
    esp_err_t ret = g_codec_if->set_vol(g_codec_if, db_val);
    if (ret == ESP_OK) {
        ESP_LOGI(TAG, "Speaker volume set to %d%% (%.1f dB)", vol_level, db_val);
    }
    return ret;
}


esp_err_t audio_manager_start_playback(void) {
    if (!g_initialized) return ESP_ERR_INVALID_STATE;
    if (g_playback_running) return ESP_OK;
    
    // Channels are already enabled permanently. Just enable the codec.
    esp_err_t ret = g_codec_if->enable(g_codec_if, true);
    if (ret != ESP_OK) return ret;
    
    // Re-apply volume to ensure DAC is not left muted by capture stop
    // audio_manager_set_speaker_vol(100);
    
    g_playback_running = true;
    ESP_LOGI(TAG, "Audio playback started");
    return ESP_OK;
}

esp_err_t audio_manager_stop_playback(void) {
    if (!g_playback_running) return ESP_OK;
    
    // Wait for ringbuffer to drain (max 3 seconds) before stopping codec.
    // This prevents the race condition where stop_playback is called before
    // audio_tx_task has finished playing buffered audio.
    uint32_t wait_ticks = 3000 / 10;
    while (xRingbufferGetCurFreeSize(g_tx_ringbuf) < TX_RINGBUF_SIZE && wait_ticks > 0) {
        vTaskDelay(pdMS_TO_TICKS(10));
        wait_ticks--;
    }
    
    g_playback_running = false;
    
    g_codec_if->enable(g_codec_if, false);
    
    ESP_LOGI(TAG, "Audio playback stopped");
    return ESP_OK;
}

int audio_manager_write(const int16_t* buffer, size_t samples, uint32_t timeout_ms) {
    if (!g_playback_running || !buffer || samples == 0) {
        return 0;
    }
    
    // Copy the mono samples into a fresh buffer to send to the queue
    int16_t* data = (int16_t*)malloc(samples * sizeof(int16_t));
    if (data) {
        memcpy(data, buffer, samples * sizeof(int16_t));
        
        if (xRingbufferSend(g_tx_ringbuf, (void *)data, samples * sizeof(int16_t), pdMS_TO_TICKS(timeout_ms)) != pdTRUE) {
            free(data);
            return 0;
        }
        return samples;
    }
    return 0;
}

int audio_manager_write_stereo(const int16_t* stereo_buffer, size_t frames, uint32_t timeout_ms) {
    if (!g_playback_running || !stereo_buffer || frames == 0) {
        return 0;
    }
    
    size_t bytes_written = 0;
    i2s_channel_write(g_i2s_tx_handle, stereo_buffer, frames * 2 * sizeof(int16_t), &bytes_written, pdMS_TO_TICKS(timeout_ms));
    return bytes_written / (2 * sizeof(int16_t));
}

void audio_manager_play_boot_sequence(void) {
    if (!g_initialized) return;
    
    // Play a professional "startup chime" (C-Major Arpeggio: C4, E4, G4, C5)
    const float freqs[] = {261.63f, 329.63f, 392.00f, 523.25f};
    const int duration_ms = 150; 
    const int sample_rate = 16000;
    const float amplitude = 8000.0f; // 16-bit max is 32767
    
    audio_manager_start_playback();
    
    for (int note = 0; note < 4; note++) {
        int total_samples = (sample_rate * duration_ms) / 1000;
        int16_t* buffer = (int16_t*)malloc(total_samples * sizeof(int16_t)); // Mono
        if (!buffer) break;
        
        for (int i = 0; i < total_samples; i++) {
            float t = (float)i / sample_rate;
            int16_t val = (int16_t)(amplitude * sin(2.0f * M_PI * freqs[note] * t));
            buffer[i] = val; 
        }
        
        audio_manager_write(buffer, total_samples, 1000);
        free(buffer);
    }
    
    audio_manager_stop_playback();
}
