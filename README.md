# ESP Bytebeat

Reusable procedural audio and Bytebeat components for ESP-IDF, with a hardware-specific Web DAW example for ESP32-P4.

## Components

The repository root is the publishable `esp_bytebeat` component. It provides a small native C bytecode engine and an output callback API. It has no dependency on QuickJS, codecs, I2S, or a particular board.

The native engine supports:
- **Unsigned Bytebeat** (`0..255`)
- **Signed Bytebeat** (`-128..127`) 
- **Floatbeat** (`-1.0..1.0`)
- **Math functions**: `sin`, `cos`, `tan`, `sqrt`, `abs`, `floor`, `ceil`, `round`, `log`, `exp`, `pow`, `min`, `max`
- **Operators**: `+`, `-`, `*`, `/`, `%`, `&`, `|`, `^`, `<<`, `>>`
- **Constants**: `PI`, integers, floats
- **Variable**: `t` (time)

The separate native bytecode VM used by the development tooling currently supports
basic array literals, indexing, `length`, `Array(length)`, and the `fill`, `map`, and
`forEach`, `filter`, `find`, `findIndex`, `some`, `every`, `reduce`, `push`, `pop`,
`shift`, `unshift`, `reverse`, `slice`, `includes`, and `indexOf` methods, plus array
and string spread; string `charCodeAt`; global `parseInt`, `isNaN`, and `isFinite`;
plain object literals with string keys and property access/update; and passing
supported builtins as arguments to user functions. The formula's `t` sample-time
parameter is mutable and function parameters shadow it normally. Two-channel array
output is downmixed to mono by averaging its channels. Object prototypes, object
methods, and other array methods are not supported. The library audits report host
execution and ESP32 playback separately.

The `examples/console_web_daw` application is a complete DAW firmware that uses **only the native C engine** - no JavaScript/QuickJS required!

The Console Web DAW allows you to:
- Send formulas via **USB Serial Console** (UART)
- Control playback from **Web Browser** via Web Serial API
- Use it as **standalone** device or **web-connected** DAW

## Dollchan formula library

The official Dollchan index and downloadable formula source files can be synchronized
into a staging directory with:

```text
python tools_py\bytebeat_library_check.py --library-dir formula_library sync
```

The download writes `library_source_all.gz` (the original index archive),
`library.json` (normalized records with available full formula source), and
`library_metadata.json` (UTC retrieval time, source URL, archive SHA-256, and
coverage counts). The official source is
[Dollchan's bytebeat library archive](https://dollchan.net/bytebeat/data/library/all.gz).

Run the PC-side formula checks against that snapshot with:

```text
python tools_py\bytebeat_library_check.py --library-dir formula_library check
```

This snapshot is source material, not a release-ready ESP32 library. Compilation,
host execution, and device playback are separate checks; `library_metadata.json`
does not mark formulas as natively verified.

To execute the formulas that lower to bytecode against the native C VM on a
Windows host and refresh the per-song report, run:

```text
python tools_py\native_bytebeat_vm_host_audit.py
```

This host audit requires Visual Studio Build Tools with the C compiler installed.
It is not a substitute for playback testing on the ESP32.

## Use the component

Add the component to an ESP-IDF project:

```yaml
dependencies:
  esp_bytebeat:
    git: https://github.com/Strg-Alt-Entf-0x00/esp-bytebeat.git
    version: "^1.0.0"
```

Include the public API:

```c
#include "esp_bytebeat.h"
```

The application supplies `write_cb` in `esp_bytebeat_config_t`; the component does not assume an audio driver or codec.

```c
static size_t write_pcm(const int16_t *samples, size_t num_bytes)
{
  return audio_write(samples, num_bytes);
}

const esp_bytebeat_config_t config = {
  .target_sample_rate = 48000,
  .task_priority = 5,
  .task_core = 0xFFFFFFFF,
  .software_volume_q15 = 32768,
  .mode = ESP_BYTEBEAT_MODE_UNSIGNED,
  .write_cb = write_pcm,
};

ESP_ERROR_CHECK(esp_bytebeat_init(&config));
ESP_ERROR_CHECK(esp_bytebeat_play_string("(t * 5) & (t >> 7)", 0));
```

Use `ESP_BYTEBEAT_MODE_SIGNED` for signed formulas and `ESP_BYTEBEAT_MODE_FLOAT` for expressions such as `(t % 1000) / 1000.0`.

You can also use **Math functions** in formulas:
```c
// Sine wave bytebeat
esp_bytebeat_play_string("sin(t / 100) * 127 + 128", 0);

// Complex formula with sqrt and pow
esp_bytebeat_play_string("sqrt(t) & pow(t, 0.5)", 0);

// Using PI constant
esp_bytebeat_play_string("sin(t * PI / 100)", 0);
```

The `examples/web_daw` application now uses **100% native C** - QuickJS has been completely removed!

## Console Web DAW example

The example is configured for the author's ESP32-P4 Rev. 1.x board profile:

- 32 MB flash, DIO, 40 MHz
- external PSRAM in HEX mode at 200 MHz
- ES8311 external audio codec
- 48 kHz stereo I2S playback output
- board wiring defined in `examples/web_daw/main/Kconfig.projbuild`

It is not a universal ESP32-P4 board configuration. Adjust the GPIOs, flash, PSRAM, codec, and partition table for other hardware.

Build and flash it with ESP-IDF 6.0 or newer:

```text
cd examples/console_web_daw
idf.py build
idf.py -p COM_PORT flash monitor
```

The browser interface is located at `examples/console_web_daw/tools/OmniBox_DAW.html` and requires a Chromium-based browser with Web Serial support.

## License

The component is licensed under the MIT License. Third-party components and formula libraries retain their respective licenses.

## Documentation

📚 **Full Documentation:**
- [ROADMAP.md](.docs/ROADMAP.md) - Development roadmap & future plans
- [FEATURES.md](.docs/FEATURES.md) - All features & capabilities
- [STATUS.md](.docs/STATUS.md) - Current project status
- [OPTIMIZATION.md](.docs/OPTIMIZATION.md) - ESP-DSP & PIE optimization details
- [MIGRATION_GUIDE.md](.docs/MIGRATION_GUIDE.md) - Migrating from QuickJS
- [TEST_EXAMPLES.md](.docs/TEST_EXAMPLES.md) - Test formulas & examples
- [SUMMARY.md](.docs/SUMMARY.md) - Quick project summary
