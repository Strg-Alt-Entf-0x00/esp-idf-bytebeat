#pragma once

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Initialize the Web Serial (UART/USB) command listener.
 *
 * Starts a background task that reads serial commands from the host
 * and dispatches them to the bytebeat_engine and audio_manager.
 *
 * Protocol:
 *   V:<0-100>       Set speaker volume
 *   S:<float>       Set speed multiplier
 *   M:<0|1|2>       Set mode (Bytebeat / Floatbeat / Signed Bytebeat)
 *   D:<int>         Set manual sample skip (0 = auto DRS)
 *   C:<formula>     Load and play a new formula
 */
void web_serial_init(void);

#ifdef __cplusplus
}
#endif
