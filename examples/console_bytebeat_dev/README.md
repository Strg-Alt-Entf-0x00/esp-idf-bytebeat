# ESP Bytebeat Console - Development Version

**🔬 Professional development environment with telemetry & analysis tools**

---

## 🎯 Purpose

This is the **development version** of ESP Bytebeat Console, designed for:
- ✅ Performance testing & profiling
- ✅ Telemetry data capture (SD card)
- ✅ Remote file access (UART Filebridge)
- ✅ AI-autonomous development
- ✅ Optimization experiments

**For end users:** Use [`console_bytebeat/`](../console_bytebeat/) instead!

---

## 🆚 Differences from console_bytebeat

| Feature | console_bytebeat | console_bytebeat_dev |
|---------|-----------------|---------------------|
| **Purpose** | End user | Development |
| **Telemetry** | ❌ | ✅ SD card capture |
| **Filebridge** | ❌ | ✅ COM13 access |
| **Complexity** | Simple | Advanced |
| **Build Scripts** | One-click | Multiple scripts |
| **Documentation** | Quick start | Detailed dev guide |

---

## 📋 Requirements

### Hardware:
- **ESP32-P4** (recommended) or ESP32-S3
- **USB Cable** (data, not charge-only) → COM4
- **FT232 Module** (external UART) → COM13
- **SD Card** (FAT32, Class 10, >= 4GB)
- **Audio Codec** (ES8311) for sound output

### Software:
- ESP-IDF v6.0+ ([Install Guide](https://docs.espressif.com/projects/esp-idf/en/stable/esp32/get-started/))
- Python 3.8+ (for analysis tools)
- Windows (scripts are .bat, but project runs on Linux/Mac)

---

## 🚀 Quick Start

### 1. Build & Flash

**From a regular Command Prompt or ESP-IDF Command Prompt:**
```batch
cd examples\console_bytebeat_dev
esp-build.bat
esp-build.bat -p COM4 flash monitor
```

`esp-build.bat` is included with this example and builds it from its own
directory. It does not require the separate `esp-build-manager` repository.
If ESP-IDF is not already active, it looks in `IDF_PATH` and common Windows
installation locations (including `C:\esp\v*\esp-idf` and
`C:\Espressif\frameworks\esp-idf*`) and calls the discovered installation's
`export.bat`.

You can also pass ESP-IDF commands through the same script:
```batch
esp-build.bat clean
esp-build.bat menuconfig
esp-build.bat flash
esp-build.bat monitor
esp-build.bat -p COM4 flash monitor
esp-build.bat --dry-run -p COM4 flash monitor
```

The example's `esp-build-config.json` controls the default action, port,
separate flash/monitor baud rates, ccache, and whether CMake-registered
additional images are flashed. Its safe default is build-only. Command-line
actions override the default for that run; for example,
`esp-build.bat -p COM4 flash monitor` selects COM4, flashes, then starts the
monitor. `--dry-run` checks target selection without accessing the board.

### 2. Check Boot Messages

**Expected output:**
```
Booting ESP Bytebeat Console Dev v1.1.0
Pure C Engine - No JavaScript!

Chip: ESP32-P4 | PIE hardware: Available (bytebeat PIE kernel: not implemented)
ESP-DSP: Enabled (square-root helper; target performance unmeasured)
Sample Rate: 48000 Hz | Chunk: 512 samples

SD Card: Mounted at /sd
Filebridge: UART1 @ 3000000 baud on COM13
Telemetry: Ready (/sd/telemetry/)

Ready for development!
```

### 3. Test Filebridge (COM13)

**From Python:**
```powershell
python -m esp_idf_uart_filebridge.cli --port COM13 --baud 3000000 ls /sd
python -m esp_idf_uart_filebridge.cli --port COM13 --baud 3000000 ls /sd/telemetry
```

**Expected:** Lists files on SD card

---

## 🔧 Configuration

### COM Ports:
- **COM4** = Console (USB Serial, monitor, flash)
- **COM13** = Filebridge (External FT232, file transfer)

**Check your ports:**
1. Device Manager → Ports (COM & LPT)
2. Find "USB Serial Device" (Console)
3. Find "USB Serial Port" (FT232 → Filebridge)

### SD Card Setup:
1. Format as **FAT32**
2. Insert into ESP32-P4 board
3. Boot → should see "SD Card: Mounted"
4. Directory `/sd/telemetry/` created automatically

### Menuconfig:
```batch
idf.py menuconfig
```

**Important settings:**
- `ESP Bytebeat Configuration` → ESP-DSP [✓]
- `Telemetry` → Enable [✓] (when implemented)
- `Component config` → External RAM [✓]

---

## 📊 Telemetry System

**Status:** 🟡 Planned (not yet implemented)

**Will capture:**
- Chunk processing time
- CPU load (Core 0 & Core 1)
- Memory usage (Heap, PSRAM)
- Audio peaks (L/R channels)
- Buffer underruns
- DSP & PIE status

**File format:** BB1T (ByteBeat Telemetry v1)
- Binary format (compact & fast)
- Header (128 bytes) + Records (40 bytes each)
- Similar to formant E32T format
- Python converter to CSV

**Workflow:**
1. Run formula on ESP32
2. Telemetry written to `/sd/telemetry/sin_t_100_0001.bb1t`
3. Download via Filebridge (COM13)
4. Analyze with Python tools

---

## 🐍 Python Tools (Coming Soon)

### AI Developer CLI:
```powershell
# Device control
python -m tools_py.ai_developer device info
python -m tools_py.ai_developer device reset

# File transfer
python -m tools_py.ai_developer device files list
python -m tools_py.ai_developer device files download

# Formula testing
python -m tools_py.ai_developer formula test "sin(t/100)"
python -m tools_py.ai_developer formula benchmark "sin(t/100)" --duration 10

# Full session (run + download + analyze)
python -m tools_py.ai_developer session run --formula "sin(t/100)" --duration 10 --download
python -m tools_py.ai_developer session analyze <session-id>

# Analysis
python -m tools_py.ai_developer analyze performance <bb1t-file>
python -m tools_py.ai_developer analyze compare <session1> <session2>
```

---

## 🔬 Development Workflow

### Typical Session:
1. **Flash firmware** (COM4)
2. **Run formula** via serial commands
3. **Telemetry captured** to SD card automatically
4. **Download data** via Filebridge (COM13)
5. **Analyze** with Python tools
6. **Optimize** code based on findings
7. **Repeat** to verify improvements

### Performance Testing:
```powershell
# Run baseline test
python -m tools_py.ai_developer session run --formula "sin(t/100)" --duration 30 --label "baseline"

# Make code changes (e.g., optimize ESP-DSP)

# Run comparison test
python -m tools_py.ai_developer session run --formula "sin(t/100)" --duration 30 --label "optimized"

# Compare results
python -m tools_py.ai_developer analyze compare baseline optimized
```

**Metrics:**
- Chunk time (target: <5ms)
- CPU load (balance Core0/Core1)
- Memory usage (avoid leaks)
- Audio quality (no clipping, distortion)

---

## 📁 Directory Structure

```
console_bytebeat_dev/
├── main/
│   ├── audio_manager.cpp
│   ├── bytebeat_engine.cpp       # Audio processing
│   ├── serial_commands.cpp       # Console commands
│   ├── main.cpp                  # Init & setup
│   ├── idf_component.yml         # Dependencies (incl. filebridge)
│   └── Kconfig.projbuild
│
├── components/                    # To be added
│   └── telemetry/                # Telemetry system (planned)
│       ├── include/
│       │   ├── telemetry.h
│       │   └── telemetry_format.h
│       └── src/
│           ├── telemetry.c
│           └── telemetry_bb1t.c
│
├── CMakeLists.txt
├── partitions.csv
├── sdkconfig.defaults             # Pre-configured settings
├── esp-build.bat                  # Standalone build, clean, flash and monitor
├── esp-build-config.json          # Local defaults for this example
└── README.md                      # This file
```

---

## ⚙️ Technical Details

### Telemetry Architecture (Planned):
```
Audio Callback (Core 1, High Priority)
    |
    v
telemetry_log_frame() → [Queue 1024 items] → Writer Task (Core 0, Low Priority)
                                                    |
                                                    v
                                              [RAM Buffer 512 records]
                                                    |
                                                    v
                                              SD Write (batch 512 records)
```

**Why queue-based?**
- Audio callback must be **fast** (no blocking!)
- Queue = async, non-blocking
- Writer task runs on separate core
- Batch writes = efficient SD usage

### Performance Targets:
- **Telemetry overhead:** <0.5ms (<10%)
- **No audio glitches** (buffer underruns)
- **SD write speed:** Class 10 card (10 MB/s+)
- **Queue:** Large enough to prevent drops (1024 records)

### Memory Usage:
- **Queue:** 1024 × 40 bytes = ~40 KB
- **Write buffer:** 512 × 40 bytes = ~20 KB
- **Total overhead:** ~60 KB (acceptable on ESP32-P4)

---

## 🐛 Troubleshooting

### Build Issues:

**"esp-uart-filebridge not found"**
```batch
# Clean and rebuild
idf.py fullclean
idf.py build
# Component Manager will download filebridge automatically
```

**"ESP-IDF not found"**
```batch
# Open ESP-IDF Terminal (from Start Menu)
# Or run export.bat:
C:\esp\v6.0.2\esp-idf\export.bat
```

### Runtime Issues:

**"SD Card mount failed"**
- Check SD card is inserted
- Must be FAT32 formatted
- Try different card (some cards incompatible)
- Check wiring (if custom board)

**"Filebridge not responding"**
- Check COM13 is correct port (Device Manager)
- Verify baud rate: 3,000,000
- Check hardware flow control (RTS/CTS)
- Try different FT232 module

**"Performance degraded"**
- Check ESP-DSP enabled (`idf.py menuconfig`)
- Check whether PIE hardware is available; the current bytebeat helper does not use PIE instructions
- Disable telemetry if testing baseline
- Check CPU affinity (Core 0 vs Core 1)

---

## 📚 Documentation

**In Project:**
- [FILEBRIDGE.md](../../documentation/development_tools/FILEBRIDGE.md) - Filebridge guide
- [INTEGRATION.md](../../documentation/INTEGRATION.md) - Component integration

**Reference:**
- [ESP-IDF Programming Guide](https://docs.espressif.com/projects/esp-idf/)
- [esp-uart-filebridge](https://github.com/Strg-Alt-Entf-0x00/esp-idf-uart-filebridge)
- [formant telemetry](https://github.com/Strg-Alt-Entf-0x00/esp32-p4-idf-espormant32)

---

## 🎯 Roadmap

### Sprint 1: Foundation (Current)
- [x] Create `console_bytebeat_dev/`
- [x] Add filebridge dependency
- [x] Configure sdkconfig
- [ ] Build & test on hardware

### Sprint 2: SD Card
- [ ] Implement SD card init
- [ ] Create `/sd/telemetry/` directory
- [ ] Test file read/write
- [ ] Verify filebridge access

### Sprint 3: Telemetry
- [ ] Design BB1T format
- [ ] Create telemetry component
- [ ] Integrate in audio callback
- [ ] Test capture & download

### Sprint 4: Python Tools
- [ ] Create AI developer CLI structure
- [ ] Implement device commands
- [ ] Implement file transfer
- [ ] Session management

### Sprint 5: Analysis
- [ ] BB1T to CSV converter
- [ ] Performance analysis
- [ ] Audio quality analysis
- [ ] Comparison tools

### Sprint 6: Optimization
- [ ] Measure baseline
- [ ] ESP-DSP tuning
- [ ] Implement and benchmark a target-specific PIE code path, retaining a portable fallback
- [ ] Memory layout
- [ ] CPU load balancing

---

## ✅ Success Criteria

### Minimal Viable Product:
- [ ] Builds successfully
- [ ] Runs on ESP32-P4 (COM4)
- [ ] SD card mounts
- [ ] Filebridge accessible (COM13)
- [ ] Telemetry writes to SD
- [ ] Data downloadable
- [ ] Python tools work
- [ ] Performance analysis available

### Quality Gates:
- [ ] No crashes or hangs
- [ ] No memory leaks
- [ ] Telemetry overhead <10%
- [ ] No audio glitches
- [ ] Documentation complete

---

## 👥 For AI Agents

**This is an AI-autonomous development environment!**

**Guidelines:**
1. Always document what you do
2. Measure before optimizing
3. One change at a time
4. Compare with baseline
5. Use session management
6. Preserve raw data
7. SHA-256 verify downloads
8. Never assume, always test

**Resources:**
- `AUTOPILOT_SESSION_1.md` - Current session log
- Reference projects in `../../esp32-p4-idf-*`

**Next Actions:**
1. Build this project
2. Flash to ESP32-P4
3. Verify basic functionality
4. Implement SD card support
5. Create telemetry component
6. Build Python tools
7. Start optimization experiments

---

**Version:** 1.1.0-dev  
**Status:** 🟡 In Development (Sprint 1)  
**Last Update:** 2025-01-20

**Ready to develop!** 🚀
