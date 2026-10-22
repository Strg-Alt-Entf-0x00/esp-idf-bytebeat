# UART Filebridge

The developer example integrates the external `esp-uart-filebridge` component. It exposes the example's SD-card files to a host PC over UART, which is useful for retrieving recorded WAV audio and E32T frame telemetry for offline analysis. It is a development and data-transfer path, not part of the speech-synthesis API.

## Example Configuration

In `console_tts_dev`, the filebridge uses UART1 at 3,000,000 baud and mounts the SD card at `/sd` when initialization and card mounting succeed. The configured values are reported by the firmware at startup; check the board wiring and active configuration rather than assuming a host COM port. The example's dependency and setup are documented in [its README](../../examples/console_tts_dev/README.md) and [component manifest](../../examples/console_tts_dev/main/idf_component.yml).

## Host Transfer

Use the Python CLI supplied by the filebridge component. Run it in an environment where that CLI and its dependencies are available. Replace `COMx` with the host serial port connected to the filebridge UART; the example's baud rate is 3,000,000:

```powershell
python -m esp_idf_uart_filebridge.cli --port COMx --baud 3000000 ls /sd/telemetry
python -m esp_idf_uart_filebridge.cli --port COMx --baud 3000000 download /sd/telemetry/aufnahme_0001.wav downloads/aufnahme_0001.wav
python -m esp_idf_uart_filebridge.cli --port COMx --baud 3000000 download /sd/telemetry/aufnahme_0001.e32t downloads/aufnahme_0001.e32t
```

Create the local `downloads` directory before downloading if it does not exist. Close any other program using the filebridge serial port while transferring files. Exact CLI options may depend on the installed filebridge version; consult that component's documentation if they differ.

The in-progress unified [ESPormant32 DevTools](../../tools_py/ai_developer/cli.py) can list the telemetry directory or download WAV/E32T/CSV captures. It compares each downloaded file size with the remote listing and records its SHA-256 checksum in the session manifest. The filebridge connection is separate from the TTS console connection and must use the port and baud configured for the bridge on the target.

```powershell
python -m tools_py.ai_developer device files list --filebridge-port COM13 --filebridge-baud 3000000
python -m tools_py.ai_developer device files download --filebridge-port COM13 --filebridge-baud 3000000
```

The current DevTools implementation saves downloads under the session's
`tools_py/data/runs/<UTC-timestamp>_<label>/captures/` directory. The intended
persistent archive is
`tools_py/ai_developer/telemetry_data/filebridge_downloads/`; wiring the
current download workflow to that archive is still future work. The target
filename format is
`YYYY-MM-DD_HH-mm-ss_<remote-basename>.<extension>`, using one UTC timestamp
for the entire download batch and preserving the remote basename and
extension. The timestamp means download time, not device recording time.
Never overwrite an earlier capture; record any collision suffix and the final
archive path in the session manifest together with the remote path, size, and
SHA-256 checksum.

Older local files under
`tools_py/ai_developer/telemetry_data/telemetry_dump/` are retained as
historical examples and are not the new archive. Do not rename or overwrite
them as part of routine Filebridge downloads.

To synthesize text and fetch captures as one traceable session, first configure both ports in `tools_py/config.toml`, then run:

```powershell
python -m tools_py.ai_developer session run --speak "Guten Tag" --download
```

Alternatively, provide the settings in a local TOML file and select it with the global `--config` option before `session run`. See the [development tools overview](DEVELOPMENT_TOOLS.md) for the configuration format, timestamped session layout, and console baud-rate considerations.

## Analyze Captures

The E32T file contains frame-level control telemetry. The WAV capture contains the digital PCM written by the developer example to its audio output path; it does not measure the analog codec, amplifier, speaker, or room response. See the [telemetry format and workflow](../../examples/console_tts_dev/components/telemetry/README.md) for capture details.

Analysis tools will be documented separately as they are implemented. Use comparable input text, profile, build, and capture settings when evaluating changes. A single capture or analysis result is not sufficient to establish a perceptual improvement.
