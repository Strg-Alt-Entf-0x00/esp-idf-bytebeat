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
 * Protocol:
 *   V:<0-100>         Set speaker volume
 *   S:<float>         Set speed multiplier
 *   M:<0|1|2>         Set mode (Bytebeat / Floatbeat / Signed Bytebeat)
 *   D:<int>           Set manual sample skip (0 = auto DRS)
 *   P:<0|1>           Pause (0) / Resume (1) playback
 *   FORMULA:<formula> Load and play a new formula (auto-resumes)
 *   BBLOAD:<bytes>    Start a native bytecode transfer
 *   BBDATA:<offset>:<hex> Append the next contiguous bytecode chunk
 *   BBEND             Validate and play the transferred program
 *   BYTEBEAT:BB:...   Transfer readiness, progress, and result responses
 *   STOP              Pause playback
 *   VERSION           Get firmware version (for handshake)
 *   INFO              Get detailed device info
 *
 * Example commands:
 *   FORMULA:sin(t/100)+cos(t/150)
 *   V:80
 *   S:1.0
 *   M:1
 *   P:1
 *   STOP
 */
void serial_commands_init(void);

#ifdef __cplusplus
}
#endif
