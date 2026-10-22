#!/usr/bin/env python3
"""
ESP Bytebeat Tool - Unified utility for ESP Bytebeat development

Usage:
    # GUI Mode
    python bytebeat_tool.py gui                              # Start GUI
    
    # CLI Mode (Agent-friendly)
    python bytebeat_tool.py list                             # List all categories
    python bytebeat_tool.py list --category classic          # List formulas in category
    python bytebeat_tool.py search "melody"                  # Search formulas
    python bytebeat_tool.py play <index> --port COM4        # Play formula by index on ESP32
    python bytebeat_tool.py send "t>>4" --port COM4         # Compile and send native bytecode
    python bytebeat_tool.py send "sin(t/100)" --mode 1 --sample-rate 8000 --port COM4
    python bytebeat_tool.py stop --port COM4                # Stop ESP32 playback
    
    # PC Preview (NEW!)
    python bytebeat_tool.py preview <index>                  # Preview formula on PC (10s)
    python bytebeat_tool.py preview <index> --duration 30    # Preview for 30 seconds
    python bytebeat_tool.py preview-custom "t>>4"            # Preview custom formula on PC
    
    # Library Management
    python bytebeat_tool.py pack                             # Pack formula library
    python bytebeat_tool.py convert <file>                   # Convert JS→C syntax
    python bytebeat_tool.py validate <file>                  # Validate formula
    
    # Serial Monitor
    python bytebeat_tool.py serial <port>                    # Serial monitor
"""

import argparse
import json
import sys
import threading
import time
import queue
from datetime import datetime
from pathlib import Path
from typing import List, Tuple, Dict, Optional

from native_bytebeat_bytecode import (
    encode_program,
    validate_native_vm_compatibility,
)
from native_bytebeat_compiler import compile_expression_song

VERSION = "2.0.0"
SERIAL_CHUNK_SIZE = 3500
SERIAL_ACK_TIMEOUT_SECONDS = 30.0
MODE_NAME_BY_ID = {
    0: "Bytebeat",
    1: "Floatbeat",
    2: "Signed Bytebeat",
    3: "Funcbeat",
}

# Global preview state
g_preview_player = None
g_preview_lock = threading.Lock()


def _write_serial(serial_port, data: bytes) -> None:
    offset = 0
    while offset < len(data):
        written = serial_port.write(data[offset:])
        if not isinstance(written, int) or written <= 0:
            raise OSError("Serial port accepted no data")
        offset += written
    flush = getattr(serial_port, "flush", None)
    if flush:
        flush()


def _wait_for_serial_ack(serial_port, expected: str, timeout: float) -> str:
    deadline = time.monotonic() + timeout
    while time.monotonic() < deadline:
        line = serial_port.readline()
        if not line:
            continue
        response = line.decode("utf-8", errors="replace").strip()
        if response.startswith("BYTEBEAT:BB:ERR:"):
            raise RuntimeError(response)
        if response == expected:
            return response
    raise TimeoutError(f"Device did not acknowledge {expected!r}")


def transfer_native_bytecode(
    serial_port,
    formula: str,
    mode: str,
    sample_rate: float,
    chunk_size: int = SERIAL_CHUNK_SIZE,
    ack_timeout: float = SERIAL_ACK_TIMEOUT_SECONDS,
) -> int:
    """Compile on the PC and transfer a validated bytecode program to the device."""
    instructions = compile_expression_song(formula, mode)
    validate_native_vm_compatibility(instructions)
    binary = encode_program(instructions, mode, sample_rate)
    if chunk_size <= 0:
        raise ValueError("Serial chunk size must be positive")

    _write_serial(serial_port, b"P:0\n")
    _write_serial(serial_port, f"BBLOAD:{len(binary)}\n".encode("ascii"))
    _wait_for_serial_ack(
        serial_port,
        f"BYTEBEAT:BB:READY:{len(binary)}",
        ack_timeout,
    )

    for offset in range(0, len(binary), chunk_size):
        chunk = binary[offset:offset + chunk_size]
        command = f"BBDATA:{offset}:{chunk.hex()}\n".encode("ascii")
        _write_serial(serial_port, command)
        expected_received = offset + len(chunk)
        _wait_for_serial_ack(
            serial_port,
            f"BYTEBEAT:BB:DATA_OK:{expected_received}",
            ack_timeout,
        )

    _write_serial(serial_port, b"BBEND\n")
    _wait_for_serial_ack(serial_port, "BYTEBEAT:BB:OK", ack_timeout)
    return len(binary)


# ============================================================================
# Formula Converter (JS → C)
# ============================================================================

def convert_js_to_c(formula: str) -> Tuple[str, List[str]]:
    """Convert JavaScript formula to C syntax.
    
    Returns:
        (converted_formula, warnings)
    """
    warnings = []
    original = formula
    
    # Math.* functions → direct functions
    replacements = {
        "Math.sin(": "sin(",
        "Math.cos(": "cos(",
        "Math.tan(": "tan(",
        "Math.sqrt(": "sqrt(",
        "Math.abs(": "abs(",
        "Math.floor(": "floor(",
        "Math.ceil(": "ceil(",
        "Math.round(": "round(",
        "Math.log(": "log(",
        "Math.exp(": "exp(",
        "Math.pow(": "pow(",
        "Math.min(": "min(",
        "Math.max(": "max(",
        "Math.PI": "PI",
    }
    
    for old, new in replacements.items():
        if old in formula:
            formula = formula.replace(old, new)
    
    # Fix scientific notation: 2e3 → 2000, 1e4 → 10000, etc.
    import re
    def expand_scientific(match):
        try:
            num_str = match.group(0)
            num_val = float(num_str)
            # If it's an integer result, format as integer
            if num_val == int(num_val):
                return str(int(num_val))
            else:
                return str(num_val)
        except:
            return match.group(0)
    
    # Match scientific notation: digits + optional decimal + e/E + optional +/- + digits
    formula = re.sub(r'\d+\.?\d*[eE][+-]?\d+', expand_scientific, formula)
    
    # Fix negative t expressions for ESP32 C compiler
    # Replace -t with (0-t) to avoid unary minus issues
    
    # Match -t but not in numbers like -128
    # Look for -t where t is preceded by operators or start of string
    def replace_negative_t(match):
        prefix = match.group(1)
        return f"{prefix}(0-t)"
    
    # Replace -t at start or after operators
    formula = re.sub(r'(^|[^0-9a-zA-Z])(-t)(?=[^0-9a-zA-Z]|$)', 
                     lambda m: m.group(1) + '(0-t)', formula)
    
    # Check for unsupported features
    if "?" in formula and ":" in formula:
        warnings.append("Ternary operators (? :) not supported by ESP32 - formula may fail to compile")
    if "function" in formula:
        warnings.append("'function' keyword not supported - use direct expressions")
    if "=>" in formula:
        warnings.append("Arrow functions (=>) not supported")
    if "let " in formula or "const " in formula or "var " in formula:
        warnings.append("Variables (let/const/var) not supported - only 't' variable")
    if "[" in formula and "]" in formula:
        warnings.append("Arrays not supported yet (stereo planned)")
        
    if formula != original:
        warnings.append(f"Converted: {original} → {formula}")
        
    return formula, warnings


# ============================================================================
# PC Audio Preview Engine
# ============================================================================

class BytebeatPreview:
    """Preview formulas on PC using Dollchan-compatible JavaScript semantics."""

    def __init__(self, sample_rate=8000, volume=1.0):
        self.sample_rate = sample_rate
        self.volume = volume  # 0.0 to 1.0
        self.playing = False
        self.stop_requested = False
        self.stream = None
        self.pa = None

    def start(self, formula: str, duration: float = 10.0, mode: int = 0):
        """Play the formula through Dollchan-compatible JavaScript semantics."""
        try:
            import pyaudio
            import numpy as np
        except ImportError:
            print("ERROR: pyaudio not installed. Run: pip install pyaudio")
            print("Note: On Windows, you may need: pip install pipwin && pipwin install pyaudio")
            return False

        try:
            from bytebeat_js_runtime import FormulaExecutionError, JavaScriptBytebeat
        except ImportError as error:
            print(f"ERROR: PC JavaScript runtime is unavailable: {error}")
            return False

        mode_names = {
            0: "Bytebeat",
            1: "Floatbeat",
            2: "Signed Bytebeat",
            3: "Funcbeat",
        }
        mode_name = mode_names.get(mode)
        if mode_name is None:
            print(f"ERROR: Unsupported preview mode: {mode}")
            return False

        runtime = None
        try:
            runtime = JavaScriptBytebeat(formula, mode_name, self.sample_rate)
        except FormulaExecutionError as error:
            print(f"ERROR: {error}")
            return False

        print(f"Starting PC preview: {formula[:60]}...")
        print(
            f"Sample rate: {self.sample_rate} Hz, Duration: "
            f"{'infinite' if duration == 0 else f'{duration}s'}, "
            f"Mode: {mode_name}, Volume: {int(self.volume * 100)}%"
        )

        if self.stop_requested:
            runtime.close()
            return False
        self.playing = True
        chunk_size = 1024
        total_samples = int(duration * self.sample_rate) if duration > 0 else None
        first_count = chunk_size if total_samples is None else min(chunk_size, total_samples)

        try:
            self.pa = pyaudio.PyAudio()
            first_chunk = runtime.render(0, first_count, timeout_ms=5000)
            self.stream = self.pa.open(
                format=pyaudio.paInt16,
                channels=2 if first_chunk["isStereo"] else 1,
                rate=self.sample_rate,
                output=True,
                frames_per_buffer=1024
            )

            t = 0
            runtime_errors = 0

            while self.playing and not self.stop_requested and (total_samples is None or t < total_samples):
                count = chunk_size if total_samples is None else min(chunk_size, total_samples - t)
                if t == 0:
                    rendered = first_chunk
                else:
                    rendered = runtime.render(t, count, timeout_ms=5000)
                runtime_errors += rendered["errorCount"] + rendered["nonFiniteCount"]
                if first_chunk["isStereo"]:
                    pcm = [
                        channel_sample
                        for left, right in zip(rendered["samples"], rendered["rightSamples"])
                        for channel_sample in (
                            max(-32768, min(32767, round(left * self.volume * 32767))),
                            max(-32768, min(32767, round(right * self.volume * 32767))),
                        )
                    ]
                else:
                    pcm = [
                        max(-32768, min(32767, round(sample * self.volume * 32767)))
                        for sample in rendered["samples"]
                    ]
                if pcm:
                    self.stream.write(np.asarray(pcm, dtype=np.int16).tobytes())
                t += count

            if runtime_errors:
                print(f"WARNING: {runtime_errors} sample(s) failed or were non-finite")

            print("PC preview finished")
            return True
        except Exception as e:
            print(f"ERROR during preview: {e}")
            return False
        finally:
            runtime.close()
            self.stop()

    def stop(self):
        """Stop playback."""
        self.request_stop()
        if self.stream:
            self.stream.stop_stream()
            self.stream.close()
            self.stream = None
        if self.pa:
            self.pa.terminate()
            self.pa = None

    def request_stop(self):
        """Request a stop without closing the stream from another thread."""
        self.stop_requested = True
        self.playing = False


# ============================================================================
# Formula Validator
# ============================================================================

def validate_formula(formula: str) -> Tuple[bool, List[str], List[str]]:
    """Validate formula syntax.
    
    Returns:
        (is_valid, errors, warnings)
    """
    errors = []
    warnings = []
    
    # Check balanced parentheses
    if formula.count("(") != formula.count(")"):
        errors.append(f"Unbalanced parentheses: {formula.count('(')} open, {formula.count(')')} close")
        
    # Check for JS syntax
    if "Math." in formula:
        errors.append("Math.* not supported - use convert command first")
        
    # Check complexity
    depth = 0
    max_depth = 0
    for char in formula:
        if char == "(":
            depth += 1
            max_depth = max(max_depth, depth)
        elif char == ")":
            depth -= 1
            
    if max_depth > 10:
        warnings.append(f"Complex formula (nesting depth: {max_depth}) - might be slow")
        
    if len(formula) > 500:
        warnings.append(f"Long formula ({len(formula)} chars) - consider simplifying")
        
    is_valid = len(errors) == 0
    return is_valid, errors, warnings


# ============================================================================
# Library Packer
# ============================================================================

def pack_library(lib_dir: Path, output_file: Path) -> int:
    """Pack formula library into JSON file.
    
    Returns:
        Number of formulas packed
    """
    library_data = []
    
    # Add system formulas
    library_data.append({
        "category": "System",
        "name": "Silence",
        "author": "System",
        "mode": 0,
        "speed": 1.0,
        "code": "0"
    })
    
    # Scan library directory
    for filepath in sorted(lib_dir.rglob("*.txt")):
        category = filepath.parent.name
        content = filepath.read_text(encoding="utf-8")
        
        # Parse metadata
        name = filepath.stem
        author = "Unknown"
        sample_rate = 8000
        mode_str = "bytebeat"
        
        for line in content.splitlines():
            if line.startswith("// Name:"):
                name = line.split(":", 1)[1].strip()
            elif line.startswith("// Author:"):
                author = line.split(":", 1)[1].strip()
            elif line.startswith("// Sample Rate:"):
                try:
                    sample_rate = int(line.split(":", 1)[1].strip())
                except ValueError:
                    pass
            elif line.startswith("// Mode:"):
                mode_str = line.split(":", 1)[1].strip().lower()
                
        # Determine mode
        mode_int = 2 if "signed" in mode_str else 0
        if "float" in mode_str or "func" in mode_str:
            mode_int = 1
            
        # Auto-convert if needed
        code = content
        if "Math." in code:
            code, _ = convert_js_to_c(code)
            
        library_data.append({
            "category": category,
            "name": name,
            "author": author,
            "mode": mode_int,
            "speed": round(sample_rate / 48000.0, 4),
            "code": code
        })
        
    # Write output
    output_file.write_text(
        json.dumps(library_data, indent=2, ensure_ascii=False),
        encoding="utf-8"
    )
    
    return len(library_data)


# ============================================================================
# Serial Monitor (Simple)
# ============================================================================

def serial_monitor(port: str, baudrate: int = 115200):
    """Simple serial monitor for ESP32."""
    try:
        import serial
    except ImportError:
        print("ERROR: pyserial not installed. Run: pip install pyserial")
        return 1
        
    try:
        ser = serial.Serial(port, baudrate, timeout=0.1)
        print(f"Connected to {port} @ {baudrate}")
        print("Type formulas and press Enter to send. Ctrl+C to exit.\n")
        
        import threading
        
        def read_serial():
            while True:
                try:
                    if ser.in_waiting:
                        line = ser.readline().decode('utf-8', errors='ignore').strip()
                        if line:
                            print(f"ESP32: {line}")
                except Exception as e:
                    break
                    
        reader = threading.Thread(target=read_serial, daemon=True)
        reader.start()
        
        while True:
            try:
                cmd = input("> ")
                if cmd:
                    ser.write(f"{cmd}\n".encode())
            except KeyboardInterrupt:
                print("\nExiting...")
                break
                
        ser.close()
        return 0
        
    except Exception as e:
        print(f"ERROR: {e}")
        return 1


# ============================================================================
# GUI (Tkinter-based)
# ============================================================================

def launch_gui():
    """Launch Professional Production GUI."""
    try:
        import tkinter as tk
        from tkinter import ttk, scrolledtext, messagebox, filedialog
    except ImportError:
        print("ERROR: tkinter not available")
        return 1
        
    try:
        import serial
        import serial.tools.list_ports
        SERIAL_OK = True
    except ImportError:
        SERIAL_OK = False

    try:
        import pyaudio
        AUDIO_OK = True  
    except ImportError:
        AUDIO_OK = False

    # MODES - Following dollchan/greggman standard
    MODES = [
        ("Bytebeat (Unsigned)", 0),
        ("Signed Bytebeat", 2),
        ("Floatbeat", 1),
        ("Funcbeat (PC only)", 3),
    ]

    # SAMPLE RATES - Industry standard
    SAMPLE_RATES = [8000, 11025, 22050, 44100, 48000]

    class ProfessionalBytebeatGUI:
        """Final Production GUI - Zero Placeholders."""

        def __init__(self, root):
            self.root = root
            self.root.title(f"ESP32 Bytebeat Tool v{VERSION}")
            self.root.geometry("1400x900")
            self.root.minsize(1200, 700)

            # State
            self.serial_port = None
            self.preview_thread = None
            self.preview_playing = False
            self.preview_instance = None  # BytebeatPreview instance for Stop button control
            self.library = LibraryManager()
            self.serial_queue = queue.Queue()
            self.running = True

            # Build UI
            self.create_menu()
            self.create_main_layout()
            self.refresh_ports()

            # Start update loop
            self.root.after(100, self.update_loop)

            # Cleanup
            self.root.protocol("WM_DELETE_WINDOW", self.on_closing)

            # Initial log
            self.log(f"ESP32 Bytebeat Tool v{VERSION} initialized", "INFO")
            self.log(f"Library: {len(self.library.formulas)} formulas loaded", "OK")
            if not SERIAL_OK:
                self.log("WARNING: pyserial not installed - serial features disabled", "WARN")
            if not AUDIO_OK:
                self.log("WARNING: pyaudio not installed - PC preview disabled", "WARN")

        def create_menu(self):
            menubar = tk.Menu(self.root)
            self.root.config(menu=menubar)

            file_menu = tk.Menu(menubar, tearoff=0)
            menubar.add_cascade(label="File", menu=file_menu)
            file_menu.add_command(label="Load Formula from File...", command=self.load_formula_file)
            file_menu.add_command(label="Save Formula to File...", command=self.save_formula_file)
            file_menu.add_separator()
            file_menu.add_command(label="Exit", command=self.on_closing)

            tools_menu = tk.Menu(menubar, tearoff=0)
            menubar.add_cascade(label="Tools", menu=tools_menu)
            tools_menu.add_command(label="Validate Formula", command=self.validate_formula)
            tools_menu.add_command(label="Convert JS→C", command=self.convert_js_to_c)
            tools_menu.add_separator()
            tools_menu.add_command(label="Refresh Library", command=self.refresh_library)

            help_menu = tk.Menu(menubar, tearoff=0)
            menubar.add_cascade(label="Help", menu=help_menu)
            help_menu.add_command(label="Documentation", command=self.show_documentation)
            help_menu.add_separator()
            help_menu.add_command(label="About", command=self.show_about)

        def create_main_layout(self):
            toolbar = ttk.Frame(self.root)
            toolbar.pack(fill=tk.X, padx=5, pady=(5, 0))

            toolbar_right = ttk.Frame(toolbar)
            toolbar_right.pack(side=tk.RIGHT)

            ttk.Label(toolbar_right, text="ESP32:").pack(side=tk.LEFT, padx=(0, 5))

            self.port_combo = ttk.Combobox(toolbar_right, width=10, state="readonly")
            self.port_combo.pack(side=tk.LEFT, padx=2)

            ttk.Button(toolbar_right, text="🔄", command=self.refresh_ports, width=3).pack(side=tk.LEFT, padx=2)

            self.connect_btn = tk.Button(toolbar_right, text="⚫ Disconnected",
                                         command=self.toggle_connection,
                                         width=14, relief=tk.RAISED,
                                         bg="#d32f2f", fg="white",
                                         activebackground="#b71c1c",
                                         font=("Arial", 8, "bold"))
            self.connect_btn.pack(side=tk.LEFT, padx=5)

            ttk.Separator(self.root, orient=tk.HORIZONTAL).pack(fill=tk.X, padx=5)

            main_paned = ttk.PanedWindow(self.root, orient=tk.HORIZONTAL)
            main_paned.pack(fill=tk.BOTH, expand=True, padx=5, pady=5)

            left_panel = ttk.Frame(main_paned, width=400)
            main_paned.add(left_panel, weight=30)
            self.create_library_panel(left_panel)

            right_panel = ttk.Frame(main_paned)
            main_paned.add(right_panel, weight=70)

            right_paned = ttk.PanedWindow(right_panel, orient=tk.VERTICAL)
            right_paned.pack(fill=tk.BOTH, expand=True)

            top_panel = ttk.Frame(right_paned)
            right_paned.add(top_panel, weight=60)
            self.create_editor_panel(top_panel)

            bottom_panel = ttk.Frame(right_paned)
            right_paned.add(bottom_panel, weight=40)
            self.create_console_panel(bottom_panel)

        def create_library_panel(self, parent):
            lib_frame = ttk.LabelFrame(parent, text="📚 Formula Library", padding=10)
            lib_frame.pack(fill=tk.BOTH, expand=True)

            search_frame = ttk.Frame(lib_frame)
            search_frame.pack(fill=tk.X, pady=(0, 5))

            self.search_var = tk.StringVar()
            self.category_var = tk.StringVar(value="All")
            self.is_placeholder = True

            ttk.Label(search_frame, text="🔍", font=('Segoe UI', 10)).pack(side=tk.LEFT, padx=(0, 5))

            entry_container = ttk.Frame(search_frame)
            entry_container.pack(side=tk.LEFT, fill=tk.X, expand=True)

            search_entry = ttk.Entry(entry_container, textvariable=self.search_var, width=40)
            search_entry.pack(fill=tk.BOTH, expand=True)

            style = ttk.Style()
            entry_bg = style.lookup('TEntry', 'fieldbackground') or 'white'

            self.counter_label = tk.Label(entry_container, text="", 
                                          bg=entry_bg, fg='gray', 
                                          font=('Segoe UI', 9),
                                          padx=8)
            self.counter_label.place(relx=1.0, rely=0.5, anchor='e')

            def show_placeholder():
                if not self.search_var.get():
                    self.is_placeholder = True
                    search_entry.config(foreground='gray')
                    self.search_var.set("Search for formula...")

            def clear_placeholder(event=None):
                if self.is_placeholder:
                    self.is_placeholder = False
                    self.search_var.set('')
                    search_entry.config(foreground='black')

            def restore_placeholder(event=None):
                if not self.search_var.get():
                    show_placeholder()

            def on_key_release(event=None):
                if not self.is_placeholder:
                    self.filter_library()
                    self.update_counter()

            search_entry.bind('<FocusIn>', clear_placeholder)
            search_entry.bind('<FocusOut>', restore_placeholder)
            search_entry.bind('<KeyRelease>', on_key_release)
            show_placeholder()

            def clear_search():
                self.search_var.set('')
                self.is_placeholder = False
                search_entry.config(foreground='black')
                self.filter_library()
                self.update_counter()
                show_placeholder()

            clear_btn = ttk.Button(search_frame, text="✕", command=clear_search, width=3)
            clear_btn.pack(side=tk.LEFT, padx=(5, 5))

            categories = ["All"] + self.library.get_categories()
            category_combo = ttk.Combobox(search_frame, textvariable=self.category_var,
                                          values=categories, state="readonly", width=15)
            category_combo.pack(side=tk.LEFT, padx=(5, 0))
            category_combo.bind('<<ComboboxSelected>>', lambda e: (self.filter_library(), self.update_counter()))

            tree_frame = ttk.Frame(lib_frame)
            tree_frame.pack(fill=tk.BOTH, expand=True)

            vsb = ttk.Scrollbar(tree_frame, orient="vertical")
            self.formula_tree = ttk.Treeview(tree_frame, columns=("name", "author", "mode"),
                                             show="tree headings", yscrollcommand=vsb.set)
            vsb.config(command=self.formula_tree.yview)

            self.formula_tree.heading("#0", text="#")
            self.formula_tree.heading("name", text="Name")
            self.formula_tree.heading("author", text="Author")
            self.formula_tree.heading("mode", text="M")

            self.formula_tree.column("#0", width=50, stretch=False)
            self.formula_tree.column("name", width=200)
            self.formula_tree.column("author", width=100)
            self.formula_tree.column("mode", width=30, stretch=False)

            self.formula_tree.pack(side=tk.LEFT, fill=tk.BOTH, expand=True)
            vsb.pack(side=tk.RIGHT, fill=tk.Y)

            self.formula_tree.bind('<Double-Button-1>', lambda e: self.load_selected_formula())
            self.search_var.trace('w', lambda *args: self.filter_library())
            self.filter_library()

        def create_editor_panel(self, parent):
            editor_frame = ttk.LabelFrame(parent, text="✏️ Formula Editor", padding=10)
            editor_frame.pack(fill=tk.BOTH, expand=True, padx=5, pady=5)

            self.formula_text = scrolledtext.ScrolledText(editor_frame, height=12, wrap=tk.WORD,
                                                          font=("Consolas", 10))
            self.formula_text.pack(fill=tk.BOTH, expand=True, pady=(0, 5))
            self.formula_text.insert("1.0", "sin(t / 100) + cos(t / 150)")

            btn_frame = ttk.Frame(editor_frame)
            btn_frame.pack(fill=tk.X)

            ttk.Button(btn_frame, text="▶ Send to ESP32", command=self.send_to_esp32, width=16).pack(side=tk.LEFT, padx=2)
            ttk.Button(btn_frame, text="🔊 Preview PC", command=self.preview_pc, width=13).pack(side=tk.LEFT, padx=2)
            ttk.Button(btn_frame, text="⏹ Stop All", command=self.stop_all, width=10).pack(side=tk.LEFT, padx=2)
            ttk.Button(btn_frame, text="✓ Validate", command=self.validate_formula, width=10).pack(side=tk.LEFT, padx=2)
            ttk.Button(btn_frame, text="JS→C", command=self.convert_js_to_c, width=7).pack(side=tk.LEFT, padx=2)
            ttk.Button(btn_frame, text="Clear", command=self.clear_editor, width=7).pack(side=tk.LEFT, padx=2)

            self.mode_var = tk.StringVar(value=MODES[2][0])
            mode_combo = ttk.Combobox(btn_frame, textvariable=self.mode_var,
                                      values=[m[0] for m in MODES], state="readonly", width=18)
            mode_combo.pack(side=tk.LEFT, padx=(10, 2))

            self.rate_var = tk.IntVar(value=8000)
            self.rate_combo = ttk.Combobox(btn_frame, textvariable=self.rate_var,
                                           values=SAMPLE_RATES, state="readonly", width=8)
            self.rate_combo.pack(side=tk.LEFT, padx=2)
            
            # Safety Duration Timer (0 = infinite)
            ttk.Label(btn_frame, text="Duration:").pack(side=tk.LEFT, padx=(10, 2))
            self.duration_var = tk.IntVar(value=5)  # Default: 5 seconds safety limit
            duration_spin = ttk.Spinbox(btn_frame, textvariable=self.duration_var,
                                       from_=0, to=300, width=5)
            duration_spin.pack(side=tk.LEFT, padx=2)
            ttk.Label(btn_frame, text="s (0=∞)", foreground="gray").pack(side=tk.LEFT, padx=2)
            
            # Volume Control (0-100)
            ttk.Label(btn_frame, text="Vol:").pack(side=tk.LEFT, padx=(10, 2))
            self.volume_var = tk.IntVar(value=80)  # Default: 80%
            volume_spin = ttk.Spinbox(btn_frame, textvariable=self.volume_var,
                                     from_=0, to=100, width=4)
            volume_spin.pack(side=tk.LEFT, padx=2)
            ttk.Label(btn_frame, text="%", foreground="gray").pack(side=tk.LEFT)

        def create_console_panel(self, parent):
            console_frame = ttk.LabelFrame(parent, text="📟 Debug Console", padding=10)
            console_frame.pack(fill=tk.BOTH, expand=True, padx=5, pady=5)

            self.console_text = scrolledtext.ScrolledText(console_frame, height=15, wrap=tk.WORD,
                                                          state="disabled", bg="black", fg="lime",
                                                          font=("Consolas", 9))
            self.console_text.pack(fill=tk.BOTH, expand=True)

            ctrl_frame = ttk.Frame(console_frame)
            ctrl_frame.pack(fill=tk.X, pady=(5, 0))

            ttk.Button(ctrl_frame, text="Clear", command=self.clear_console, width=10).pack(side=tk.LEFT, padx=2)
            self.autoscroll_var = tk.BooleanVar(value=True)
            ttk.Checkbutton(ctrl_frame, text="Auto-scroll", variable=self.autoscroll_var).pack(side=tk.LEFT, padx=5)

        def filter_library(self):
            for item in self.formula_tree.get_children():
                self.formula_tree.delete(item)

            query_text = self.search_var.get()
            query = ""
            if query_text and not self.is_placeholder:
                query = query_text.lower()

            category = self.category_var.get()
            formulas = self.library.formulas
            if category != "All":
                formulas = [f for f in formulas if f.get("category") == category]
            if query:
                formulas = [f for f in formulas if
                           query in f.get("name", "").lower() or
                           query in f.get("author", "").lower()]

            mode_map = {0: "U", 1: "F", 2: "S"}
            for i, formula in enumerate(formulas):
                idx = self.library.formulas.index(formula)
                mode_str = mode_map.get(formula.get("mode", 0), "?")
                self.formula_tree.insert("", "end", iid=f"f_{i}", text=f"{idx}",
                                        values=(formula.get("name", "Unnamed"),
                                               formula.get("author", "Unknown"),
                                               mode_str))
            self.update_counter()

        def update_counter(self):
            count = len(self.formula_tree.get_children())
            total = len(self.library.formulas)
            if count == total:
                self.counter_label.config(text=f"{total} formulas")
            else:
                self.counter_label.config(text=f"{count} of {total}")

        def load_selected_formula(self):
            selection = self.formula_tree.selection()
            if not selection:
                return

            item_text = self.formula_tree.item(selection[0], "text")
            idx = int(item_text)
            formula = self.library.get_by_index(idx)

            if formula:
                code = formula.get("code", "")
                self.formula_text.delete("1.0", "end")
                self.formula_text.insert("1.0", code)

                # Set mode
                mode_idx = formula.get("mode", 1)
                for mode_name, mode_val in MODES:
                    if mode_val == mode_idx:
                        self.mode_var.set(mode_name)
                        break

                sample_rate = formula.get("sample_rate")
                for line in code.split('\n'):
                    if sample_rate is None and 'Sample Rate:' in line:
                        try:
                            # Extract number from "// Sample Rate: 8000"
                            rate_str = line.split('Sample Rate:')[1].strip().split()[0]
                            sample_rate = float(rate_str)
                            break
                        except:
                            pass
                
                # Fallback to speed-based calculation
                if sample_rate is None:
                    speed = formula.get("speed", 1.0)
                    sample_rate = int(speed * 48000)
                
                selected_rate = int(round(float(sample_rate)))
                self.rate_combo.configure(values=sorted(set(SAMPLE_RATES + [selected_rate])))
                self.rate_var.set(selected_rate)
                
                self.log(f"Loaded #{idx}: {formula.get('name')} ({selected_rate}Hz)", "INFO")

        def refresh_library(self):
            self.library.load()
            self.filter_library()
            self.log("Library reloaded", "OK")

        def refresh_ports(self):
            if not SERIAL_OK:
                self.log("Serial not available", "ERROR")
                return

            ports = [p.device for p in serial.tools.list_ports.comports()]
            self.port_combo['values'] = ports
            if ports:
                self.port_combo.current(0)
                self.log(f"Found {len(ports)} port(s)", "INFO")

        def toggle_connection(self):
            if self.serial_port and self.serial_port.is_open:
                self.disconnect()
            else:
                self.connect()

        def connect(self):
            if not SERIAL_OK:
                messagebox.showerror("Error", "pyserial not installed!")
                return

            port = self.port_combo.get()
            if not port:
                messagebox.showwarning("No Port", "Select a port first")
                return

            # BUSY state - Orange
            self.connect_btn.config(text="🟠 Connecting...", bg="#ff9800", state="disabled")
            self.root.update()

            try:
                # Open serial port
                self.serial_port = serial.Serial(port, 115200, timeout=0.5)
                time.sleep(0.3)  # Wait for device boot output to finish
                
                # Clear any boot messages
                self.serial_port.reset_input_buffer()
                
                # HANDSHAKE - Send VERSION command
                self.log(f"Testing connection to {port}...", "INFO")
                self.serial_port.write(b"VERSION\n")
                self.serial_port.flush()
                
                # Wait for response - read multiple lines
                response = ""
                found = False
                start_time = time.time()
                while time.time() - start_time < 2.0:  # Longer timeout
                    if self.serial_port.in_waiting:
                        line = self.serial_port.readline().decode('utf-8', errors='ignore').strip()
                        if "BYTEBEAT:" in line:
                            response = line
                            found = True
                            break
                    time.sleep(0.05)
                
                # Verify response
                if found:
                    # CONNECTED - Green
                    self.connect_btn.config(text="🟢 Connected", bg="#388e3c", state="normal",
                                           activebackground="#2e7d32")
                    self.log(f"Connected to {port}", "OK")
                    self.log(f"Device: {response}", "INFO")
                else:
                    # WRONG DEVICE - Orange warning
                    self.serial_port.close()
                    self.serial_port = None
                    self.connect_btn.config(text="🟠 Unknown Device", bg="#ff9800", state="normal",
                                           activebackground="#f57c00")
                    self.log(f"Wrong firmware on {port}", "WARN")
                    self.log(f"No BYTEBEAT response - check firmware", "WARN")
                    messagebox.showwarning("Wrong Device", 
                        f"Device on {port} doesn't respond correctly.\n\n"
                        f"Expected: BYTEBEAT:...\n"
                        f"Got: (timeout or wrong response)\n\n"
                        f"Please flash the bytebeat firmware first.")
                    
            except Exception as e:
                # ERROR - Red
                if self.serial_port:
                    try:
                        self.serial_port.close()
                    except:
                        pass
                    self.serial_port = None
                    
                self.connect_btn.config(text="⚫ Disconnected", bg="#d32f2f", state="normal",
                                       activebackground="#b71c1c")
                self.log(f"Connection failed: {e}", "ERROR")
                messagebox.showerror("Connection Error", str(e))

        def disconnect(self):
            if self.serial_port:
                try:
                    self.serial_port.close()
                except:
                    pass
                self.serial_port = None

            self.connect_btn.config(text="⚫ Disconnected", bg="#d32f2f",
                                   activebackground="#b71c1c")
            self.log("Disconnected", "INFO")

        def send_to_esp32(self):
            if not self.serial_port or not self.serial_port.is_open:
                messagebox.showwarning("Not Connected", "Connect to ESP32 first")
                return

            formula = self.formula_text.get("1.0", "end-1c").strip()

            try:
                # Get selected sample rate and mode
                sample_rate = self.rate_var.get()
                mode_name = self.mode_var.get()
                mode_val = 1
                for name, val in MODES:
                    if name == mode_name:
                        mode_val = val
                        break
                native_mode = MODE_NAME_BY_ID[mode_val]
                
                # Calculate speed multiplier for ESP32 (which runs at 48kHz)
                # Speed = formula_sample_rate / esp32_dac_rate
                # Example: 8000 Hz formula on 48000 Hz DAC = 8000/48000 = 0.1667
                speed = sample_rate / 48000.0
                
                # Send volume (0-100 → 0-255)
                volume_percent = self.volume_var.get()
                volume_byte = int(volume_percent * 255 / 100)
                self.serial_port.write(f"V:{volume_byte}\n".encode('utf-8'))
                self.log(f"Volume: {volume_percent}% ({volume_byte}/255)", "TX")
                time.sleep(0.05)
                
                # Send speed (CRITICAL for correct playback!)
                self.serial_port.write(f"S:{speed:.4f}\n".encode('utf-8'))
                self.log(f"Speed: {speed:.4f} ({sample_rate}Hz on 48kHz DAC)", "TX")
                time.sleep(0.05)
                
                # Send duration
                duration = self.duration_var.get()
                self.serial_port.write(f"T:{duration}\n".encode('utf-8'))
                self.log(f"Duration: {duration}s {'(infinite)' if duration == 0 else ''}", "TX")
                time.sleep(0.05)
                
                # Send mode
                self.serial_port.write(f"M:{mode_val}\n".encode('utf-8'))
                self.log(f"Mode: {mode_name} ({mode_val})", "TX")
                time.sleep(0.05)
                
                binary_size = transfer_native_bytecode(
                    self.serial_port,
                    formula,
                    native_mode,
                    sample_rate,
                )
                self.log(
                    f"Native bytecode loaded: {binary_size} bytes, "
                    f"{native_mode}, {sample_rate} Hz",
                    "OK",
                )
            except Exception as e:
                self.log(f"Send failed: {e}", "ERROR")
                messagebox.showerror("Send Error", str(e))

        def preview_pc(self):
            if not AUDIO_OK:
                messagebox.showerror("PyAudio Missing", "Install: pip install pyaudio")
                return

            # Stop any running preview first
            if self.preview_playing:
                self.stop_preview()
                time.sleep(0.1)

            formula = self.formula_text.get("1.0", "end-1c").strip()
            mode_name = self.mode_var.get()
            mode_val = 1
            for name, val in MODES:
                if name == mode_name:
                    mode_val = val
                    break

            sample_rate = self.rate_var.get()
            duration = self.duration_var.get()
            volume = self.volume_var.get() / 100.0
            pc_duration = float(duration)
            self.log(f"PC Preview: {formula[:50]}... ({sample_rate}Hz, {duration}s)", "INFO")

            preview = BytebeatPreview(sample_rate=sample_rate, volume=volume)
            self.preview_instance = preview
            self.preview_playing = True

            def preview_thread():
                try:
                    started = preview.start(formula, duration=pc_duration, mode=mode_val)
                    if started:
                        self.log("PC preview finished", "OK")
                    else:
                        self.log("PC preview did not start; see the reported error", "ERROR")
                except Exception as e:
                    self.log(f"Preview error: {e}", "ERROR")
                finally:
                    if self.preview_instance is preview:
                        self.preview_instance = None
                        self.preview_playing = False

            self.preview_thread = threading.Thread(target=preview_thread, daemon=True)
            self.preview_thread.start()

        def stop_preview(self):
            """Stop PC preview immediately."""
            if self.preview_instance:
                self.preview_instance.request_stop()
                self.log("PC preview stopped", "INFO")
            self.preview_playing = False

        def stop_all(self):
            # Stop PC preview immediately
            self.stop_preview()

            # Stop ESP32
            if self.serial_port and self.serial_port.is_open:
                try:
                    self.serial_port.write(b"STOP\n")
                    self.log("ESP32 stopped", "TX")
                except Exception as e:
                    self.log(f"Stop failed: {e}", "ERROR")

        def validate_formula(self):
            formula = self.formula_text.get("1.0", "end-1c").strip()
            valid, errors, warnings = validate_formula(formula)

            if valid:
                msg = "✓ Formula is valid!"
                if warnings:
                    msg += "\n\nWarnings:\n" + "\n".join(f"• {w}" for w in warnings)
                messagebox.showinfo("Validation", msg)
                self.log("Validation: OK", "OK")
            else:
                msg = "✗ Errors:\n" + "\n".join(f"• {e}" for e in errors)
                messagebox.showerror("Validation Failed", msg)
                self.log("Validation: FAILED", "ERROR")

        def convert_js_to_c(self):
            formula = self.formula_text.get("1.0", "end-1c").strip()
            converted, warnings = convert_js_to_c(formula)

            if converted != formula:
                self.formula_text.delete("1.0", "end")
                self.formula_text.insert("1.0", converted)
                self.log("Converted JS→C", "OK")
            else:
                self.log("Already C syntax", "INFO")

        def clear_editor(self):
            self.formula_text.delete("1.0", "end")
            self.log("Editor cleared", "INFO")

        def load_formula_file(self):
            filename = filedialog.askopenfilename(
                title="Load Formula",
                filetypes=[("Text files", "*.txt"), ("All files", "*.*")]
            )
            if filename:
                try:
                    with open(filename, 'r', encoding='utf-8') as f:
                        content = f.read()
                    lines = [line for line in content.split('\n') if not line.strip().startswith('//')]
                    clean = '\n'.join(lines).strip()
                    self.formula_text.delete("1.0", "end")
                    self.formula_text.insert("1.0", clean)
                    self.log(f"Loaded: {Path(filename).name}", "OK")
                except Exception as e:
                    messagebox.showerror("Load Error", str(e))
                    self.log(f"Load failed: {e}", "ERROR")

        def save_formula_file(self):
            filename = filedialog.asksaveasfilename(
                title="Save Formula",
                defaultextension=".txt",
                filetypes=[("Text files", "*.txt"), ("All files", "*.*")]
            )
            if filename:
                try:
                    formula = self.formula_text.get("1.0", "end-1c").strip()
                    with open(filename, 'w', encoding='utf-8') as f:
                        f.write(formula)
                    self.log(f"Saved: {Path(filename).name}", "OK")
                except Exception as e:
                    messagebox.showerror("Save Error", str(e))
                    self.log(f"Save failed: {e}", "ERROR")

        def log(self, message, level="INFO"):
            timestamp = datetime.now().strftime("%H:%M:%S.%f")[:-3]
            colors = {
                "INFO": "white",
                "OK": "lime",
                "WARN": "yellow",
                "ERROR": "red",
                "TX": "cyan",
                "RX": "lightgreen"
            }
            color = colors.get(level, "white")
            formatted = f"[{timestamp}] [{level:5}] {message}"

            self.console_text.config(state="normal")
            self.console_text.insert("end", formatted + "\n", level)
            self.console_text.tag_config(level, foreground=color)

            if self.autoscroll_var.get():
                self.console_text.see("end")
            self.console_text.config(state="disabled")

        def clear_console(self):
            self.console_text.config(state="normal")
            self.console_text.delete("1.0", "end")
            self.console_text.config(state="disabled")

        def show_documentation(self):
            doc = f"""ESP32 Bytebeat Tool v{VERSION}

MODES:
• Bytebeat (Unsigned): 0-255 output
• Signed Bytebeat: -128 to 127 output  
• Floatbeat: -1.0 to 1.0 output (for sin/cos)

SAMPLE RATES:
• 8000 Hz - Standard (low CPU)
• 44100 Hz - CD Quality
• 48000 Hz - Professional
"""
            messagebox.showinfo("Documentation", doc)

        def show_about(self):
            about = f"""ESP32 Bytebeat Tool v{VERSION}
Professional GUI for ESP32 Bytebeat Development
"""
            messagebox.showinfo("About", about)

        def update_loop(self):
            if self.serial_port and self.serial_port.is_open:
                try:
                    if self.serial_port.in_waiting:
                        line = self.serial_port.readline().decode('utf-8', errors='ignore').strip()
                        if line:
                            self.log(line, "RX")
                except:
                    pass
            if self.running:
                self.root.after(100, self.update_loop)

        def on_closing(self):
            if messagebox.askokcancel("Quit", "Exit ESP32 Bytebeat Tool?"):
                self.running = False
                self.preview_playing = False
                if self.serial_port:
                    self.disconnect()
                self.root.destroy()

    root = tk.Tk()
    app = ProfessionalBytebeatGUI(root)
    root.mainloop()
    return 0


# ============================================================================
# Library Manager
# ============================================================================

class LibraryManager:
    """Manages the dollchan bytebeat library."""
    
    def __init__(self, lib_path: Optional[Path] = None):
        if lib_path is not None:
            self.lib_path = lib_path
        else:
            tools_dir = Path(__file__).parent
            library_path = tools_dir / "library" / "library.json"
            legacy_path = tools_dir / "library.json"
            self.lib_path = library_path if library_path.exists() else legacy_path
        self.formulas = []
        self.load()
        
    def load(self):
        if self.lib_path.exists():
            try:
                self.formulas = json.loads(self.lib_path.read_text(encoding="utf-8"))
            except Exception as e:
                print(f"ERROR loading library: {e}")
                self.formulas = []
        else:
            self.formulas = []
            
    def get_categories(self) -> List[str]:
        return sorted(set(f.get("category", "Unknown") for f in self.formulas))
        
    def list_by_category(self, category: Optional[str] = None) -> List[Dict]:
        if category:
            return [f for f in self.formulas if f.get("category") == category]
        return self.formulas
        
    def search(self, query: str) -> List[Dict]:
        query_lower = query.lower()
        results = []
        for f in self.formulas:
            if (query_lower in f.get("name", "").lower() or
                query_lower in f.get("author", "").lower() or
                query_lower in f.get("category", "").lower() or
                query_lower in f.get("code", "").lower()):
                results.append(f)
        return results
        
    def get_by_index(self, index: int) -> Optional[Dict]:
        if 0 <= index < len(self.formulas):
            return self.formulas[index]
        return None


# ============================================================================
# ESP32 Serial Controller
# ============================================================================

class ESP32Controller:
    """Handles serial communication with ESP32."""
    
    def __init__(self, port: str, baudrate: int = 115200):
        self.port = port
        self.baudrate = baudrate
        self.ser = None
        
    def connect(self):
        try:
            import serial
        except ImportError:
            print("ERROR: pyserial not installed.")
            return False
            
        try:
            self.ser = serial.Serial(self.port, self.baudrate, timeout=0.1)
            return True
        except Exception as e:
            print(f"ERROR connecting to {self.port}: {e}")
            return False
            
    def disconnect(self):
        if self.ser and self.ser.is_open:
            self.ser.close()
            
    def send_formula(
        self,
        formula: str,
        mode: int = 0,
        speed: float = 1.0,
        sample_rate: Optional[float] = None,
    ):
        if not self.ser or not self.ser.is_open:
            print("ERROR: Not connected")
            return False

        try:
            rate = sample_rate if sample_rate is not None else speed * 48000.0
            mode_name = MODE_NAME_BY_ID[mode]
            self.ser.write(f"S:{speed:.9f}\n".encode("ascii"))
            transfer_native_bytecode(self.ser, formula, mode_name, rate)
            return True
        except Exception as e:
            print(f"ERROR compiling or transferring native bytecode: {e}")
            return False
            
    def stop(self):
        if not self.ser or not self.ser.is_open:
            return False
        try:
            _write_serial(self.ser, b"STOP\n")
            return True
        except OSError as error:
            print(f"ERROR stopping playback: {error}")
            return False


# ============================================================================
# CLI Commands
# ============================================================================

def cmd_list(args):
    lib = LibraryManager()
    if args.categories:
        categories = lib.get_categories()
        print(f"\nFound {len(categories)} categories:")
        for i, cat in enumerate(categories, 1):
            count = len(lib.list_by_category(cat))
            print(f"  {i:2}. {cat:20} ({count} formulas)")
        return 0
        
    formulas = lib.list_by_category(args.category)
    if not formulas:
        print("No formulas found")
        return 1
        
    for i, f in enumerate(formulas):
        mode_str = {0: "Unsigned", 1: "Float", 2: "Signed"}.get(f.get("mode", 0), "Unknown")
        print(f"{i:5}  {f.get('category', 'N/A'):15}  {f.get('name', 'Unnamed')[:40]:40}  {mode_str}")
    return 0


def cmd_search(args):
    lib = LibraryManager()
    results = lib.search(args.query)
    for f in results:
        print(f"{f.get('name')} ({f.get('category')})")
    return 0


def cmd_play(args):
    lib = LibraryManager()
    formula = lib.get_by_index(args.index)
    if not formula:
        print(f"Formula #{args.index} not found")
        return 1
    esp = ESP32Controller(args.port)
    if not esp.connect():
        return 1
    mode = formula.get("mode", 0)
    sample_rate = formula.get("sample_rate", formula.get("speed", 1.0) * 48000.0)
    speed = float(sample_rate) / 48000.0
    success = esp.send_formula(
        formula.get("code", ""),
        mode=mode,
        speed=speed,
        sample_rate=sample_rate,
    )
    esp.disconnect()
    return 0 if success else 1


def cmd_send(args):
    esp = ESP32Controller(args.port)
    if not esp.connect():
        return 1
    success = esp.send_formula(
        args.formula,
        mode=args.mode,
        speed=args.sample_rate / 48000.0,
        sample_rate=args.sample_rate,
    )
    esp.disconnect()
    return 0 if success else 1


def cmd_stop(args):
    esp = ESP32Controller(args.port)
    if not esp.connect():
        return 1
    success = esp.stop()
    esp.disconnect()
    return 0 if success else 1


def cmd_preview(args):
    lib = LibraryManager()
    formula_data = lib.get_by_index(args.index)
    if not formula_data:
        print(f"Formula #{args.index} not found")
        return 1
    preview = BytebeatPreview(sample_rate=args.sample_rate or 8000)
    success = preview.start(formula_data.get('code', '0'), duration=args.duration or 10.0, mode=formula_data.get('mode', 0))
    return 0 if success else 1


def cmd_preview_custom(args):
    preview = BytebeatPreview(sample_rate=args.sample_rate or 8000)
    success = preview.start(args.formula, duration=args.duration or 10.0, mode=args.mode or 0)
    return 0 if success else 1


def cmd_pack(args):
    lib_dir = Path(args.library or Path(__file__).parent / "dollchan_library")
    output = Path(args.output or Path(__file__).parent / "library" / "library.json")
    if not lib_dir.exists():
        print(f"Library directory not found: {lib_dir}")
        return 1
    count = pack_library(lib_dir, output)
    print(f"Packed {count} formulas → {output}")
    return 0


def cmd_convert(args):
    input_file = Path(args.file)
    formula = input_file.read_text(encoding="utf-8")
    converted, warnings = convert_js_to_c(formula)
    print(converted)
    return 0


def cmd_validate(args):
    input_file = Path(args.file)
    formula = input_file.read_text(encoding="utf-8")
    valid, errors, _ = validate_formula(formula)
    return 0 if valid else 1


# ============================================================================
# Main
# ============================================================================

def main():
    parser = argparse.ArgumentParser(description="ESP Bytebeat Tool v" + VERSION)
    subparsers = parser.add_subparsers(dest="command", help="Command to run")
    
    subparsers.add_parser("gui", help="Launch GUI")
    
    list_parser = subparsers.add_parser("list", help="List formulas")
    list_parser.add_argument("--category", help="Filter by category")
    list_parser.add_argument("--categories", action="store_true", help="List all categories")
    
    search_parser = subparsers.add_parser("search", help="Search formulas")
    search_parser.add_argument("query", help="Search query")
    
    play_parser = subparsers.add_parser("play", help="Play formula by index")
    play_parser.add_argument("index", type=int, help="Formula index")
    play_parser.add_argument("--port", required=True, help="Serial port")
    
    send_parser = subparsers.add_parser("send", help="Send custom formula")
    send_parser.add_argument("formula", help="Formula code")
    send_parser.add_argument("--port", required=True, help="Serial port")
    send_parser.add_argument("--mode", type=int, choices=(0, 1, 2, 3), default=0)
    send_parser.add_argument("--sample-rate", type=float, default=8000.0)
    
    stop_parser = subparsers.add_parser("stop", help="Stop playback")
    stop_parser.add_argument("--port", required=True, help="Serial port")
    
    preview_parser = subparsers.add_parser("preview", help="Preview formula on PC")
    preview_parser.add_argument("index", type=int, help="Formula index")
    preview_parser.add_argument("--duration", type=float, default=10.0)
    preview_parser.add_argument("--sample-rate", type=int)
    
    preview_custom_parser = subparsers.add_parser("preview-custom", help="Preview custom formula on PC")
    preview_custom_parser.add_argument("formula", help="Formula code")
    preview_custom_parser.add_argument("--duration", type=float, default=10.0)
    preview_custom_parser.add_argument("--sample-rate", type=int)
    preview_custom_parser.add_argument("--mode", type=int, default=0)
    
    pack_parser = subparsers.add_parser("pack", help="Pack formula library")
    pack_parser.add_argument("--library")
    pack_parser.add_argument("--output")
    
    convert_parser = subparsers.add_parser("convert", help="Convert JS→C syntax")
    convert_parser.add_argument("file")
    
    validate_parser = subparsers.add_parser("validate", help="Validate formula")
    validate_parser.add_argument("file")
    
    serial_parser = subparsers.add_parser("serial", help="Serial monitor")
    serial_parser.add_argument("port")
    serial_parser.add_argument("--baudrate", type=int, default=115200)
    
    args = parser.parse_args()
    
    if not args.command:
        parser.print_help()
        return 0
        
    if args.command == "gui":
        return launch_gui()
    elif args.command == "list":
        return cmd_list(args)
    elif args.command == "search":
        return cmd_search(args)
    elif args.command == "play":
        return cmd_play(args)
    elif args.command == "send":
        return cmd_send(args)
    elif args.command == "stop":
        return cmd_stop(args)
    elif args.command == "preview":
        return cmd_preview(args)
    elif args.command == "preview-custom":
        return cmd_preview_custom(args)
    elif args.command == "pack":
        return cmd_pack(args)
    elif args.command == "convert":
        return cmd_convert(args)
    elif args.command == "validate":
        return cmd_validate(args)
    elif args.command == "serial":
        return serial_monitor(args.port, args.baudrate)
    else:
        parser.print_help()
        return 1


if __name__ == "__main__":
    sys.exit(main())