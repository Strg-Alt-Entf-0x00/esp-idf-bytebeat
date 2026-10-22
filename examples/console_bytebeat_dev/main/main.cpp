#include <stdio.h>
#include "audio_manager.h"
#include "bytebeat_engine.h"
#include "serial_commands.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

extern "C" void app_main(void)
{
    printf("==============================================\n");
    printf("ESP32 Bytebeat Console v2.0\n");
    printf("Pure C Engine - No JavaScript!\n");
    printf("==============================================\n");
    printf("System started in PAUSED mode.\n");
    printf("Use the Python tool to load formulas:\n");
    printf("  python bytebeat_tool.py list\n");
    printf("  python bytebeat_tool.py play <index> --port COM4\n");
    printf("==============================================\n\n");

    // 1. Initialize audio hardware (I2C, I2S, ES8311 Codec)
    esp_err_t err = audio_manager_init();
    if (err != ESP_OK) {
        printf("Audio init failed: %d\n", (int)err);
        return;
    }

    // 2. Set initial volume and start I2S playback
    audio_manager_set_speaker_vol(100);
    audio_manager_start_playback();

    // 3. Start the Serial command listener (UART)
    serial_commands_init();

    // 4. Start the dual-core C Bytebeat engine (starts PAUSED)
    bytebeat_engine_init();
    
    printf("\n[INFO] System ready. Use serial commands to load formulas.\n");
    printf("[INFO] System starts in PAUSED mode.\n\n");
}
