# Integration

`esp_bytebeat` is the reusable ESP-IDF component at the repository root. It
compiles and renders supported Bytebeat expressions; it does not initialize a
board, audio codec, I2S peripheral, or speaker. The consuming application
provides an audio output callback and owns hardware setup.

## Add the Component

The component manifest declares ESP-IDF 5.0 or newer and ESP-DSP. For a
published project, add the component through the ESP-IDF Component Manager:

```yaml
dependencies:
  esp_bytebeat:
    git: https://github.com/Strg-Alt-Entf-0x00/esp-bytebeat.git
    version: "^1.0.0"
```

For local development, add the component repository through the consumer's
component-manager override or `EXTRA_COMPONENT_DIRS`. Do not copy component
source into an example or edit generated `managed_components` content.

## Provide Audio Output

Include the public header and configure a callback that writes the generated
16-bit PCM samples to the application's audio sink:

```c
#include "esp_bytebeat.h"

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

The callback's return value and buffer handling must follow the public API
contract in [`include/esp_bytebeat.h`](../include/esp_bytebeat.h). The example
above assumes the application provides `audio_write`; it is not supplied by
the Bytebeat component.

## Target Portability

The component's expression and sample-generation code is written in C and
does not own board peripherals. Its manifest declares ESP-IDF 5.0 or newer and
ESP-DSP, but those declarations alone do not certify every ESP32 target. The
supplied playback examples use ESP32-P4 board/audio configurations. For
another target, verify dependency support, build the consumer application,
and test output, memory use, and timing on the intended hardware.

`bytebeat_vm_pie` retains a legacy name. Its current stereo-fill routine uses
portable C and does not issue PIE/SIMD instructions; the P4/S3 target check
does not mean that this implementation receives hardware acceleration.
ESP-DSP's selected implementation can vary by target and configuration, so
measure before claiming a performance benefit.

Available component modes are unsigned Bytebeat, Floatbeat, and Signed
Bytebeat. Select the mode to match the expression. The root
[`README.md`](../README.md) documents the component's current expression
capabilities and limitations.

## Hardware Examples

The repository examples demonstrate application-level integration with
specific ESP32-P4 audio hardware:

- [`console_bytebeat`](../examples/console_bytebeat/README.md) is the basic
  serial-controlled playback example.
- [`console_bytebeat_dev`](../examples/console_bytebeat_dev/README.md) adds
  development controls and declares a UART Filebridge dependency.

These example configurations are not requirements of the reusable component.
Review each example's README and configuration before using it with different
hardware.
