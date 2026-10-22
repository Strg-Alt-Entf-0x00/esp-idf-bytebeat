#include <stdio.h>
#include "audio_manager.h"
#include "bytebeat_engine.h"
#include "serial_commands.h"

extern "C" void app_main(void)
{
    printf("Booting ESP Bytebeat Console v2.0...\n");
    printf("Pure C Engine - No JavaScript!\n\n");

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

    // 4. Start the dual-core C Bytebeat engine
    bytebeat_engine_init();
}
