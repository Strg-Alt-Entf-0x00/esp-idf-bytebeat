# ESP Bytebeat Console - Quick Start

**🎵 Algorithmische Audio-Synthese auf ESP32-P4!**

---

## 🚀 Schnellstart (3 Schritte!)

### **So einfach geht's:**

1. **ESP32-P4 anschließen** (USB)
2. **COM-Port finden** (Device Manager → z.B. COM4)
3. **Script ausführen:**

```batch
FLASH_AND_RUN.bat COM4
```

**Das war's!** 🎉

Das Script macht **alles** automatisch:
- ✅ Clean Build (alte Dateien löschen → immer frisch!)
- ✅ Von Grund auf neu bauen
- ✅ Auf ESP32 flashen
- ✅ Serial Monitor öffnen

**Dauert:** ~2-3 Minuten beim ersten Mal

---

## 📋 Was du brauchst

### Hardware:
- **ESP32-P4** (empfohlen) oder ESP32-S3
- USB-Kabel (Daten, nicht nur Laden!)
- **Empfohlen:** Audio-Codec (ES8311) für besten Sound
- **Alternative:** Nutze built-in DAC (qualitativ schlechter)

### Software:
- **ESP-IDF v6.0+** ([Download](https://docs.espressif.com/projects/esp-idf/en/stable/esp32/get-started/))
- **Windows** (Scripts sind für Windows, aber Projekt läuft auf Linux/Mac)

---

## 🎮 Nach dem Flash

### **Erwarteter Boot-Output:**
```
Booting ESP Bytebeat Console v1.1.0...
Pure C Engine - No JavaScript!

Chip: ESP32-P4 | PIE: Available
ESP-DSP: Enabled (Hardware-accelerated math)
Sample Rate: 48000 Hz | Chunk: 512 samples

Ready to rock! Send FORMULA: commands...
```

### **Performance Check:**
```
[Core 0] chunk 20: 5 ms, skip=1    ← GUT! (mit ESP-DSP)
```

**Falls ~25ms:** ESP-DSP nicht aktiv → `menuconfig.bat` prüfen!

---

## 🎵 Formeln ausprobieren

### **Manuelle Eingabe (Serial Monitor):**
```
FORMULA:sin(t/100)
FORMULA:sin(t/100)+cos(t/150)
FORMULA:(t*5)&(t>>7)
FORMULA:sin(t/100)*sin(t/50)
```

### **Mit Python Tool (empfohlen!):**
```batch
cd ..\..\tools_py
python bytebeat_tool.py
```

**Features:**
- 🎨 GUI-Modus
- 📚 2422 Formeln-Bibliothek
- ✅ Formel-Validator
- 🔄 Bytecode-Converter

---

## 🛠️ Für Entwickler

**Wenn du aktiv entwickelst**, nutze die Standard ESP-IDF Befehle:

```batch
# Standard Build (schneller, kein clean)
idf.py build

# Flash + Monitor
idf.py -p COM4 flash monitor

# Nur Monitor
idf.py -p COM4 monitor

# Clean Build
idf.py fullclean && idf.py build

# Konfiguration
idf.py menuconfig
```

**Tipp:** Öffne ESP-IDF Command Prompt für automatische Umgebung!

---

## 📖 Beispiel-Formeln

### **Einfache Töne:**
```c
sin(t/100)                           // Sinus-Ton
(t&128) ? 127 : -127                 // Rechteck
abs((t%512)-256)-128                 // Dreieck
```

### **Chip-Music:**
```c
(t*5)&(t>>7)                         // Classic Bytebeat
(t|(t>>9|t>>7))*t&(t>>11|t>>9)       // Techno Beat
((t>>10)&42)*t                        // Melody
```

### **Fortgeschritten:**
```c
sin(t/100)*0.5+sin(t/50)*0.3         // Simple Orgel
sin(t/100+sin(t/200)*10)             // FM Synthesis
sin(t/(50+t/100))*exp(-t/1000)       // Kick Drum
```

**📚 Mehr Formeln:** Siehe `tools_py/library.json` (2422 Stück!)

---

## ⚙️ Konfiguration

### **ESP-DSP aktivieren** (für beste Performance):
```batch
menuconfig.bat
```

Dann:
```
→ ESP Bytebeat Configuration
  → [x] Use ESP-DSP (hardware-accelerated math)
```

### **Audio-Einstellungen:**
```
→ ESP Bytebeat Configuration
  → Sample Rate: 48000 Hz
  → Chunk Size: 512 samples
  → Number of Buffers: 40
```

---

## 🐛 Probleme lösen

### **"idf.py not found"**
**Lösung:** 
1. ESP-IDF installieren: https://docs.espressif.com/projects/esp-idf/en/stable/esp32/get-started/
2. ESP-IDF Command Prompt öffnen
3. Zu diesem Verzeichnis wechseln
4. `FLASH_AND_RUN.bat COM4` ausführen

### **"Port COM4 not found"**
**Lösung:** 
1. Device Manager öffnen (Windows)
2. "Ports (COM & LPT)" erweitern
3. USB Serial Device finden (z.B. COM3)
4. `FLASH_AND_RUN.bat COM3` mit richtigem Port

### **"esp-dsp not found" oder Build-Fehler**
**Lösung:** `FLASH_AND_RUN.bat` nochmal ausführen (lädt Dependencies automatisch)

### **Performance schlecht (~25ms statt ~5ms)**
**Lösung:** 
1. `menuconfig.bat`
2. ESP-DSP aktivieren [✓]
3. `FLASH_AND_RUN.bat` nochmal

### **Kein Sound**
**Lösung:**
1. Audio-Codec angeschlossen?
2. I2S Pins korrekt? (siehe `menuconfig`)
3. Volume zu leise?

---

## 🎯 Nächste Schritte

### **1. Formeln erkunden (Python Tool):**
```batch
cd ..\..\tools_py
python bytebeat_tool.py
```
→ 2422 Formeln-Bibliothek, GUI, Validator!

### **2. Eigene Formeln schreiben:**
- [Bytebeat Tutorial](http://viznut.fi/en/articles/bytebeat_intro.html) (Original)
- [Dollchan Bytebeat Board](http://dollchan.net/bytebeat/) (Community + Formeln)
- [Greggman's Bytebeat](https://greggman.com/downloads/examples/html5bytebeat/html5bytebeat.html) (Interactive)

### **3. Code studieren (für Entwickler):**
- `main/bytebeat_engine.cpp` - Audio Engine
- `../../src/bytebeat_vm.c` - Virtual Machine (40+ Opcodes)
- `../../src/esp_bytebeat.c` - Public API
- `../../include/esp_bytebeat.h` - Header

### **4. Dokumentation lesen:**
```
../../.docs/
├── ARCHITECTURE.md         # System-Design
├── FEATURES.md             # Alle Features
├── RESEARCH_SYNTHESIS.md   # Was ist möglich?
├── RESEARCH_WAVETABLES.md  # Wavetable-Analyse
└── ROADMAP.md              # Zukunfts-Pläne
```

---

## 📚 Dokumentation

**Im Projekt-Root (../../.docs/):**
- `ARCHITECTURE.md` - System-Architektur
- `FEATURES.md` - Alle Features
- `RESEARCH_SYNTHESIS.md` - Was ist möglich?
- `RESEARCH_WAVETABLES.md` - Wavetable-Integration
- `ROADMAP.md` - Zukunfts-Pläne
- `STATUS.md` - Aktueller Status
- `CHANGELOG.md` - Version History

**In diesem Verzeichnis:**
- `README.md` - Diese Datei (Quick Start)
- `FLASH_AND_RUN.bat` - One-Click Build & Flash

---

## ✨ Features

- ✅ **Pure C Engine** - Kein JavaScript!
- ✅ **ESP-DSP** - Hardware-Beschleunigung (3-5x schneller!)
- ✅ **PIE Support** - ESP32-P4/S3 Optimierungen (2-4x schneller!)
- ✅ **13 Math-Funktionen** - sin, cos, tan, sqrt, exp, log, pow, ...
- ✅ **40+ Opcodes** - Vollständige VM
- ✅ **48kHz Stereo** - CD-Quality Output
- ✅ **2422 Formeln** - Riesige Bibliothek
- ✅ **Python Tool** - GUI, Validator, Converter

**Gesamt-Speedup auf ESP32-P4:** 5-7x! 🚀

---

## 🎵 Was ist Bytebeat?

**Bytebeat** = Algorithmische Musik-Synthese

**Prinzip:**
```c
// Einfache Formel erzeugt komplexe Klänge
output = formula(t);  // t = Zeit in Samples
```

**Beispiel:**
```c
output = (t * 5) & (t >> 7);
```

→ Erzeugt interessante, chaotische Melodien!

**Erfunden von:** Viznut (2011)  
**Philosophie:** Maximale Klang-Komplexität aus minimalen Formeln!

---

## 📊 System-Specs

**Hardware:**
- ESP32-P4: 400 MHz, 64 MB PSRAM, PIE
- ESP32-S3: 240 MHz, 8 MB PSRAM, PIE

**Performance:**
- ~5ms pro Chunk (mit ESP-DSP)
- ~25ms ohne ESP-DSP (5x langsamer!)
- 48 kHz Stereo
- Realtime-fähig

**Memory:**
- Binary: ~400 KB
- RAM: ~100 KB
- PSRAM: Optional (für Zukunft)

---

## 🔗 Links

**Projekt:**
- GitHub: `esp-idf-bytebeat` (dieser Repo)
- Version: 1.1.0 (Performance Release)
- Lizenz: MIT

**Bytebeat Community:**
- [Viznut's Blog](http://viznut.fi/en/)
- [Dollchan](http://dollchan.net/bytebeat/)
- [Greggman](https://greggman.com/downloads/examples/html5bytebeat/)

**ESP32 Audio Projekte:**
- [copych/ESP32_SF2_Sampler](https://github.com/copych/ESP32_SF2_Sampler_Synthesizer)
- [copych/ESP32_SD_Sampler](https://github.com/copych/ESP32_SD_Sampler)
- [bartpleiter/ESP32Synth](https://github.com/bartpleiter/ESP32Synth)

---

**Version:** 1.1.0 (Performance Release)  
**Datum:** 2025-01-20  
**Status:** ✅ Production Ready!

**Have fun! 🎵🚀**
