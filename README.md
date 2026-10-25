# ESP Bytebeat

Reusable procedural audio and Bytebeat components for ESP-IDF, with a hardware-specific serial-controlled playback example for ESP32-P4.

## Components

The repository root is the publishable `esp_bytebeat` component. It provides a small native C bytecode engine and an output callback API. It has no dependency on QuickJS, codecs, I2S, or a particular board.

The component is designed to keep its synthesis and expression engine
independent of board hardware. The supplied playback examples currently use
ESP32-P4-specific board/audio configurations; that does not establish a
build- or hardware-validated target matrix for the reusable component. Check
the consuming target's ESP-IDF and ESP-DSP support and validate its build,
audio output, memory use, and timing before claiming support for that target.
The legacy-named `bytebeat_vm_pie` helper currently uses portable C, not PIE
instructions; its target gate is not evidence of a hardware-accelerated path.

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

The `examples/console_bytebeat` application is a serial-controlled formula playback example that uses **only the native C engine**; it does not require JavaScript or QuickJS.

It accepts formulas and playback controls through its USB serial console. The
separate `examples/console_bytebeat_dev` application is intended for
development and adds diagnostics and a declared UART Filebridge dependency;
telemetry capture is not established by that dependency alone.

Both examples include an `esp-build.bat` wrapper and an
`esp-build-config.json` for their own build workflow. See their READMEs for
supported commands and example-specific hardware requirements:

- [Basic user example](examples/console_bytebeat/README.md)
- [Developer example](examples/console_bytebeat_dev/README.md)

## Dollchan formula snapshot

The checked-in source snapshot is under `data/formula_library/`. It contains the
original `library_source_all.gz` archive, source metadata, and a 2,091-entry
derived dataset. The metadata records an unknown retrieval date; its source
coverage is not evidence that formulas compile or play on the ESP32. See
[Development Tools](documentation/development_tools/DEVELOPMENT_TOOLS.md) for
the current snapshot and tool limitations.

The current `tools_py/bytebeat_tool.py` expects helper modules and catalog paths
that are not present in this checkout, so its CLI cannot currently be run.
See [Development Tools](documentation/development_tools/DEVELOPMENT_TOOLS.md)
for the observed limitations and tool inventory.

## Use the component

See [Component integration](documentation/INTEGRATION.md) for dependency
setup, the output callback, and a minimal use example.

## Hardware-specific playback example

The example is configured for the author's ESP32-P4 Rev. 1.x board profile:

- 32 MB flash, DIO, 40 MHz
- external PSRAM in HEX mode at 200 MHz
- ES8311 external audio codec
- 48 kHz stereo I2S playback output
- board wiring defined in `examples/console_bytebeat/main/Kconfig.projbuild`

It is not a universal ESP32-P4 board configuration. Adjust the GPIOs, flash, PSRAM, codec, and partition table for other hardware.

Build and flash it with ESP-IDF 6.0 or newer:

```text
cd examples/console_bytebeat
idf.py build
idf.py -p COM_PORT flash monitor
```

## License

The component is licensed under the MIT License. Third-party components and formula libraries retain their respective licenses.

## Documentation

- [Changelog](CHANGELOG.md)
- [Component integration](documentation/INTEGRATION.md)
- [Development tools](documentation/development_tools/DEVELOPMENT_TOOLS.md)
- [UART Filebridge](documentation/development_tools/FILEBRIDGE.md)
