import os
import unittest
from pathlib import Path
from unittest import mock

os.environ.setdefault("QT_QPA_PLATFORM", "offscreen")

from PySide6.QtCore import QTimer, Qt
from PySide6.QtTest import QTest
from PySide6.QtWidgets import (
    QApplication,
    QMainWindow,
    QMenu,
    QMessageBox,
    QSpinBox,
    QSplitter,
    QWidgetAction,
)

from tools_py import bytebeat_tool


class FakeSerial:
    def __init__(self):
        self.responses = []
        self.writes = []
        self.is_open = True
        self.closed = False

    @property
    def in_waiting(self):
        return sum(len(response) for response in self.responses)

    def reset_input_buffer(self):
        self.responses.clear()

    def write(self, data):
        self.writes.append(data)
        command = data.decode("utf-8").strip()
        if command == "TOOL_ID":
            self.responses.append(f"{bytebeat_tool.FIRMWARE_ID}\r\n".encode())
        elif command.startswith("VLD:"):
            self.responses.append(b"BYTEBEAT:FORMULA:VALID\r\n")
        elif command.startswith("FORMULA:"):
            self.responses.append(b"[SERIAL] Playback STARTED\r\n")
        elif command == "STOP":
            self.responses.append(b"[SERIAL] Playback STOPPED\r\n")
        return len(data)

    def flush(self):
        pass

    def readline(self):
        return self.responses.pop(0) if self.responses else b""

    def close(self):
        self.closed = True
        self.is_open = False


class BytebeatQtGuiTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.app = QApplication.instance() or QApplication([])

    def test_log_has_resizable_space_and_startup_never_connects(self):
        observations = {}
        port = FakeSerial()
        original_worker = bytebeat_tool.SerialWorker
        bytebeat_tool.SerialWorker = lambda events, jobs: original_worker(
            events,
            jobs,
            serial_factory=lambda *_args, **_kwargs: port,
            boot_delay=0,
            identity_timeout=0.1,
        )

        def inspect_window():
            window = next(
                widget
                for widget in QApplication.topLevelWidgets()
                if isinstance(widget, QMainWindow)
                and widget.windowTitle() == "Bytebeat"
            )
            observations["window"] = window
            try:
                splitters = window.findChildren(QSplitter)
                main_splitter = next(
                    splitter
                    for splitter in splitters
                    if splitter.orientation() == Qt.Orientation.Vertical
                )
                self.assertGreaterEqual(window.log_view.minimumHeight(), 140)
                self.assertGreaterEqual(main_splitter.widget(1).minimumHeight(), 180)
                self.assertGreaterEqual(main_splitter.sizes()[1], 180)
                self.assertEqual(window.connection_state, "disconnected")
                self.assertFalse(window.worker.verified)
                self.assertEqual(port.writes, [])
                settings_menu = next(
                    menu
                    for menu in window.findChildren(QMenu)
                    if menu.title() == "&Settings"
                )
                spin_actions = [
                    action
                    for action in settings_menu.actions()
                    if isinstance(action, QWidgetAction)
                ]
                self.assertEqual(len(spin_actions), 2)
                volume_spin, timeout_spin = [
                    action.defaultWidget().findChild(QSpinBox)
                    for action in spin_actions
                ]
                self.assertEqual((volume_spin.value(), timeout_spin.value()), (80, 60))
                volume_spin.setValue(65)
                timeout_spin.setValue(30)
                self.assertEqual((window.volume, window.duration), (65, 30))
                view_menu = next(
                    action.menu()
                    for action in settings_menu.actions()
                    if action.menu() and action.menu().title() == "&View"
                )
                show_log_action = view_menu.actions()[0]
                self.assertTrue(show_log_action.isChecked())
                show_log_action.trigger()
                self.assertFalse(window.log_panel.isVisible())
                show_log_action.trigger()
                self.assertTrue(window.log_panel.isVisible())
                if window.log_file:
                    observations["log_path"] = Path(window.log_file.name)
                window.port_combo.addItem("COM_TEST")
                window.toggle_connection()
                QTimer.singleShot(20, lambda: wait_for_connection(window))
            except Exception as error:
                observations["error"] = error
                window.close()

        def wait_for_connection(window):
            if window.connection_state == "connected" and window.busy_action is None:
                window.editor.setPlainText("t>>4")
                QTest.mousePress(window.hold_button, Qt.MouseButton.LeftButton)
                QTimer.singleShot(100, lambda: release_hold_and_check(window))
                return
            QTimer.singleShot(20, lambda: wait_for_connection(window))

        def release_hold_and_check(window):
            QTest.mouseRelease(window.hold_button, Qt.MouseButton.LeftButton)
            QTimer.singleShot(20, lambda: wait_for_stop(window))

        def wait_for_stop(window):
            if window.busy_action is None and window.playback_state == "idle":
                try:
                    self.assertTrue(
                        any(line.startswith(b"FORMULA:") for line in port.writes)
                    )
                    self.assertIn(b"V:166\n", port.writes)
                    self.assertIn(b"T:30\n", port.writes)
                    self.assertEqual(port.writes[-1], b"STOP\n")
                    observations["hold_stop_confirmed"] = True
                except Exception as error:
                    observations["error"] = error
                window.close()
                return
            QTimer.singleShot(20, lambda: wait_for_stop(window))

        QTimer.singleShot(100, inspect_window)

        def timeout_window():
            if not observations.get("hold_stop_confirmed"):
                observations.setdefault(
                    "error", TimeoutError("Qt GUI test did not complete.")
                )
                window = observations.get("window")
                if window is not None:
                    window.close()

        QTimer.singleShot(5000, timeout_window)
        with mock.patch.object(
            QMessageBox,
            "question",
            return_value=QMessageBox.StandardButton.Yes,
        ):
            try:
                bytebeat_tool.run_gui()
            finally:
                bytebeat_tool.SerialWorker = original_worker

        log_path = observations.get("log_path")
        if log_path is not None:
            log_path.unlink(missing_ok=True)
        if "error" in observations:
            raise observations["error"]
        self.assertTrue(observations.get("hold_stop_confirmed"))
        self.assertTrue(port.closed)
