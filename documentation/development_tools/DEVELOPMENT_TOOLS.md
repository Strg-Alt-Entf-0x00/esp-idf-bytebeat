# ESP32 Bytebeat Development Tools

## 📁 Current State (Latest Update)

### ✅ Completed
- **Production GUI** fully integrated into `bytebeat_tool.py`
- **2422 Formula Library** from dollchan successfully loaded
- **PC Audio Preview** working (pyaudio-based)
- **CLI Commands** all functional (list, search, preview, play, send, stop)
- **Modern UX** with professional search field (counter inside, placeholder, clear button)
- **All in ONE file**: `tools_py/bytebeat_tool.py` (no duplicates, cleaned up)

### 📦 Files Structure
```
tools_py/
├── bytebeat_tool.py       # ✅ Final unified tool (CLI + GUI)
├── library.json           # ✅ 2422 formulas packed
└── dollchan_library/      # Original formula sources
    ├── classic/           # 421 formulas
    ├── floatbeat/         # 362 formulas
    ├── floatbeat-big/     # 215 formulas
    ├── funcbeat/          # 137 formulas
    ├── js-1k/             # 497 formulas
    ├── js-256/            # 572 formulas
    └── js-big/            # 217 formulas
```

### 🎯 Tool Features
**GUI Mode:**
- Professional 3-panel layout (Library | Editor | Console)
- Search field with live counter INSIDE field
- Category dropdown (no redundant label)
- Colored connection button (Red/Orange/Green states)
- PC Preview button (plays on PC speakers)
- Send to ESP32 button
- Full-width debug console with timestamps

**CLI Mode:**
```bash
# List & Search
python bytebeat_tool.py list --categories
python bytebeat_tool.py list --category classic
python bytebeat_tool.py search "melody"

# PC Preview (test locally)
python bytebeat_tool.py preview 42
python bytebeat_tool.py preview-custom "t>>4" --duration 10

# ESP32 Control
python bytebeat_tool.py play 42 --port COM4
python bytebeat_tool.py send "t*(t>>8|t>>9)&63" --port COM4
python bytebeat_tool.py stop --port COM4
```

---

## 🚀 TODO: Next Session Priorities

### 1. 🔌 **Connection Hardening** (CRITICAL)
**Problem:** Tool connects to ANY device on COMx, doesn't verify it's OUR firmware.

**Solution:** Professional Handshake Protocol
- [ ] **Firmware Side** (`console_bytebeat_dev`):
  - Add command handler for `VERSION?\n`
  - Respond with `BYTEBEAT:v2.0.0\n`
  - Add `INFO?\n` → `BYTEBEAT:ESP32-P4:8000Hz:modes=U,F,S\n`
  
- [ ] **Tool Side** (`bytebeat_tool.py`):
  - After `serial.Serial()`, send `VERSION?\n`
  - Wait 500ms for response
  - Parse response, verify contains `BYTEBEAT:`
  - Only then set status to GREEN "Connected"
  - If timeout or wrong response: ORANGE "Unknown Device" or RED "Wrong Firmware"

- [ ] **Auto-Detection:**
  - Scan all available COM ports
  - Send `VERSION?\n` to each
  - Auto-select first port that responds with `BYTEBEAT:`
  - Like Arduino IDE / PlatformIO does it

**Research:**
- How do professional tools detect devices? (Arduino IDE, PlatformIO, esptool)
- Serial handshake best practices
- Timeout handling
- Multi-device scenarios

---

### 2. 🎵 **Dollchan Deep Analysis** (CRITICAL)
**Problem:** Some formulas work, some don't. Why?

**Investigation Tasks:**
- [ ] **Analyze Original Implementation:**
  - URL: https://dollchan.net/bytebeat/#4AAAA+kUzNNDSKLGzM68pqQFSZpraJkC+GpBpaAwRAAA
  - Download/inspect JavaScript source
  - Understand how they parse and execute formulas
  - What features do they support that we don't?

- [ ] **Compare Implementations:**
  - **Dollchan:** JavaScript eval-based, browser AudioContext
  - **Our ESP32:** C-based bytecode interpreter
  - What's missing? Arrays? Advanced functions? Stereo?

- [ ] **Test Each Category:**
  - `classic` (421) - Basic unsigned bytebeat
  - `floatbeat` (362) - Float-based (sin/cos)
  - `funcbeat` (137) - Complex functions
  - `js-1k`, `js-256`, `js-big` (1286) - JavaScript syntax
  - Which categories work? Which fail?

- [ ] **Identify Missing Features:**
  - Arrays support? `[note1, note2][t>>10]`
  - String operations?
  - Advanced Math functions? `atan2`, `random`
  - Stereo output? `[left, right]`
  - Variable declarations? `let x = ...`

- [ ] **Document Findings:**
  - Create compatibility matrix
  - List unsupported features
  - Prioritize what to implement next

**Files to check:**
- Original: https://dollchan.net/bytebeat/
- Our VM: `src/internal/moonshine_vm.c` or similar
- Our parser: Tool's `convert_js_to_c()` function

---

### 3. 🔧 **Tool Improvements**
- [ ] Add Handshake to `connect()` method
- [ ] Add Auto-Port-Detection button
- [ ] Better error messages (parse ESP32 error responses)
- [ ] Add "Test Formula" button (validates before sending)
- [ ] Add Formula Compatibility indicator (green/yellow/red based on features used)

---

### 4. 🛠️ **Firmware Improvements** 
**Location:** `D:\github-repositorys\ESP32\esp-idf-bytebeat\examples\console_bytebeat_dev\`

- [ ] Add VERSION command handler in `main.cpp` or serial_commands
- [ ] Add INFO command (report capabilities)
- [ ] Better formula error reporting (send back to tool)
- [ ] Add missing features identified from dollchan analysis
- [ ] Test with problematic formulas

---

### 5. 📊 **Testing & Validation**
- [ ] Create test suite script
- [ ] Test all 2422 formulas automatically
- [ ] Measure success rate per category
- [ ] Generate report: `formulas_compatibility.json`
- [ ] Priority fix list for broken formulas

---

## 🔥 Quick Start Commands (for next session)

### Start GUI
```bash
cd D:\github-repositorys\ESP32\esp-idf-bytebeat\tools_py
python bytebeat_tool.py gui
```

### Build & Flash Firmware
```bash
cd D:\github-repositorys\ESP32\esp-idf-bytebeat\examples\console_bytebeat_dev
cmd /c "C:\esp\v6.0.2\esp-idf\export.bat && idf.py build flash monitor"
```

### Test Serial Connection
```bash
python bytebeat_tool.py serial COM4
```

---

## 📝 Session Notes

### Current Issues
1. **Connection not verified** - Tool connects to any serial device
2. **Some formulas don't play** - Need to identify why
3. **No auto-detection** - User must manually select port

### Known Working Examples
- Simple formulas: `t>>4`, `t*(t>>8)`
- Floatbeat: `sin(t/100)+cos(t/150)`
- Classic bytebeat: `t*(t>>8|t>>9)&63`

### Known Broken Examples
- (To be identified in next session via dollchan analysis)

---

## 🎯 Success Criteria for Next Session

**Connection Hardening:**
- ✅ Tool only shows "Connected" if ESP32 responds with correct handshake
- ✅ Auto-detection finds ESP32 automatically
- ✅ Clear error messages for wrong devices

**Dollchan Analysis:**
- ✅ Understand why some formulas fail
- ✅ Document missing features
- ✅ Create compatibility matrix

**Implementation:**
- ✅ Handshake protocol working both sides
- ✅ At least one missing feature implemented
- ✅ Success rate improved measurably

---

## 📚 References

- **Dollchan Bytebeat:** https://dollchan.net/bytebeat/
- **Original Bytebeat:** http://canonical.org/~kragen/bytebeat/
- **ESP-IDF Serial:** https://docs.espressif.com/projects/esp-idf/en/latest/esp32/api-reference/peripherals/uart.html
- **PySerial Docs:** https://pyserial.readthedocs.io/

---

## 🤖 Autopilot Mode Instructions

When resuming this session:

1. **Read this file first** to understand current state
2. **Start with connection hardening** - Most critical for UX
3. **Then dollchan deep-dive** - Research first, implement later
4. **Test incrementally** - Build, flash, test each change
5. **Document findings** - Update this file with results

**Key Files to Edit:**
- `tools_py/bytebeat_tool.py` - Tool-side handshake
- `examples/console_bytebeat_dev/main/serial_commands.cpp` - Firmware-side commands
- `examples/console_bytebeat_dev/main/main.cpp` - Version reporting

**Testing Workflow:**
1. Edit firmware → Build → Flash → Test
2. Edit tool → Run GUI → Test connection
3. Try problematic formulas → Document results
4. Iterate

---

**Last Updated:** 2024-01-XX (End of GUI integration session)
**Next Session:** Connection hardening + Dollchan analysis
**Status:** Ready for Autopilot Mode 🚀
