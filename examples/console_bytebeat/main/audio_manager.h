#pragma once

#include <stdint.h>
#include <stddef.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

// Max file path length for SPIFFS
#define MAX_FILE_PATH 300

/**
 * @brief Check if the audio subsystem is initialized
 */
bool audio_manager_is_initialized(void);

/**
 * @brief Initialize the audio I2C, I2S, and ES8311 Codec
 */
esp_err_t audio_manager_init(void);

/**
 * @brief Deinitialize the audio subsystem
 */
esp_err_t audio_manager_deinit(void);


/**
 * @brief Set the speaker output volume (hardware DAC gain)
 * 
 * @param vol_level 0-100 (percentage)
 */
esp_err_t audio_manager_set_speaker_vol(uint8_t vol_level);

/**
 * @brief Start continuous audio playback (I2S TX)
 */
esp_err_t audio_manager_start_playback(void);

/**
 * @brief Stop continuous audio playback
 */
esp_err_t audio_manager_stop_playback(void);

/**
 * @brief Write audio samples to the I2S TX channel
 * 
 * @param buffer Input buffer (MONO)
 * @param samples Number of samples to write
 * @param timeout_ms Timeout in milliseconds
 * @return Number of samples actually written
 */
int audio_manager_write(const int16_t* buffer, size_t samples, uint32_t timeout_ms);

/**
 * @brief Write stereo audio samples to the I2S TX channel
 * 
 * @param stereo_buffer Input buffer (INTERLEAVED STEREO, L R L R)
 * @param frames Number of stereo frames to write (length of buffer / 2)
 * @param timeout_ms Timeout in milliseconds
 * @return Number of frames actually written
 */
int audio_manager_write_stereo(const int16_t* stereo_buffer, size_t frames, uint32_t timeout_ms);

// ---------------------------------------------------------
// Sound Effects
// ---------------------------------------------------------

void audio_manager_play_boot_sequence(void);

#ifdef __cplusplus
}
#endif
