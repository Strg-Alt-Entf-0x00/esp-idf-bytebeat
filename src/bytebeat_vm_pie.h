#pragma once

#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Legacy-named stereo buffer fill gated to ESP32-P4/S3 builds.
 *
 * The current implementation is portable C with a manually unrolled loop; it
 * does not issue PIE/SIMD instructions and has no measured speedup.
 * 
 * @param out_buf Output buffer (interleaved stereo int16)
 * @param left Left channel value
 * @param right Right channel value
 * @param count Number of stereo samples
 */
#if defined(CONFIG_IDF_TARGET_ESP32P4) || defined(CONFIG_IDF_TARGET_ESP32S3)
void bytebeat_fill_stereo_pie(int16_t *out_buf, int16_t left, int16_t right, int count);
#endif

/**
 * @brief Check whether the build target is ESP32-S3 or ESP32-P4.
 * 
 * @return true for those target builds. This reports target capability only;
 *         it does not indicate that the current buffer-fill implementation
 *         uses PIE/SIMD instructions.
 */
bool bytebeat_pie_available(void);

/**
 * @brief Get chip name for debugging
 */
const char* bytebeat_get_chip_name(void);

#ifdef __cplusplus
}
#endif
