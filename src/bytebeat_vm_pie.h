#pragma once

#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief PIE-optimized stereo buffer fill for ESP32-P4/S3
 * 
 * Uses SIMD instructions to fill stereo buffers 3-4x faster.
 * Only available on ESP32-S3 and ESP32-P4.
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
 * @brief Check if PIE optimization is available
 * 
 * @return true if running on ESP32-S3 or ESP32-P4
 */
bool bytebeat_pie_available(void);

/**
 * @brief Get chip name for debugging
 */
const char* bytebeat_get_chip_name(void);

#ifdef __cplusplus
}
#endif
