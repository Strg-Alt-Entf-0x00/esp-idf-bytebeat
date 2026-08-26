#pragma once

#include <stdint.h>
#include <stddef.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Output callback for writing 16-bit PCM audio.
 * 
 * This decouples the engine from specific I2S/codec implementations.
 * 
 * @param data Pointer to the generated 16-bit signed PCM samples.
 * @param num_bytes Number of BYTES to write.
 * @return size_t Number of bytes successfully written.
 */
typedef size_t (*bytebeat_write_cb_t)(const int16_t *data, size_t num_bytes);

/**
 * @brief Configuration for the esp-bytebeat engine.
 */
typedef struct {
    uint32_t target_sample_rate;    ///< The sample rate of the output (e.g., 16000, 22050). The engine will upsample the 8000Hz bytebeat to this rate.
    uint32_t task_priority;         ///< FreeRTOS task priority for the engine.
    uint32_t task_core;             ///< Core ID to pin the task to (0, 1, or tskNO_AFFINITY).
    uint32_t software_volume_q15;   ///< Internal software volume multiplier (0 = mute, 32768 = 100%).
    bytebeat_write_cb_t write_cb;   ///< Callback to write audio data.
} esp_bytebeat_config_t;

/**
 * @brief Initializes the Bytebeat engine.
 * 
 * @param config Pointer to configuration struct.
 * @return esp_err_t ESP_OK on success.
 */
esp_err_t esp_bytebeat_init(const esp_bytebeat_config_t *config);

/**
 * @brief Deinitializes the engine and frees resources.
 */
esp_err_t esp_bytebeat_deinit(void);

/**
 * @brief Plays a Bytebeat formula from a string expression.
 * 
 * Compiles the string into RPN bytecode and plays it in the background task.
 * Supports operators: +, -, *, /, %, &, |, ^, <<, >>
 * Variables: t
 * 
 * @param equation String containing the equation (e.g. "(t*5 & t>>7) | (t*3 & t>>10)")
 * @param duration_ms Duration to play the sound in milliseconds. (0 = infinite)
 * @return esp_err_t ESP_OK if compilation and queuing were successful.
 */
esp_err_t esp_bytebeat_play_string(const char* equation, uint32_t duration_ms);

/**
 * @brief Immediately stops the currently playing sound (with a quick fade-out).
 * 
 * @return esp_err_t ESP_OK.
 */
esp_err_t esp_bytebeat_stop(void);

#ifdef __cplusplus
}
#endif
