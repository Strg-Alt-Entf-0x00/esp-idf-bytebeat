#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Initialize the Bytebeat engine (C bytecode workers + coordinator).
 *
 * Creates dual-core C bytecode VM worker tasks and the audio coordinator task.
 * Must be called after audio_manager_init().
 */
void bytebeat_engine_init(void);

/**
 * @brief Load a new formula into the engine.
 *
 * The formula is compiled asynchronously by the C bytecode workers.
 * Passing "0" results in silence.
 *
 * @param formula  Null-terminated C formula string (max 32767 chars).
 *                 Example: "sin(t / 100) + cos(t / 150)"
 */
void bytebeat_engine_set_formula(const char* formula);

/**
 * @brief Check whether a formula is supported by the device compiler.
 *
 * This does not change the active formula or playback state.
 */
bool bytebeat_engine_validate_formula(const char* formula);

typedef enum {
    BYTEBEAT_NATIVE_LOAD_OK,
    BYTEBEAT_NATIVE_LOAD_INVALID_ARGUMENT,
    BYTEBEAT_NATIVE_LOAD_NOT_READY,
    BYTEBEAT_NATIVE_LOAD_NO_MEMORY,
    BYTEBEAT_NATIVE_LOAD_INVALID_BYTECODE,
    BYTEBEAT_NATIVE_LOAD_VM_UNSUPPORTED,
    BYTEBEAT_NATIVE_LOAD_PREFLIGHT_FAILED,
    BYTEBEAT_NATIVE_LOAD_QUEUE_FULL
} bytebeat_native_load_result_t;

/**
 * @brief Validate and queue a compiled native Bytebeat program.
 *
 * The byte buffer can be released as soon as this function returns.
 */
bytebeat_native_load_result_t bytebeat_engine_set_native_bytecode(
    const uint8_t *data,
    size_t length);

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

/**
 * @brief Pause/resume the bytebeat engine.
 *
 * When paused, the engine stops generating audio and the worker tasks sleep.
 *
 * @param paused  true = pause, false = resume
 */
void bytebeat_engine_set_paused(bool paused);

/**
 * @brief Set safety timer for automatic playback stop.
 *
 * After the specified duration, the engine will automatically pause.
 * This prevents endless playback from running forever.
 *
 * @param duration_sec  Duration in seconds (0 = infinite/no auto-stop)
 */
void bytebeat_engine_set_duration(uint32_t duration_sec);

#ifdef __cplusplus
}
#endif
