# Development Tools

## Scope

This guide covers the Bytebeat tools in `tools_py`, the ESP-IDF examples, and
the formula snapshot in `formula_library`. It describes observed source and
checked-in files, not assumed functionality.

## Python Bytebeat Tool

The unified CLI and GUI entry point is `tools_py/bytebeat_tool.py`.

| Command | Purpose |
| --- | --- |
| `gui` | Open the desktop interface |
| `list [--categories] [--category NAME]` | List formulas or categories |
| `search QUERY` | Search the loaded formula catalog |
| `preview INDEX` | Preview a catalog entry on the host |
| `preview-custom FORMULA` | Preview a supplied formula |
| `play INDEX --port PORT` | Send a catalog formula to the device |
| `send FORMULA --port PORT [--mode N] [--sample-rate HZ]` | Compile and transfer bytecode |
| `stop --port PORT` | Stop device playback |
| `convert FILE` | Apply the tool's JavaScript-to-expression replacements |
| `validate FILE` | Run the tool's formula checks |
| `pack [--library DIR] [--output FILE]` | Pack formula files into a JSON catalog |
| `serial PORT [--baudrate RATE]` | Open a serial monitor |

Run `python tools_py\bytebeat_tool.py --help` for the current argument
definitions. Device modes in the tool are 0 (Bytebeat), 1 (Floatbeat), 2
(Signed Bytebeat), and 3 (Funcbeat). The firmware's `INFO` response currently
advertises modes U, F, and S; do not assume mode 3 is accepted by every example.

### Current checkout limitations

The script imports `native_bytebeat_bytecode` and `native_bytebeat_compiler`.
Neither helper module is beside `bytebeat_tool.py` in the visible `tools_py`
root. Running the script's help command in this checkout currently fails with
`ModuleNotFoundError: native_bytebeat_bytecode`. The CLI, GUI, preview,
transfer, and pack workflows therefore require their helper modules to be
restored or otherwise made available before they can be treated as runnable.

The script looks for its formula catalog at
`tools_py/library/library.json`, falling back to `tools_py/library.json`.
Neither file is currently present. Its pack command defaults to
`tools_py/dollchan_library` and writes to `tools_py/library/library.json`;
that source directory is also absent. These are legacy expectations, not
verified commands for the current formula snapshot.

The script uses Python standard-library modules and imports PySerial for serial
operations. Host audio preview imports PyAudio. No project-local dependency
manifest for this script was found.

## Formula Snapshot

The checked-in source snapshot is under `data/formula_library/`:

- `library_source_all.gz`: original Dollchan archive.
- `library_source_metadata.json`: source URL, archive hash, and recorded
  provenance.
- `library_validated.json`: a 2,091-entry derived dataset.

The metadata says the retrieval date is unknown and the archive was obtained
manually. Its 100% figure describes source-record coverage, not successful
compilation, execution, or playback. Preserve the source archive and hash;
record new retrieval dates and validation stages when the snapshot is
refreshed. The individual formula licensing terms may differ.

## Device Examples

`examples/console_bytebeat` is the user-facing hardware example.
`examples/console_bytebeat_dev` adds development controls, diagnostic output,
and a UART Filebridge dependency. Its `esp-build.bat` and
`esp-build-config.json` define the build and port workflow; use the active
configuration instead of assuming a particular COM port.

The developer firmware accepts formula commands over serial and implements a
native bytecode transfer protocol (`BBLOAD`, `BBDATA`, `BBEND`) with
acknowledgements. It also responds to `VERSION` and `INFO`. The device's
supported modes and command behavior are firmware-specific; verify them
against the example source and README before relying on a host-tool option.

The Filebridge is a separate data-transfer connection. Its use does not by
itself mean that telemetry is being recorded. See the
[Filebridge guide](FILEBRIDGE.md) for the currently documented connection.

## Development Workflow

1. Identify the formula source and the execution target.
2. Check syntax with the tool when its helper modules are available.
3. Treat host preview as a preliminary check only.
4. Build and test the intended firmware on the target board.
5. Record board revision, ESP-IDF version, configuration, formula, sample
   rate, and test result for performance comparisons.

Do not infer device compatibility or audio quality from a successful source
download, JSON validation, host compilation, or host playback alone.
