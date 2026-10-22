# ESPormant32 Integration

ESPormant32 is an ESP-IDF component for parametric formant speech synthesis.
It provides the synthesis engine and an adapter to the separate ESPure G2P
component. The consuming application remains responsible for board-specific
audio and codec initialization, task scheduling, and writing generated PCM to
its output device.

## Component responsibilities

- **ESPure G2P** converts UTF-8 text into pronunciation output and owns the
  language resources and G2P algorithms.
- **ESPormant32** adapts G2P output to its phoneme IDs, applies its
  application-side pronunciation/frame processing, and provides Klatt-style
  synthesis and DSP.
- **The application** selects text and voice settings, initializes board
  peripherals, schedules synthesis, and sends generated samples to the audio
  sink.

Keep fixes in the repository that owns the behavior. Do not patch or copy
ESPure sources under `managed_components/` or maintain an ESPormant-specific
fork to work around an ESPure defect. If an integration failure is traced to
ESPure, record a reproducible input, output/error, dependency revision, and
build configuration; fix the G2P-owned issue in the ESPure repository and
then validate the consuming application against that revision. Adapter bugs
belong in ESPormant32.

## Requirements and dependency setup

The root [idf_component.yml](../idf_component.yml) declares ESP-IDF and
ESPure dependencies. Its `override_path` is for local sibling-repository
development; it must be removed or changed to a valid path for another
checkout. For a registry build, use a released ESPure version constraint
instead.

The reference application manifests are:

- [console_tts](../examples/console_tts/main/idf_component.yml)
- [console_tts_dev](../examples/console_tts_dev/main/idf_component.yml)

For a local component dependency, the ESP-IDF component name is
`esp32-p4-idf-espormant32`; its CMake registration declares the ESPure
component as `esp-idf-espure-g2p`. A consuming component can require
ESPormant32 as follows:

```cmake
idf_component_register(
    SRCS "main.cpp"
    INCLUDE_DIRS "."
    REQUIRES esp32-p4-idf-espormant32
)
```

The consuming application may also require board-specific components such
as its audio HAL, codec driver, console, GPIO, or NVS components.

## Configure ESPure locales

ESPure language data is selected at build time. Enable every locale the
application may select, for example in the consuming project's
`sdkconfig.defaults`:

```text
CONFIG_ESPURE_LOCALE_DE_DE=y
```

The optional `en-GB` locale is an architecture pilot with limited vocabulary,
not full English support. The ESPormant32 reference application currently
enables the German locale and compressed Huffman dictionary:

```text
CONFIG_ESPURE_DICT_HUFFMAN_TRIE=y
```

The selected dictionary and language resources are embedded in the firmware
image. ESPormant32 does not load or replace ESPure dictionaries from the
UART Filebridge or SD card at runtime. See the
[ESPure integration guide](../../esp-idf-espure-g2p/documentation/INTEGRATION.md)
for locale and dictionary configuration details.

## G2P adapter and TTS data flow

The adapter implementation is in
[`espure_g2p.cpp`](../src/g2p/espure/espure_g2p.cpp). It initializes an
ESPure handle for the selected locale, calls `espure_phonemize()` for each
word, and maps the returned Kirshenbaum-style output into ESPormant32
`PhonemeNode` values. Locale changes are accepted only when that locale was
compiled into the application.

The current adapter uses the rendered string API; it does not currently
consume `espure_phonemize_stream()`'s structured segment output. Treat this
as an integration detail, not as a stable cross-component file or wire
format. Changes to that data flow should be made and tested explicitly in
the ESPormant32 adapter.

The runnable reference orchestration is in:

- [`tts_app.cpp`](../examples/console_tts/main/tts_app.cpp): text and markup
  processing, G2P conversion, and preparation of synthesis frames.
- [`main.cpp`](../examples/console_tts/main/main.cpp): application startup,
  board/audio setup, task scheduling, and PCM output.
- [`espure_g2p.cpp`](../src/g2p/espure/espure_g2p.cpp): ESPure adapter.
- [`klatt_synth.h`](../src/dsp/klatt_synth.h): synthesizer interface.

## Build the reference applications

The user-facing console is the clean end-to-end TTS reference:

```powershell
Set-Location examples\console_tts
idf.py set-target esp32p4
idf.py build
```

The developer variant adds its display, telemetry, and UART Filebridge setup:

```powershell
Set-Location examples\console_tts_dev
idf.py set-target esp32p4
idf.py build
```

For board assumptions and commands, see the
[console_tts README](../examples/console_tts/README.md) and
[console_tts_dev README](../examples/console_tts_dev/README.md). The
[UART Filebridge guide](development_tools/FILEBRIDGE.md) documents the
development transfer workflow.
