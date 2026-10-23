#pragma once

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Initialize the Serial command listener (UART).
 *
 * Starts a background task that reads serial commands from the host
 * and dispatches them to the bytebeat_engine and audio_manager.
 *
 * Commands:
 *   FORMULA:<formula>  Load and play a new formula
 *   V:<0-100>          Set speaker volume
 *   S:<0.01-16>        Set playback speed multiplier
 *   M:<0|1|2>          Set mode (Bytebeat / Floatbeat / Signed Bytebeat)
 *   HELP               Show available commands
 *
 * Example commands:
 *   FORMULA:sin(t/100)+cos(t/150)
 *   V:80
 *   M:1
 */
void serial_commands_init(void);

#ifdef __cplusplus
}
#endif
