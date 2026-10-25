#include "bytebeat_vm_pie.h"
#include <string.h>

// Chip detection
bool bytebeat_pie_available(void) {
#if defined(CONFIG_IDF_TARGET_ESP32P4) || defined(CONFIG_IDF_TARGET_ESP32S3)
    return true;
#else
    return false;
#endif
}

const char* bytebeat_get_chip_name(void) {
#if defined(CONFIG_IDF_TARGET_ESP32P4)
    return "ESP32-P4";
#elif defined(CONFIG_IDF_TARGET_ESP32S3)
    return "ESP32-S3";
#elif defined(CONFIG_IDF_TARGET_ESP32S2)
    return "ESP32-S2";
#elif defined(CONFIG_IDF_TARGET_ESP32C3)
    return "ESP32-C3";
#elif defined(CONFIG_IDF_TARGET_ESP32C6)
    return "ESP32-C6";
#elif defined(CONFIG_IDF_TARGET_ESP32H2)
    return "ESP32-H2";
#elif defined(CONFIG_IDF_TARGET_ESP32)
    return "ESP32";
#else
    return "Unknown";
#endif
}

// Legacy-named portable C stereo fill, available to ESP32-P4/S3 builds.
#if defined(CONFIG_IDF_TARGET_ESP32P4) || defined(CONFIG_IDF_TARGET_ESP32S3)

void bytebeat_fill_stereo_pie(int16_t *out_buf, int16_t left, int16_t right, int count) {
    // Keep the implementation portable; target-specific PIE code is not used.
    int i = 0;
    for (; i < (count & ~3); i += 4) {
        out_buf[i * 2 + 0] = left;
        out_buf[i * 2 + 1] = right;
        out_buf[i * 2 + 2] = left;
        out_buf[i * 2 + 3] = right;
        out_buf[i * 2 + 4] = left;
        out_buf[i * 2 + 5] = right;
        out_buf[i * 2 + 6] = left;
        out_buf[i * 2 + 7] = right;
    }
    
    // Handle remaining samples
    for (; i < count; i++) {
        out_buf[i * 2 + 0] = left;
        out_buf[i * 2 + 1] = right;
    }
}

#endif // ESP32-P4 or ESP32-S3
