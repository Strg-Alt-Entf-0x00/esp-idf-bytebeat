# ESP Bytebeat

Reusable procedural audio and Bytebeat components for ESP-IDF, with a hardware-specific Web DAW example for ESP32-P4.

## Components

The repository root is the publishable `esp_bytebeat` component. It provides a small native C bytecode engine and an output callback API. It has no dependency on QuickJS, codecs, I2S, or a particular board.

The `examples/web_daw` application is separate. It adds QuickJS, Web Serial, I2S, and the ES8311 codec to provide a complete DAW firmware.

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

## Web DAW example

The example is configured for the author's ESP32-P4 Rev. 1.x board profile:

- 32 MB flash, DIO, 40 MHz
- external PSRAM in HEX mode at 200 MHz
- ES8311 external audio codec
- 48 kHz stereo I2S playback output
- board wiring defined in `examples/web_daw/main/Kconfig.projbuild`

It is not a universal ESP32-P4 board configuration. Adjust the GPIOs, flash, PSRAM, codec, and partition table for other hardware.

Build and flash it with ESP-IDF 6.0 or newer:

```text
cd examples/web_daw
idf.py build
idf.py -p COM_PORT flash monitor
```

The browser interface is located at `examples/web_daw/tools/OmniBox_DAW.html` and requires a Chromium-based browser with Web Serial support.

## License

The component is licensed under the MIT License. Third-party components and formula libraries retain their respective licenses.
