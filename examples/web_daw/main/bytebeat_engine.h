#pragma once

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Initialize the Bytebeat engine (QuickJS workers + coordinator).
 *
 * Creates dual-core QuickJS worker tasks and the audio coordinator task.
 * Must be called after audio_manager_init().
 */
void bytebeat_engine_init(void);

/**
 * @brief Load a new formula into the engine.
 *
 * The formula is compiled asynchronously by the QuickJS workers.
 * Passing "0" results in silence.
 *
 * @param formula  Null-terminated JS formula string (max 32767 chars).
 */
void bytebeat_engine_set_formula(const char* formula);

/**
 * @brief Set the playback mode.
 *
 * @param mode  0 = Classic Bytebeat (& 255), 1 = Floatbeat (-1..1), 2 = Signed Bytebeat
 */
void bytebeat_engine_set_mode(int mode);

/**
 * @brief Set the playback speed multiplier.
 *
 * @param speed  Speed multiplier (e.g. 0.1667 for 8kHz songs on a 48kHz DAC).
 */
void bytebeat_engine_set_speed(float speed);

/**
 * @brief Set manual sample skip (0 = auto DRS).
 *
 * @param skip  Number of samples to skip per evaluation (0 for automatic).
 */
void bytebeat_engine_set_sample_skip(int skip);

#ifdef __cplusplus
}
#endif
