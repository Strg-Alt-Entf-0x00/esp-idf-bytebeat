# Changelog - ESP Bytebeat

## [Unreleased]

### Added
- Project-specific Web Serial interface under `tools/` for the development
  firmware, formula catalog search, safe local preview, and direct formula
  transfer through the device's native compiler.
- Unified Python Bytebeat desktop console and CLI with exact firmware identity
  verification, bounded playback defaults, explicit playback controls, and a
  Windows launcher using an isolated virtual environment.
- Compact playback toolbar with Play, Hold-to-play, Stop, and Settings-menu
  controls for speaker volume and safety timeout.
- Migrated the desktop UI to PySide6 and added a resizable vertical workspace/log
  splitter consistent with the Siren Console.
- Host-side serial protocol tests using a simulated development device.

### Fixed
- Added a firmware-side formula preflight so unsupported catalog syntax is
  rejected before playback settings or start commands reach the device.
- Bumped the development firmware identity to distinguish devices implementing
  the formula-validation protocol.

## [1.1.0] - 2025-01-20 - Performance Release 🚀

### ⚡ Performance Improvements
- **ESP-DSP Integration:** Hardware-accelerated math functions (3-5x speedup!)
  - `sin()`, `cos()`, `sqrt()` now use DSP acceleration
  - Configurable via Kconfig: `CONFIG_ESP_BYTEBEAT_USE_DSP`
  - Automatic fallback to standard math on unsupported chips
  
- **PIE Support:** SIMD optimizations for ESP32-S3/P4
  - Chip detection and automatic optimization
  - Loop unrolling for stereo buffer fill
  - Foundation for future SIMD math operations
  
- **Total Speedup:** 5-7x faster on ESP32-P4 with DSP+PIE! ⚡

### 🎯 Added
- `Kconfig` - Configuration options for optimizations
- `src/bytebeat_vm_pie.h` - PIE optimization header
- `src/bytebeat_vm_pie.c` - PIE implementation with chip detection
- `.docs/validation/PERFORMANCE_TEST.md` - Benchmark results and test methodology

### 🔧 Changed
- `idf_component.yml` - Added ESP-DSP dependency
- `CMakeLists.txt` - Added ESP-DSP and PIE source files
- `src/bytebeat_vm.c` - Integrated DSP macros for math functions
- `examples/console_bytebeat/main/bytebeat_engine.cpp` - Added boot info (chip, DSP status)

### 📊 Performance Metrics
| Formula | Before | After | Speedup |
|---------|--------|-------|---------|
| `sin(t/100)` | 15ms | 4ms | **3.75x** |
| `sin(t/100)+cos(t/150)` | 25ms | 5ms | **5x** |
| Complex math | 35ms | 7ms | **5x** |

### 🎮 Chip Support
- ✅ ESP32 - DSP enabled
- ✅ ESP32-S3 - DSP + PIE enabled
- ✅ **ESP32-P4 - DSP + PIE + 400MHz = Maximum Performance!** 🔥

---

## [1.0.0] - 2025-01-20 - JavaScript-Free Release 🎉

### 🚫 Removed
- **QuickJS completely eliminated!** No more JavaScript anywhere!
  - Removed `espressif/quickjs-ng` dependency
  - Removed all JS-based formula execution
  - Removed HTML web interface (replaced with Python GUI)
  - Removed `library_data.js` (now pure JSON)

### ✅ Added
- **Native C-Engine:** Pure C bytecode VM with full math support
  - 13 Math functions: sin, cos, tan, sqrt, abs, floor, ceil, round, log, exp, pow, min, max
  - Constants: PI, integers, floats
  - Variable: `t` (time/sample counter)
  - Operators: +, -, *, /, %, &, |, ^, <<, >>
  
- **Python Tool:** Unified `bytebeat_tool.py` with:
  - GUI (Tkinter-based)
  - Formula converter (JS→C)
  - Validator
  - Serial monitor
  - Library packer
  
- **2422 Formulas:** Community library converted to JSON

### 🔄 Changed
- `examples/web_daw/` → `examples/console_bytebeat/` (renamed)
- `src/bytebeat_vm.h` - Extended with 20+ new opcodes
- `src/bytebeat_vm.c` - Completely rewritten with math functions
- All docs moved to `.docs/` directory

### 📁 Project Structure
```
esp-idf-bytebeat/
├── src/                  # Core C engine
├── examples/
│   └── console_bytebeat/ # Example (no JS!)
├── tools_py/
│   ├── bytebeat_tool.py  # Unified Python tool
│   └── library.json      # 2422 formulas (pure JSON)
└── .docs/                # All documentation
```

### 📚 Documentation
- `FEATURES.md` - Feature overview
- `MIGRATION_GUIDE.md` - QuickJS→C migration
- `OPTIMIZATION.md` - Performance optimization details
- `ROADMAP.md` - Development roadmap
- `STATUS.md` - Current project status
- `SUMMARY.md` - Quick summary
- `TEST_EXAMPLES.md` - Test formulas

### 🎯 Impact
- **Binary size:** -83% (~1.2MB → ~200KB)
- **RAM usage:** -90% (~500KB → ~50KB)
- **Compile time:** -95% (~100ms → ~5ms)
- **JavaScript:** -100% (0 bytes!) 🚫☕

---

## [0.5.0] - 2024 - Legacy (QuickJS-based)

### Features
- QuickJS JavaScript engine for formula execution
- Web Serial API interface
- Math.* functions support
- Funcbeat support

### Issues
- Large binary size (~1.2MB)
- High RAM usage (~500KB)
- Slow compilation (~100ms)
- JavaScript dependency

**Status:** ❌ **Deprecated - Use v1.0.0+ instead!**

---

## Migration Guide

### From 0.5.x to 1.0.0:
1. Update formulas: `Math.sin(x)` → `sin(x)`
2. Remove function wrappers: `function audio(t) { return ... }` → `...`
3. Use Python tool instead of HTML interface
4. Rebuild project: `idf.py fullclean && idf.py build`

### From 1.0.0 to 1.1.0:
1. Update dependencies: `idf.py fullclean`
2. Enable ESP-DSP in menuconfig (recommended)
3. Rebuild: `idf.py build`
4. Flash and enjoy 5x performance! ⚡

---

## Roadmap

### Next (v1.2.0):
- [ ] Stereo support (comma operator)
- [ ] Comparison operators (<, >, ==)
- [ ] Ternary operator (a ? b : c)

### Future (v2.0.0):
- [ ] Multi-voice support
- [ ] Effects chain (reverb, delay, filter)
- [ ] MIDI integration

See `ROADMAP.md` for full details.

---

**License:** MIT
**Repository:** [Your GitHub URL]
**Maintainer:** Strg-Alt-Entf-0x00
