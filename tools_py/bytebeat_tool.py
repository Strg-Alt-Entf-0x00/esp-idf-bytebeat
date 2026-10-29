#!/usr/bin/env python3
"""Desktop console and automation CLI for the ESP Bytebeat development firmware."""

from __future__ import annotations

import argparse
import json
import math
import queue
import sys
import threading
import time
from datetime import datetime
from pathlib import Path
from typing import Any, Callable

try:
    import serial
    from serial.tools import list_ports
except ImportError:
    serial = None
    list_ports = None


VERSION = "3.0.0"
FIRMWARE_ID = "TOOL_ID:esp-idf-bytebeat:console_bytebeat_dev:2"
BAUD_RATE = 115200
DAC_SAMPLE_RATE = 48000
SAMPLE_RATES = (8000, 11025, 22050, 44100, 48000)
SAMPLE_RATE_LABELS = {
    8000: "8 kHz",
    11025: "11.025 kHz",
    22050: "22.05 kHz",
    44100: "44.1 kHz",
    48000: "48 kHz",
}
SAMPLE_RATE_VALUES = {label: rate for rate, label in SAMPLE_RATE_LABELS.items()}
MODES = {
    0: "Unsigned Bytebeat",
    1: "Floatbeat",
    2: "Signed Bytebeat",
}
SERIAL_LINE_LIMIT = 8191
MAX_FORMULA_BYTES = SERIAL_LINE_LIMIT - len("FORMULA:".encode("ascii"))
DEFAULT_SAFETY_SECONDS = 60
IDENTITY_TIMEOUT_SECONDS = 10.0
FIRMWARE_BOOT_DELAY_SECONDS = 1.2
SERIAL_READ_TIMEOUT_SECONDS = 0.05
STOP_CONFIRM_TIMEOUT_SECONDS = 2.0
FORMULA_VALIDATION_TIMEOUT_SECONDS = 5.0
START_CONFIRM_TIMEOUT_SECONDS = 5.0
REPOSITORY_ROOT = Path(__file__).resolve().parent.parent
CATALOG_PATH = REPOSITORY_ROOT / "data" / "formula_library" / "library_validated.json"


class ToolError(Exception):
    """Expected user-facing tool or device error."""


def verify_firmware_identity(serial_port, timeout: float = IDENTITY_TIMEOUT_SECONDS):
    """Query identity and return the reported ID; callers must compare it exactly."""
    if not math.isfinite(timeout) or timeout <= 0:
        raise ValueError("Identity timeout must be a positive finite number.")

    reset_input_buffer = getattr(serial_port, "reset_input_buffer", None)
    if callable(reset_input_buffer):
        reset_input_buffer()

    deadline = time.monotonic() + timeout
    next_probe = 0.0
    while time.monotonic() < deadline:
        now = time.monotonic()
        if now >= next_probe:
            _write_all(serial_port, b"TOOL_ID\n")
            next_probe = now + 1.0

        response = serial_port.readline()
        if not response:
            continue
        line = response.decode("utf-8", errors="replace").strip()
        if line.startswith("TOOL_ID:"):
            return line
    return None


def _write_all(serial_port, data: bytes) -> None:
    offset = 0
    while offset < len(data):
        written = serial_port.write(data[offset:])
        if not isinstance(written, int) or written <= 0:
            raise OSError("The serial port accepted no data.")
        offset += written
    flush = getattr(serial_port, "flush", None)
    if callable(flush):
        flush()


def validate_formula(formula: str) -> list[str]:
    errors = []
    if not formula or not formula.strip():
        errors.append("Formula is empty.")
        return errors
    if "\n" in formula or "\r" in formula:
        errors.append(
            "The firmware serial protocol accepts one formula line at a time."
        )
    if "\x00" in formula:
        errors.append("Formula contains a null character.")
    byte_count = len(formula.encode("utf-8"))
    if byte_count > MAX_FORMULA_BYTES:
        errors.append(
            f"Formula is {byte_count} UTF-8 bytes; the firmware limit is "
            f"{MAX_FORMULA_BYTES} bytes."
        )

    depth = 0
    for character in formula:
        if character == "(":
            depth += 1
        elif character == ")":
            depth -= 1
            if depth < 0:
                errors.append(
                    "Formula has a closing parenthesis without a matching opening parenthesis."
                )
                break
    if depth > 0:
        errors.append(f"Formula has {depth} unmatched opening parenthesis(es).")
    return errors


def validate_device_settings(
    formula: str,
    mode: int,
    sample_rate: int,
    volume: int,
    duration: int,
) -> None:
    errors = validate_formula(formula)
    if mode not in MODES:
        errors.append(
            "The device supports modes 0 (unsigned), 1 (float), and 2 (signed)."
        )
    if sample_rate not in SAMPLE_RATES:
        errors.append(
            f"Sample rate must be one of: {', '.join(map(str, SAMPLE_RATES))} Hz."
        )
    if not 0 <= volume <= 100:
        errors.append("Volume must be between 0 and 100 percent.")
    if not 0 <= duration <= 3600:
        errors.append("Safety timeout must be between 0 and 3600 seconds.")
    if errors:
        raise ValueError("\n".join(errors))


def build_play_commands(
    formula: str,
    mode: int,
    sample_rate: int,
    volume: int,
    duration: int,
) -> list[str]:
    validate_device_settings(formula, mode, sample_rate, volume, duration)
    volume_byte = round(volume * 255 / 100)
    speed = sample_rate / DAC_SAMPLE_RATE
    return [
        "P:0",
        f"V:{volume_byte}",
        f"S:{speed:.4f}",
        f"T:{duration}",
        f"M:{mode}",
        f"FORMULA:{formula}",
    ]


def _read_catalog(path: Path = CATALOG_PATH) -> list[dict[str, Any]]:
    try:
        value = json.loads(path.read_text(encoding="utf-8"))
    except (OSError, json.JSONDecodeError) as error:
        raise ToolError(f"Could not load formula catalog at {path}: {error}") from error
    if not isinstance(value, list):
        raise ToolError(f"Formula catalog at {path} must contain a JSON array.")

    formulas = []
    for index, item in enumerate(value):
        if not isinstance(item, dict):
            raise ToolError(f"Formula catalog entry {index} is not an object.")
        if not isinstance(item.get("code"), str):
            raise ToolError(f"Formula catalog entry {index} has no formula string.")
        formulas.append(item)
    return formulas


class FormulaLibrary:
    def __init__(self, path: Path = CATALOG_PATH):
        self.path = path
        self.formulas = _read_catalog(path)

    def categories(self) -> list[str]:
        return sorted(
            {str(item.get("category", "Unknown")) for item in self.formulas},
            key=str.casefold,
        )

    def search(
        self, query: str = "", category: str = "All"
    ) -> list[tuple[int, dict[str, Any]]]:
        needle = query.casefold().strip()
        results = []
        for index, formula in enumerate(self.formulas):
            formula_category = str(formula.get("category", "Unknown"))
            if category != "All" and formula_category != category:
                continue
            searchable = " ".join(
                (
                    str(formula.get("name", "")),
                    str(formula.get("author", "")),
                    formula_category,
                    str(formula.get("code", "")),
                )
            ).casefold()
            if needle and needle not in searchable:
                continue
            results.append((index, formula))
        return results

    def get(self, index: int) -> dict[str, Any]:
        if index < 0 or index >= len(self.formulas):
            raise ToolError(f"Formula index {index} is outside the catalog.")
        return self.formulas[index]


class SerialWorker(threading.Thread):
    """Owns the serial connection and keeps all blocking I/O off the UI thread."""

    def __init__(
        self,
        events: queue.Queue,
        jobs: queue.Queue,
        serial_factory: Callable[..., Any] | None = None,
        boot_delay: float = FIRMWARE_BOOT_DELAY_SECONDS,
        identity_timeout: float = IDENTITY_TIMEOUT_SECONDS,
    ):
        super().__init__(name="bytebeat-serial-worker", daemon=True)
        self.events = events
        self.jobs = jobs
        self.serial_factory = serial_factory
        self.boot_delay = boot_delay
        self.identity_timeout = identity_timeout
        self.port = None
        self.verified = False
        self.running = True
        self.playback_state = "unknown"
        self.playback_started_by_tool = False

    def _emit(self, *event) -> None:
        self.events.put(event)

    def _log(self, level: str, message: str) -> None:
        self._emit("log", level, message)

    def _factory(self):
        if self.serial_factory is not None:
            return self.serial_factory
        if serial is None:
            raise ToolError(
                "pyserial is not installed. Run start_bytebeat_tool.bat or install "
                "tools_py\\requirements.txt."
            )
        return serial.Serial

    def _connect(self, job_id: int, port_name: str) -> None:
        self._emit("connection", "connecting", port_name)
        self._log(
            "INFO",
            f"Opening {port_name} at {BAUD_RATE} baud. A USB-UART adapter may reset the board.",
        )
        try:
            self.port = self._factory()(
                port_name,
                BAUD_RATE,
                timeout=SERIAL_READ_TIMEOUT_SECONDS,
                write_timeout=2.0,
            )
            if self.boot_delay:
                time.sleep(self.boot_delay)
            self._emit("connection", "verifying", port_name)
            identity = verify_firmware_identity(self.port, self.identity_timeout)
            if identity != FIRMWARE_ID:
                if identity is None:
                    raise ToolError(
                        f"No firmware identity response from {port_name}. Confirm the "
                        "Bytebeat development firmware is running and retry the port."
                    )
                raise ToolError(
                    f"Wrong firmware on {port_name}. Expected {FIRMWARE_ID}; "
                    f"received {identity}."
                )
            self.verified = True
            self.playback_state = "unknown"
            self.playback_started_by_tool = False
            self._emit("connection", "connected", port_name)
            self._emit("playback", "unknown")
            self._emit("playback_owner", False)
            self._log("INFO", f"Verified firmware: {identity}")
            self._log(
                "INFO", "Connected. No formula was sent and playback was not changed."
            )
            self._emit("job_done", job_id, "connect", True, "Firmware verified.")
        except Exception as error:
            self._close_port()
            self._emit("connection", "disconnected", "")
            self._emit("playback", "unknown")
            self._emit("playback_owner", False)
            self._emit("job_done", job_id, "connect", False, str(error))

    def _send(self, job_id: int, payload: dict[str, Any]) -> None:
        action = "send"
        wrote_to_device = False
        try:
            self._require_verified()
            commands = build_play_commands(**payload)
            self._validate_formula(commands[-1][len("FORMULA:") :])
            for command in commands:
                if command.startswith("FORMULA:"):
                    self.playback_started_by_tool = True
                    self._emit("playback_owner", True)
                _write_all(self.port, f"{command}\n".encode("utf-8"))
                wrote_to_device = True
                self._log("TX", command)
            self.playback_state = "starting"
            self.playback_started_by_tool = True
            self._emit("playback", "starting")
            self._emit(
                "job_done",
                job_id,
                action,
                True,
                "Formula sent. Waiting for firmware playback response.",
            )
        except Exception as error:
            if wrote_to_device:
                self.playback_state = "unknown"
                self._emit("playback", "unknown")
            self._log("ERROR", f"Could not send formula: {error}")
            self._emit("job_done", job_id, action, False, str(error))

    def _validate_formula(self, formula: str) -> None:
        _write_all(self.port, f"VLD:{formula}\n".encode("utf-8"))
        self._log("TX", f"VLD:{formula}")
        deadline = time.monotonic() + FORMULA_VALIDATION_TIMEOUT_SECONDS
        while time.monotonic() < deadline and self.verified:
            if self._read_one_line():
                if self._last_line == "BYTEBEAT:FORMULA:VALID":
                    self._log("INFO", "Firmware accepted formula syntax.")
                    return
                if self._last_line == "BYTEBEAT:FORMULA:INVALID":
                    raise ToolError(
                        "Firmware rejected this formula. The device compiler "
                        "supports a smaller expression syntax than some catalog "
                        "entries; no playback command was sent."
                    )
            else:
                time.sleep(0.01)
        raise ToolError(
            "Firmware did not confirm formula validation. No playback command "
            "was sent; reconnect and verify the Bytebeat firmware version."
        )

    def _stop(self, job_id: int, action: str = "stop") -> None:
        try:
            self._require_verified()
            _write_all(self.port, b"STOP\n")
            self._log("TX", "STOP")
            confirmed = self._wait_for_playback_message(
                "STOPPED", STOP_CONFIRM_TIMEOUT_SECONDS
            )
            if confirmed:
                self.playback_state = "idle"
                self.playback_started_by_tool = False
                self._emit("playback", "idle")
                message = "Firmware confirmed playback stopped."
                self._log("INFO", message)
                self._emit("job_done", job_id, action, True, message)
            else:
                self.playback_state = "unknown"
                self._emit("playback", "unknown")
                message = (
                    "STOP was sent, but firmware did not confirm the stopped state."
                )
                self._log("WARN", message)
                self._emit("job_done", job_id, action, False, message)
        except Exception as error:
            self._log("ERROR", f"Could not stop playback: {error}")
            self._emit("job_done", job_id, action, False, str(error))

    def _wait_for_playback_message(self, expected: str, timeout: float) -> bool:
        deadline = time.monotonic() + timeout
        while time.monotonic() < deadline and self.verified:
            if self._read_one_line():
                event = self._last_line
                if f"Playback {expected}" in event:
                    return True
            else:
                time.sleep(0.01)
        return False

    def _require_verified(self) -> None:
        if (
            not self.verified
            or self.port is None
            or not getattr(self.port, "is_open", True)
        ):
            raise ToolError("No verified Bytebeat development firmware is connected.")

    def _read_one_line(self) -> bool:
        if self.port is None:
            return False
        try:
            if hasattr(self.port, "in_waiting") and not self.port.in_waiting:
                return False
            data = self.port.readline()
        except Exception as error:
            self._log("ERROR", f"Serial read failed: {error}")
            self._connection_lost()
            return False
        if not data:
            return False
        line = data.decode("utf-8", errors="replace").strip()
        if not line:
            return False
        self._last_line = line
        self._log("RX", line)
        if "Playback STARTED" in line or "BYTEBEAT:BB:OK" in line:
            self.playback_state = "active"
            self.playback_started_by_tool = True
            self._emit("playback", "active")
            self._emit("playback_owner", True)
        elif "Playback STOPPED" in line:
            self.playback_state = "idle"
            self._emit("playback", "idle")
            self.playback_started_by_tool = False
            self._emit("playback_owner", False)
        elif "Playback PAUSED" in line and self.playback_state != "starting":
            self.playback_state = "idle"
            self._emit("playback", "idle")
        return True

    def _read_available(self) -> None:
        while self.verified and self.port is not None:
            if not self._read_one_line():
                break

    def _disconnect(self, job_id: int | None = None, safe_stop: bool = False) -> None:
        if self.verified and safe_stop and self.playback_started_by_tool:
            try:
                _write_all(self.port, b"STOP\n")
                self._log("TX", "STOP (closing tool-started playback)")
                confirmed = self._wait_for_playback_message(
                    "STOPPED", STOP_CONFIRM_TIMEOUT_SECONDS
                )
                if not confirmed:
                    self._log(
                        "WARN",
                        "Closing without stop confirmation; device safety timeout remains active.",
                    )
            except Exception as error:
                self._log(
                    "ERROR",
                    f"Could not confirm stop before disconnecting: {error}. "
                    "The device safety timeout remains active.",
                )
        elif self.verified and self.playback_started_by_tool:
            self._log(
                "WARN",
                "Disconnecting while tool-started playback is active; "
                "the device safety timeout remains active.",
            )
        self._close_port()
        self._emit("connection", "disconnected", "")
        self._emit("playback", "unknown")
        self._emit("playback_owner", False)
        self._log("INFO", "Serial connection closed.")
        if job_id is not None:
            self._emit("job_done", job_id, "disconnect", True, "Disconnected.")

    def _close_port(self) -> None:
        self.verified = False
        port, self.port = self.port, None
        if port is not None:
            try:
                port.close()
            except Exception as error:
                self._log("ERROR", f"Could not close serial port cleanly: {error}")

    def _connection_lost(self) -> None:
        self._close_port()
        self._emit("connection", "disconnected", "")
        self._emit("playback", "unknown")
        self._emit("playback_owner", False)

    def run(self) -> None:
        while self.running:
            try:
                job = self.jobs.get(timeout=SERIAL_READ_TIMEOUT_SECONDS)
            except queue.Empty:
                self._read_available()
                continue
            if not job:
                continue
            action = job[0]
            if action == "connect":
                self._connect(job[1], job[2])
            elif action == "send":
                self._send(job[1], job[2])
            elif action == "stop":
                self._stop(job[1], job[2] if len(job) > 2 else "stop")
            elif action == "disconnect":
                self._disconnect(job[1], safe_stop=job[2])
            elif action == "shutdown":
                job_id, safe_stop = job[1], job[2]
                try:
                    self._disconnect(safe_stop=safe_stop)
                except Exception as error:
                    self._log("ERROR", f"Shutdown encountered a serial error: {error}")
                finally:
                    self._close_port()
                    self.running = False
                    self._emit("closed", job_id)
        self._close_port()


class ESP32Controller:
    """Synchronous serial controller for explicit CLI actions."""

    def __init__(self, port_name: str):
        self.port_name = port_name
        self.port = None
        self.playback_started = False

    def connect(self) -> None:
        if serial is None:
            raise ToolError(
                "pyserial is not installed. Install tools_py\\requirements.txt."
            )
        try:
            self.port = serial.Serial(
                self.port_name,
                BAUD_RATE,
                timeout=SERIAL_READ_TIMEOUT_SECONDS,
                write_timeout=2.0,
            )
            time.sleep(FIRMWARE_BOOT_DELAY_SECONDS)
            identity = verify_firmware_identity(self.port)
            if identity != FIRMWARE_ID:
                raise ToolError(
                    f"Firmware identity mismatch. Expected {FIRMWARE_ID}; "
                    f"received {identity or 'no identity'}."
                )
            print(f"Connected to verified firmware on {self.port_name}: {identity}")
        except Exception:
            self.disconnect()
            raise

    def send(
        self, formula: str, mode: int, sample_rate: int, volume: int, duration: int
    ) -> None:
        commands = build_play_commands(formula, mode, sample_rate, volume, duration)
        self._require_connection()
        self._validate_formula(formula)
        for command in commands:
            _write_all(self.port, f"{command}\n".encode("utf-8"))
            print(f"TX {command}")
        self.playback_started = True
        self._wait_response("Playback STARTED", START_CONFIRM_TIMEOUT_SECONDS)

    def _validate_formula(self, formula: str) -> None:
        _write_all(self.port, f"VLD:{formula}\n".encode("utf-8"))
        print(f"TX VLD:{formula}")
        deadline = time.monotonic() + FORMULA_VALIDATION_TIMEOUT_SECONDS
        while time.monotonic() < deadline:
            line = self.port.readline()
            if not line:
                continue
            text = line.decode("utf-8", errors="replace").strip()
            print(f"RX {text}")
            if text == "BYTEBEAT:FORMULA:VALID":
                return
            if text == "BYTEBEAT:FORMULA:INVALID":
                raise ToolError(
                    "Firmware rejected this formula. The device compiler "
                    "supports a smaller expression syntax than some catalog "
                    "entries; no playback command was sent."
                )
        raise TimeoutError(
            "Firmware did not confirm formula validation. No playback command "
            "was sent; verify the Bytebeat firmware version."
        )

    def stop(self) -> None:
        self._require_connection()
        _write_all(self.port, b"STOP\n")
        print("TX STOP")
        self._wait_response("Playback STOPPED", STOP_CONFIRM_TIMEOUT_SECONDS)
        self.playback_started = False

    def _wait_response(self, expected: str, timeout: float) -> None:
        deadline = time.monotonic() + timeout
        while time.monotonic() < deadline:
            line = self.port.readline()
            if not line:
                continue
            text = line.decode("utf-8", errors="replace").strip()
            print(f"RX {text}")
            if expected in text:
                return
        message = f"Firmware did not confirm {expected}."
        if expected == "Playback STARTED":
            message += (
                " The formula may still be running; send STOP or rely on the "
                "device safety timeout."
            )
        raise TimeoutError(message)

    def _require_connection(self) -> None:
        if self.port is None or not getattr(self.port, "is_open", True):
            raise ToolError("No verified serial connection.")

    def disconnect(self) -> None:
        if self.port is not None:
            try:
                self.port.close()
            finally:
                self.port = None


def _formula_mode_name(mode: Any) -> str:
    try:
        return MODES[int(mode)]
    except (KeyError, TypeError, ValueError):
        return "Unknown"


def _format_formula_line(index: int, formula: dict[str, Any]) -> str:
    name = str(formula.get("name", "Unnamed"))[:42]
    category = str(formula.get("category", "Unknown"))[:22]
    return (
        f"{index:5}  {category:22}  {name:42}  "
        f"{_formula_mode_name(formula.get('mode', 0))}"
    )


def run_gui() -> int:
    try:
        from PySide6.QtCore import QEvent, Qt, QTimer
        from PySide6.QtGui import (
            QAction,
            QColor,
            QFontDatabase,
            QTextCharFormat,
            QTextCursor,
        )
        from PySide6.QtWidgets import (
            QApplication,
            QComboBox,
            QFileDialog,
            QHBoxLayout,
            QLabel,
            QLineEdit,
            QMainWindow,
            QMessageBox,
            QPlainTextEdit,
            QPushButton,
            QSpinBox,
            QSplitter,
            QStatusBar,
            QTreeWidget,
            QTreeWidgetItem,
            QVBoxLayout,
            QWidget,
            QWidgetAction,
        )
    except ImportError as error:
        print(f"PySide6 is unavailable: {error}", file=sys.stderr)
        return 2

    class BytebeatWindow(QMainWindow):
        def __init__(self):
            super().__init__()
            self.setWindowTitle("Bytebeat")
            self.resize(1320, 860)
            self.setMinimumSize(900, 600)
            self.events = queue.Queue()
            self.jobs = queue.Queue()
            self.worker = SerialWorker(self.events, self.jobs)
            self.worker.start()
            self.library = None
            self.library_error = None
            try:
                self.library = FormulaLibrary()
            except ToolError as error:
                self.library_error = str(error)
            self.connection_state = "disconnected"
            self.playback_state = "unknown"
            self.playback_started_by_tool = False
            self.playback_started_at = None
            self.playback_start_deadline = None
            self.hold_active = False
            self.busy_action = None
            self.job_counter = 0
            self.closing = False
            self.allow_close = False
            self.volume = 80
            self.duration = DEFAULT_SAFETY_SECONDS
            self._open_log()
            self._build_ui()
            self._refresh_ports()
            self._log(
                "INFO", "Console ready. Device connection and playback are manual."
            )
            if self.library_error:
                self._log("ERROR", self.library_error)
                self.statusBar().showMessage(
                    "Formula library unavailable; editor remains available.", 10000
                )
            else:
                self._populate_categories()
                self._filter_library()
                self._log(
                    "INFO", f"Loaded {len(self.library.formulas):,} catalog formulas."
                )
            if serial is None:
                self._log(
                    "ERROR",
                    "pyserial is missing. Run start_bytebeat_tool.bat to repair setup.",
                )
            if self.startup_log_error:
                self._log("INFO" if self.log_file else "ERROR", self.startup_log_error)
            self.event_timer = QTimer(self)
            self.event_timer.timeout.connect(self._poll_events)
            self.event_timer.start(50)
            self.search_timer = QTimer(self)
            self.search_timer.setSingleShot(True)
            self.search_timer.setInterval(140)
            self.search_timer.timeout.connect(self._filter_library)
            self.clock_timer = QTimer(self)
            self.clock_timer.setInterval(1000)
            self.clock_timer.timeout.connect(self._update_play_clock)
            self.clock_timer.start()
            QApplication.instance().applicationStateChanged.connect(
                self._application_state_changed
            )
            self.statusBar().showMessage("Ready. Connection and playback are manual.")
            self._sync_controls()

        def _open_log(self):
            try:
                directory = REPOSITORY_ROOT / ".logs"
                directory.mkdir(parents=True, exist_ok=True)
                stamp = datetime.now().strftime("%Y-%m-%d_%H-%M-%S")
                path = directory / f"{stamp}_bytebeat-tool.log"
                suffix = 1
                while path.exists():
                    path = directory / f"{stamp}_{suffix:02d}_bytebeat-tool.log"
                    suffix += 1
                self.log_file = path.open("x", encoding="utf-8", buffering=1)
            except OSError as error:
                self.log_file = None
                self.startup_log_error = f"Session log could not be created: {error}"
            else:
                self.startup_log_error = f"Session log: {path}"

        def _build_ui(self):
            self._build_menus()
            central = QWidget(self)
            root = QVBoxLayout(central)
            root.setContentsMargins(10, 8, 10, 8)
            root.setSpacing(6)
            root.addWidget(self._build_toolbar())

            self.formula_tree = QTreeWidget()
            self.formula_tree.setHeaderLabels(("Formula", "Category", "Mode"))
            self.formula_tree.setRootIsDecorated(False)
            self.formula_tree.setAlternatingRowColors(True)
            self.formula_tree.setUniformRowHeights(True)
            self.formula_tree.itemDoubleClicked.connect(self.load_selected)
            self.editor = QPlainTextEdit()
            self.editor.setFont(
                QFontDatabase.systemFont(QFontDatabase.SystemFont.FixedFont)
            )
            self.editor.setLineWrapMode(QPlainTextEdit.LineWrapMode.NoWrap)
            self.editor.setPlainText("t*(42&t>>10)")
            self.editor.setPlaceholderText(
                "Enter one device-supported Bytebeat formula."
            )

            library = QWidget()
            library_layout = QVBoxLayout(library)
            library_layout.setContentsMargins(0, 0, 6, 0)
            search_row = QHBoxLayout()
            self.search_entry = QLineEdit()
            self.search_entry.setPlaceholderText("Search formulas...")
            self.search_entry.textChanged.connect(self.search_timer_start)
            self.category_combo = QComboBox()
            self.category_combo.currentTextChanged.connect(self._filter_library)
            search_row.addWidget(self.search_entry, 1)
            search_row.addWidget(self.category_combo)
            library_layout.addLayout(search_row)
            library_layout.addWidget(self.formula_tree, 1)
            self.catalog_count = QLabel("Formula catalog")
            library_layout.addWidget(self.catalog_count)

            editor = QWidget()
            editor_layout = QVBoxLayout(editor)
            editor_layout.setContentsMargins(6, 0, 0, 0)
            editor_header = QHBoxLayout()
            editor_header.addWidget(QLabel("Formula editor"))
            editor_header.addStretch(1)
            self.validate_button = QPushButton("Validate")
            self.validate_button.clicked.connect(self.validate_current)
            self.convert_button = QPushButton("Convert JS Math")
            self.convert_button.clicked.connect(self.convert_current)
            editor_header.addWidget(self.validate_button)
            editor_header.addWidget(self.convert_button)
            editor_layout.addLayout(editor_header)
            editor_layout.addWidget(self.editor, 1)
            editor_layout.addWidget(
                QLabel(
                    "Sending compiles the formula on the device and starts playback."
                )
            )

            work = QSplitter(Qt.Orientation.Horizontal)
            work.setChildrenCollapsible(False)
            work.addWidget(library)
            work.addWidget(editor)
            work.setStretchFactor(0, 3)
            work.setStretchFactor(1, 7)
            work.setSizes([390, 870])

            self.log_view = QPlainTextEdit()
            self.log_view.setReadOnly(True)
            self.log_view.setFont(
                QFontDatabase.systemFont(QFontDatabase.SystemFont.FixedFont)
            )
            self.log_view.setMinimumHeight(140)
            self.log_view.setMaximumBlockCount(10000)
            log_header = QHBoxLayout()
            log_header.addWidget(QLabel("Serial console"))
            log_header.addStretch(1)
            clear_button = QPushButton("Clear")
            clear_button.clicked.connect(self.log_view.clear)
            log_header.addWidget(clear_button)
            self.log_panel = QWidget()
            log_layout = QVBoxLayout(self.log_panel)
            log_layout.setContentsMargins(0, 0, 0, 0)
            log_layout.addLayout(log_header)
            log_layout.addWidget(self.log_view, 1)
            self.log_panel.setMinimumHeight(180)

            self.main_splitter = QSplitter(Qt.Orientation.Vertical)
            self.main_splitter.setChildrenCollapsible(False)
            self.main_splitter.addWidget(work)
            self.main_splitter.addWidget(self.log_panel)
            self.main_splitter.setStretchFactor(0, 7)
            self.main_splitter.setStretchFactor(1, 3)
            self.main_splitter.setSizes([590, 240])
            root.addWidget(self.main_splitter, 1)
            self.setCentralWidget(central)
            self.show_log_action.toggled.connect(self.log_panel.setVisible)
            status = QStatusBar(self)
            self.setStatusBar(status)
            self.connection_status = QLabel("Disconnected")
            self.playback_status = QLabel("Playback: unknown")
            status.addPermanentWidget(self.connection_status)
            status.addPermanentWidget(self.playback_status)

        def _build_toolbar(self):
            row = QWidget()
            layout = QHBoxLayout(row)
            layout.setContentsMargins(0, 0, 0, 0)
            layout.setSpacing(6)
            self.play_button = QPushButton("Play")
            self.play_button.setToolTip("Play until Stop or the safety timeout.")
            self.play_button.clicked.connect(self.send_formula)
            self.hold_button = QPushButton("Hold")
            self.hold_button.setToolTip("Play only while held; releasing sends STOP.")
            self.hold_button.pressed.connect(self._hold_press)
            self.hold_button.released.connect(self._release_hold)
            self.stop_button = QPushButton("Stop")
            self.stop_button.clicked.connect(self.stop_playback)
            self.stop_button.setToolTip("Stop device playback.")
            layout.addWidget(self.play_button)
            layout.addWidget(self.hold_button)
            layout.addWidget(self.stop_button)
            self.mode_combo = QComboBox()
            self.mode_combo.addItems(MODES.values())
            self.mode_combo.setCurrentText(MODES[0])
            self.mode_combo.setToolTip("Bytebeat formula mode")
            self.rate_combo = QComboBox()
            self.rate_combo.addItems(SAMPLE_RATE_LABELS.values())
            self.rate_combo.setCurrentText(SAMPLE_RATE_LABELS[8000])
            self.rate_combo.setToolTip("Formula sample rate")
            layout.addWidget(self.mode_combo)
            layout.addWidget(self.rate_combo)
            layout.addStretch(1)
            self.port_combo = QComboBox()
            self.port_combo.setMinimumWidth(120)
            self.port_combo.setMaximumWidth(220)
            self.port_combo.setToolTip("Serial port for the development firmware")
            self.refresh_button = QPushButton("Refresh")
            self.refresh_button.clicked.connect(self._refresh_ports)
            self.refresh_button.setToolTip("Rescan serial ports")
            self.connect_button = QPushButton("Connect")
            self.connect_button.clicked.connect(self.toggle_connection)
            layout.addWidget(self.port_combo)
            layout.addWidget(self.refresh_button)
            layout.addWidget(self.connect_button)
            return row

        def _build_menus(self):
            file_menu = self.menuBar().addMenu("&File")
            self._action(file_menu, "Open formula...", self.open_formula)
            self._action(file_menu, "Save formula...", self.save_formula)
            file_menu.addSeparator()
            self._action(file_menu, "Quit", self.close)
            tools = self.menuBar().addMenu("&Tools")
            self._action(tools, "Validate formula", self.validate_current)
            self._action(tools, "Convert JS Math syntax", self.convert_current)
            settings = self.menuBar().addMenu("&Settings")
            settings.addAction(
                self._menu_spinbox(
                    settings,
                    "Speaker volume (%)",
                    0,
                    100,
                    self.volume,
                    self._set_volume,
                    "Output volume sent with the next Play command.",
                )
            )
            settings.addAction(
                self._menu_spinbox(
                    settings,
                    "Safety timeout (s)",
                    0,
                    3600,
                    self.duration,
                    self._set_duration,
                    "Maximum playback duration. 0 disables the device safety timeout.",
                )
            )
            settings.addSeparator()
            view_menu = settings.addMenu("&View")
            self.show_log_action = view_menu.addAction("Show Serial Log")
            self.show_log_action.setCheckable(True)
            self.show_log_action.setChecked(True)
            help_menu = self.menuBar().addMenu("&Help")
            self._action(help_menu, "Help", self.show_help)
            self._action(help_menu, "About", self.show_about)

        @staticmethod
        def _action(menu, title, callback):
            action = QAction(title, menu)
            action.triggered.connect(callback)
            menu.addAction(action)

        @staticmethod
        def _menu_spinbox(menu, label, minimum, maximum, initial, changed, tooltip):
            row = QWidget()
            layout = QHBoxLayout(row)
            layout.setContentsMargins(12, 4, 12, 4)
            text = QLabel(label)
            spin = QSpinBox(row)
            spin.setRange(minimum, maximum)
            spin.setValue(initial)
            spin.setFixedWidth(90)
            row.setToolTip(tooltip)
            text.setToolTip(tooltip)
            spin.setToolTip(tooltip)
            spin.valueChanged.connect(changed)
            layout.addWidget(text)
            layout.addStretch(1)
            layout.addWidget(spin)
            action = QWidgetAction(menu)
            action.setDefaultWidget(row)
            return action

        def _set_volume(self, value):
            self.volume = value
            self.statusBar().showMessage(
                f"Speaker volume set to {value}% for the next Play.", 4000
            )

        def _set_duration(self, value):
            self.duration = value
            message = (
                "Safety timeout disabled for the next Play."
                if value == 0
                else f"Safety timeout set to {value}s for the next Play."
            )
            self.statusBar().showMessage(message, 4000)

        def _populate_categories(self):
            self.category_combo.blockSignals(True)
            self.category_combo.addItems(["All", *self.library.categories()])
            self.category_combo.blockSignals(False)

        def search_timer_start(self):
            self.search_timer.start()

        def _filter_library(self, *_args):
            if self.library is None:
                return
            matches = self.library.search(
                self.search_entry.text(), self.category_combo.currentText() or "All"
            )
            self.formula_tree.setUpdatesEnabled(False)
            self.formula_tree.clear()
            for index, formula in matches:
                item = QTreeWidgetItem(
                    [
                        str(formula.get("name", "Unnamed")),
                        str(formula.get("category", "Unknown")),
                        _formula_mode_name(formula.get("mode", 0)),
                    ]
                )
                item.setData(0, Qt.ItemDataRole.UserRole, index)
                self.formula_tree.addTopLevelItem(item)
            self.formula_tree.setUpdatesEnabled(True)
            self.catalog_count.setText(
                f"{len(matches):,} of {len(self.library.formulas):,} formulas"
            )

        def load_selected(self, item=None, _column=0):
            item = item or self.formula_tree.currentItem()
            if item is None or self.library is None:
                return
            try:
                index = int(item.data(0, Qt.ItemDataRole.UserRole))
                formula = self.library.get(index)
            except (TypeError, ValueError, ToolError) as error:
                self._log("ERROR", str(error))
                return
            self.editor.setPlainText(str(formula.get("code", "")))
            mode = formula.get("mode", 0)
            if mode in MODES:
                self.mode_combo.setCurrentText(MODES[mode])
            sample_rate = formula.get("sample_rate")
            if sample_rate is None:
                sample_rate = float(formula.get("speed", 1.0)) * DAC_SAMPLE_RATE
            try:
                rate = int(round(float(sample_rate)))
                if rate in SAMPLE_RATES:
                    self.rate_combo.setCurrentText(SAMPLE_RATE_LABELS[rate])
            except (TypeError, ValueError, OverflowError):
                self._log(
                    "WARN", "Invalid catalog sample rate; retaining current setting."
                )
            self._log(
                "INFO",
                f"Loaded catalog #{index}: {formula.get('name', 'Unnamed')}. Review before sending.",
            )
            self.statusBar().showMessage(f"Loaded formula #{index}", 4000)

        def _current_settings(self):
            mode = next(
                (
                    key
                    for key, name in MODES.items()
                    if name == self.mode_combo.currentText()
                ),
                -1,
            )
            rate = SAMPLE_RATE_VALUES.get(self.rate_combo.currentText(), -1)
            formula = self.editor.toPlainText().strip()
            validate_device_settings(formula, mode, rate, self.volume, self.duration)
            return {
                "formula": formula,
                "mode": mode,
                "sample_rate": rate,
                "volume": self.volume,
                "duration": self.duration,
            }

        def send_formula(self, hold=False):
            if self.connection_state != "connected":
                QMessageBox.warning(
                    self, "Not connected", "Verify the firmware connection first."
                )
                return
            try:
                payload = self._current_settings()
            except ValueError as error:
                QMessageBox.critical(self, "Invalid playback settings", str(error))
                return
            if payload["duration"] == 0:
                answer = QMessageBox.question(
                    self,
                    "Unlimited playback",
                    "A safety timeout of 0 disables automatic device stop. Play anyway?",
                    QMessageBox.StandardButton.Yes | QMessageBox.StandardButton.No,
                    QMessageBox.StandardButton.No,
                )
                if answer != QMessageBox.StandardButton.Yes:
                    return
            if hold:
                self.hold_active = True
            if not self._submit_job("send", payload):
                self.hold_active = False if hold else self.hold_active
            elif hold:
                self.statusBar().showMessage("Hold playback; release to stop.")

        def _hold_press(self):
            if not self.hold_active:
                self.send_formula(hold=True)

        def _release_hold(self):
            if not self.hold_active:
                return
            self.hold_active = False
            if self.connection_state != "connected":
                return
            self.job_counter += 1
            self.busy_action = "hold-stop"
            self.playback_start_deadline = None
            self.jobs.put(("stop", self.job_counter, "hold-stop"))
            self._sync_controls()

        def stop_playback(self):
            if self.connection_state != "connected":
                self.statusBar().showMessage(
                    "Connect to verified firmware before STOP.", 5000
                )
                return
            self._submit_job("stop")

        def _submit_job(self, action, payload=None):
            if self.busy_action:
                return False
            self.job_counter += 1
            job_id = self.job_counter
            self.busy_action = action
            if action == "send":
                self.playback_started_at = None
                self.playback_state = "starting"
                self.playback_start_deadline = (
                    time.monotonic() + START_CONFIRM_TIMEOUT_SECONDS
                )
                self.playback_status.setText("Playback: starting")
                self.jobs.put(("send", job_id, payload))
            elif action == "stop":
                self.playback_start_deadline = None
                self.jobs.put(("stop", job_id))
            elif action == "disconnect":
                self.jobs.put(
                    ("disconnect", job_id, bool(payload and payload.get("safe_stop")))
                )
            self._sync_controls()
            return True

        def toggle_connection(self):
            if self.connection_state == "connected":
                safe_stop = False
                if self.playback_started_by_tool or self.playback_state in {
                    "starting",
                    "active",
                }:
                    answer = QMessageBox.question(
                        self,
                        "Playback is active",
                        "This tool started playback. Stop before disconnecting?",
                        QMessageBox.StandardButton.Yes | QMessageBox.StandardButton.No,
                        QMessageBox.StandardButton.Yes,
                    )
                    safe_stop = answer == QMessageBox.StandardButton.Yes
                self._submit_job("disconnect", {"safe_stop": safe_stop})
                return
            if self.connection_state != "disconnected":
                return
            port = self.port_combo.currentText().strip()
            if not port:
                QMessageBox.warning(
                    self, "No serial port", "Select an available COM port."
                )
                return
            self.job_counter += 1
            self.busy_action = "connect"
            self.connection_state = "connecting"
            self._show_connection("connecting", port)
            self.jobs.put(("connect", self.job_counter, port))
            self._sync_controls()

        def _refresh_ports(self):
            if list_ports is None:
                self._log("ERROR", "pyserial is not installed; cannot scan ports.")
                return
            selected = self.port_combo.currentText()
            try:
                ports = sorted(port.device for port in list_ports.comports())
            except Exception as error:
                self._log("ERROR", f"Serial port scan failed: {error}")
                self.statusBar().showMessage("Port scan failed; see session log.", 5000)
                return
            self.port_combo.clear()
            self.port_combo.addItems(ports)
            if selected in ports:
                self.port_combo.setCurrentText(selected)
            self.statusBar().showMessage(
                f"Available serial ports: {', '.join(ports)}"
                if ports
                else "No serial ports detected.",
                5000,
            )
            self._log(
                "INFO",
                f"Available serial ports: {', '.join(ports) if ports else 'none'}",
            )

        def open_formula(self):
            filename, _ = QFileDialog.getOpenFileName(
                self,
                "Open formula",
                "",
                "Formula text (*.txt *.bb *.js);;All files (*)",
            )
            if not filename:
                return
            try:
                self.editor.setPlainText(
                    Path(filename).read_text(encoding="utf-8").rstrip("\r\n")
                )
            except (OSError, UnicodeError) as error:
                QMessageBox.critical(self, "Could not open formula", str(error))

        def save_formula(self):
            filename, _ = QFileDialog.getSaveFileName(
                self,
                "Save formula",
                "formula.txt",
                "Formula text (*.txt);;All files (*)",
            )
            if not filename:
                return
            try:
                Path(filename).write_text(
                    self.editor.toPlainText() + "\n", encoding="utf-8"
                )
            except (OSError, UnicodeError) as error:
                QMessageBox.critical(self, "Could not save formula", str(error))

        def validate_current(self):
            errors = validate_formula(self.editor.toPlainText().strip())
            if errors:
                QMessageBox.warning(
                    self, "Formula validation failed", "\n".join(errors)
                )
                self._log("WARN", "; ".join(errors))
            else:
                self.statusBar().showMessage(
                    "Basic formula checks passed; firmware syntax is not checked.", 7000
                )
                self._log(
                    "INFO",
                    "Basic formula and transport checks passed; firmware syntax is not checked.",
                )

        def convert_current(self):
            original = self.editor.toPlainText()
            converted, warnings = convert_js_math(original)
            if converted == original:
                self.statusBar().showMessage("No formula changes were needed.", 4000)
                return
            self.editor.setPlainText(converted)
            for warning in warnings:
                self._log("WARN", warning)
            self.statusBar().showMessage(
                "Converted JavaScript Math names; review before sending.", 6000
            )

        def show_help(self):
            QMessageBox.information(
                self,
                "Bytebeat Help",
                "Choose a catalog formula or edit one. Play starts device playback; Hold sends STOP on release.\\n\\n"
                "Mode and sample rate are in the toolbar. Speaker volume and safety timeout can be adjusted directly under Settings. "
                "Connecting never starts playback. Formula compilation happens on-device.",
            )

        def show_about(self):
            QMessageBox.about(
                self,
                "About Bytebeat",
                f"Bytebeat {VERSION}\\nVerified serial formula console.",
            )

        def _log(self, level, message):
            line = f"[{datetime.now().strftime('%H:%M:%S')}] {level:<5} {message}"
            colors = {
                "INFO": "#d8e3ec",
                "OK": "#78dba9",
                "WARN": "#ffd166",
                "ERROR": "#ff7777",
                "TX": "#76c7ff",
                "RX": "#9be7ae",
            }
            cursor = self.log_view.textCursor()
            cursor.movePosition(QTextCursor.MoveOperation.End)
            formatting = QTextCharFormat()
            formatting.setForeground(QColor(colors.get(level, "#d8e3ec")))
            cursor.insertText(line + "\n", formatting)
            self.log_view.setTextCursor(cursor)
            self.log_view.ensureCursorVisible()
            if self.log_file:
                try:
                    self.log_file.write(line + "\n")
                except OSError as error:
                    self.log_file = None
                    self.statusBar().showMessage(
                        f"Session logging failed: {error}", 10000
                    )

        def _poll_events(self):
            while True:
                try:
                    event = self.events.get_nowait()
                except queue.Empty:
                    break
                kind = event[0]
                if kind == "log":
                    self._log(event[1], event[2])
                elif kind == "connection":
                    self.connection_state = event[1]
                    self._show_connection(event[1], event[2])
                elif kind == "playback":
                    self.playback_state = event[1]
                    self.playback_status.setText(f"Playback: {event[1]}")
                    if event[1] in {"active", "idle", "unknown"}:
                        self.playback_start_deadline = None
                    if event[1] == "active":
                        self.playback_started_at = (
                            self.playback_started_at or time.monotonic()
                        )
                        self.statusBar().showMessage("Playback active.")
                    elif event[1] in {"idle", "unknown"}:
                        self.playback_started_at = None
                        self.play_button.setText("Play")
                elif kind == "playback_owner":
                    self.playback_started_by_tool = event[1]
                elif kind == "job_done":
                    _job_id, action, success, message = event[1:]
                    if action == self.busy_action:
                        self.busy_action = None
                    self.statusBar().showMessage(
                        message, 10000 if not success else 5000
                    )
                    if not success:
                        self._log("ERROR", message)
                    elif action == "send":
                        self._log("OK", message)
                    self._sync_controls()
                elif kind == "closed":
                    self.allow_close = True
                    self.close()
                    return
            if (
                self.playback_state == "starting"
                and self.playback_start_deadline
                and time.monotonic() >= self.playback_start_deadline
            ):
                self.playback_state = "unknown"
                self.playback_start_deadline = None
                self.playback_status.setText("Playback: unknown")
                message = "No playback confirmation arrived; playback may still be running. Use Stop or rely on the device timeout."
                self.statusBar().showMessage(message, 10000)
                self._log("WARN", message)
            self._update_play_clock()

        def _update_play_clock(self):
            if self.playback_started_at is None or self.playback_state != "active":
                return
            elapsed = max(0, int(time.monotonic() - self.playback_started_at))
            hours, remainder = divmod(elapsed, 3600)
            minutes, seconds = divmod(remainder, 60)
            self.play_button.setText(f"{hours:02d}:{minutes:02d}:{seconds:02d}")

        def _show_connection(self, state, port):
            labels = {
                "disconnected": "Disconnected",
                "connecting": f"Connecting · {port}",
                "verifying": f"Verifying firmware · {port}",
                "connected": f"Connected · {port}",
            }
            text = labels.get(state, state.title())
            self.connection_status.setText(text)
            self.statusBar().showMessage(text, 5000)
            if state == "disconnected":
                self.playback_started_at = None
                self.play_button.setText("Play")
            self._sync_controls()

        def _sync_controls(self):
            connected = self.connection_state == "connected"
            busy = self.busy_action is not None or self.closing
            may_play = (
                self.playback_state in {"starting", "active"}
                or self.playback_started_by_tool
            )
            self.connect_button.setText("Disconnect" if connected else "Connect")
            self.connect_button.setEnabled(
                not busy and (connected or self.connection_state == "disconnected")
            )
            self.port_combo.setEnabled(
                self.connection_state == "disconnected" and not busy
            )
            self.refresh_button.setEnabled(
                self.connection_state == "disconnected" and not busy
            )
            self.play_button.setEnabled(connected and not busy and not may_play)
            self.hold_button.setEnabled(
                connected and (self.hold_active or (not busy and not may_play))
            )
            self.stop_button.setEnabled(connected and not busy)
            self.mode_combo.setEnabled(not busy)
            self.rate_combo.setEnabled(not busy)
            self.validate_button.setEnabled(not busy)
            self.convert_button.setEnabled(not busy)

        def _application_state_changed(self, state):
            if state != Qt.ApplicationState.ApplicationActive:
                self._release_hold()

        def changeEvent(self, event):
            if event.type() == QEvent.Type.WindowDeactivate:
                self._release_hold()
            super().changeEvent(event)

        def closeEvent(self, event):
            if self.allow_close:
                if self.log_file:
                    try:
                        self.log_file.write(
                            f"[{datetime.now():%H:%M:%S}] INFO  Console closed.\n"
                        )
                        self.log_file.close()
                    except OSError:
                        pass
                    self.log_file = None
                event.accept()
                return
            if self.closing:
                event.ignore()
                return
            safe_stop = False
            if self.connection_state == "connected" and (
                self.playback_started_by_tool
                or self.playback_state in {"starting", "active"}
            ):
                answer = QMessageBox.question(
                    self,
                    "Playback is active",
                    "This tool started playback. Stop before closing?",
                    QMessageBox.StandardButton.Yes
                    | QMessageBox.StandardButton.No
                    | QMessageBox.StandardButton.Cancel,
                    QMessageBox.StandardButton.Yes,
                )
                if answer == QMessageBox.StandardButton.Cancel:
                    event.ignore()
                    return
                safe_stop = answer == QMessageBox.StandardButton.Yes
            self.closing = True
            self.busy_action = "shutdown"
            self._sync_controls()
            self.job_counter += 1
            self.jobs.put(("shutdown", self.job_counter, safe_stop))
            event.ignore()

    app = QApplication.instance() or QApplication(sys.argv[:1])
    window = BytebeatWindow()
    window.show()
    return app.exec()


def convert_js_math(formula: str) -> tuple[str, list[str]]:
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
    converted = formula
    for old, new in replacements.items():
        converted = converted.replace(old, new)
    warnings = []
    if converted != formula:
        warnings.append(
            "Converted JavaScript Math.* names; confirm the target supports each function."
        )
    if "?" in converted and ":" in converted:
        warnings.append(
            "Ternary expressions may not be supported by this firmware compiler."
        )
    return converted, warnings


def _add_port_argument(parser):
    parser.add_argument("--port", required=True, help="Serial port (for example COM4)")


def _emit_json(value) -> None:
    print(json.dumps(value, ensure_ascii=True, separators=(",", ":")))


def _catalog_record(
    index: int, formula: dict[str, Any], include_code: bool
) -> dict[str, Any]:
    record = {
        "index": index,
        "name": formula.get("name", "Unnamed"),
        "category": formula.get("category", "Unknown"),
        "author": formula.get("author", "Unknown"),
        "mode": formula.get("mode", 0),
        "sample_rate": formula.get("sample_rate"),
        "speed": formula.get("speed"),
    }
    if include_code:
        record["code"] = formula.get("code", "")
    return record


def _cli_send_payload(port_name: str, payload: dict[str, Any]) -> int:
    allow_infinite = payload.pop("allow_infinite", False)
    if payload["duration"] == 0 and not allow_infinite:
        raise ToolError(
            "Duration 0 disables the safety timeout; pass --allow-infinite explicitly."
        )
    controller = ESP32Controller(port_name)
    try:
        controller.connect()
        controller.send(**payload)
        print("Firmware reported playback started.")
        return 0
    finally:
        controller.disconnect()


def _cli_send(args) -> int:
    return _cli_send_payload(
        args.port,
        {
            "formula": args.formula,
            "mode": args.mode,
            "sample_rate": args.sample_rate,
            "volume": args.volume,
            "duration": args.duration,
            "allow_infinite": args.allow_infinite,
        },
    )


def _cli_play(args) -> int:
    formula = FormulaLibrary().get(args.index)
    mode = formula.get("mode", 0) if args.mode is None else args.mode
    if mode not in MODES:
        raise ToolError(
            "This catalog entry is marked as PC-only or uses an unsupported device mode."
        )
    sample_rate = formula.get("sample_rate")
    if sample_rate is None:
        sample_rate = float(formula.get("speed", 1.0)) * DAC_SAMPLE_RATE
    if args.sample_rate is not None:
        sample_rate = args.sample_rate
    return _cli_send_payload(
        args.port,
        {
            "formula": formula.get("code", ""),
            "mode": mode,
            "sample_rate": int(round(float(sample_rate))),
            "volume": args.volume,
            "duration": args.duration,
            "allow_infinite": args.allow_infinite,
        },
    )


def _cli_stop(args) -> int:
    controller = ESP32Controller(args.port)
    try:
        controller.connect()
        controller.stop()
        print("Firmware confirmed playback stopped.")
        return 0
    finally:
        controller.disconnect()


def build_parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(
        description="ESP Bytebeat formula workbench and verified device console.",
        epilog="Without a command, opens the GUI. No command connects or plays automatically.",
    )
    parser.add_argument("--version", action="version", version=f"%(prog)s {VERSION}")
    commands = parser.add_subparsers(dest="command")
    commands.add_parser("gui", help="Open the desktop GUI")
    ports_parser = commands.add_parser("ports", help="List detected serial ports")
    ports_parser.add_argument("--json", action="store_true", dest="as_json")
    list_parser = commands.add_parser("list", help="List formula catalog entries")
    list_parser.add_argument("--category")
    list_parser.add_argument("--json", action="store_true", dest="as_json")
    list_parser.add_argument("--include-code", action="store_true")
    list_parser.add_argument("--limit", type=int, default=100)
    list_parser.add_argument("--offset", type=int, default=0)
    list_parser.add_argument("--all", action="store_true")
    categories_parser = commands.add_parser(
        "categories", help="List catalog categories"
    )
    categories_parser.add_argument("--json", action="store_true", dest="as_json")
    search_parser = commands.add_parser(
        "search", help="Search formula names, authors, categories, and code"
    )
    search_parser.add_argument("query")
    search_parser.add_argument("--json", action="store_true", dest="as_json")
    search_parser.add_argument("--include-code", action="store_true")
    search_parser.add_argument("--limit", type=int, default=100)
    search_parser.add_argument("--offset", type=int, default=0)
    search_parser.add_argument("--all", action="store_true")
    validate_parser = commands.add_parser(
        "validate", help="Check formula transport limits and basic syntax"
    )
    validate_parser.add_argument("formula")
    send_parser = commands.add_parser(
        "send", help="Verify firmware, then explicitly send and play a formula"
    )
    send_parser.add_argument("formula")
    _add_port_argument(send_parser)
    send_parser.add_argument("--mode", type=int, choices=tuple(MODES), default=0)
    send_parser.add_argument(
        "--sample-rate", type=int, choices=SAMPLE_RATES, default=8000
    )
    send_parser.add_argument("--volume", type=int, choices=range(101), default=80)
    send_parser.add_argument(
        "--duration",
        type=int,
        default=DEFAULT_SAFETY_SECONDS,
        help="Safety timeout in seconds (0 requires --allow-infinite)",
    )
    send_parser.add_argument("--allow-infinite", action="store_true")
    play_parser = commands.add_parser("play", help="Play a catalog formula by index")
    play_parser.add_argument("index", type=int)
    _add_port_argument(play_parser)
    play_parser.add_argument("--mode", type=int, choices=tuple(MODES))
    play_parser.add_argument("--sample-rate", type=int, choices=SAMPLE_RATES)
    play_parser.add_argument("--volume", type=int, choices=range(101), default=80)
    play_parser.add_argument("--duration", type=int, default=DEFAULT_SAFETY_SECONDS)
    play_parser.add_argument("--allow-infinite", action="store_true")
    stop_parser = commands.add_parser(
        "stop", help="Verify firmware, then explicitly stop playback"
    )
    _add_port_argument(stop_parser)
    return parser


def _run_cli(args) -> int:
    if args.command in (None, "gui"):
        return run_gui()
    if args.command == "ports":
        if list_ports is None:
            raise ToolError("pyserial is not installed.")
        ports = sorted(port.device for port in list_ports.comports())
        if args.as_json:
            _emit_json(ports)
        else:
            print("\n".join(ports))
        return 0

    if args.command in ("list", "categories", "search"):
        library = FormulaLibrary()
        if args.command == "categories":
            categories = [
                {"name": name, "count": len(library.search(category=name))}
                for name in library.categories()
            ]
            if args.as_json:
                _emit_json(categories)
            else:
                for category in categories:
                    print(f"{category['name']}\t{category['count']}")
            return 0

        category = getattr(args, "category", None)
        if args.command == "search":
            matches = library.search(args.query)
        else:
            matches = library.search(category=category or "All")
        if args.limit < 1 or args.offset < 0:
            raise ToolError(
                "Catalog limit must be positive and offset cannot be negative."
            )
        total_matches = len(matches)
        matches = matches[args.offset :]
        if not args.all:
            matches = matches[: args.limit]
        if args.as_json:
            _emit_json(
                [
                    _catalog_record(index, formula, args.include_code)
                    for index, formula in matches
                ]
            )
        else:
            for index, formula in matches:
                print(_format_formula_line(index, formula))
            print(
                f"Showing {len(matches)} of {total_matches} matches "
                f"(offset {args.offset})."
            )
        return 0

    if args.command == "validate":
        errors = validate_formula(args.formula)
        if errors:
            for error in errors:
                print(f"error: {error}", file=sys.stderr)
            return 2
        print("Basic formula and serial transport checks passed.")
        print("Firmware syntax and compilation are not checked on the host.")
        return 0
    if args.command == "send":
        return _cli_send(args)
    if args.command == "play":
        return _cli_play(args)
    if args.command == "stop":
        return _cli_stop(args)
    raise ToolError(f"Unsupported command: {args.command}")


def main(argv: list[str] | None = None) -> int:
    for stream in (sys.stdout, sys.stderr):
        if hasattr(stream, "reconfigure"):
            stream.reconfigure(errors="backslashreplace")
    parser = build_parser()
    args = parser.parse_args(argv)
    try:
        return _run_cli(args)
    except (ToolError, ValueError, OSError, TimeoutError) as error:
        print(f"error: {error}", file=sys.stderr)
        return 1


if __name__ == "__main__":
    raise SystemExit(main())
