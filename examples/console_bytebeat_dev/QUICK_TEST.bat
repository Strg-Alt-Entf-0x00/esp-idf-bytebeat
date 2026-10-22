@echo off
echo ========================================
echo ESP32 Bytebeat - Quick Flash (5sec Auto-Stop)
echo ========================================
echo.
echo WICHTIG: Schliesse den Monitor JETZT (Ctrl+])
echo.
timeout /t 3
echo Flashing...
echo.

cd /d %~dp0

C:\esp\python_env\idf6.0_py3.13_env\Scripts\python.exe -m esptool --chip esp32p4 -p COM4 -b 460800 --before default-reset --after hard-reset write-flash --flash-mode dio --flash-size 32MB --flash-freq 40m 0x2000 build\bootloader\bootloader.bin 0x8000 build\partition_table\partition-table.bin 0x10000 build\esp_bytebeat_console.bin

echo.
echo ========================================
echo Flash complete! 
echo Audio wird nach 5 Sekunden automatisch stoppen.
echo ========================================
echo.
echo Druecke eine Taste um Monitor zu starten...
pause > nul

C:\esp\python_env\idf6.0_py3.13_env\Scripts\python.exe C:\esp\v6.0.2\esp-idf\tools\idf_monitor.py -p COM4 -b 115200 --toolchain-prefix riscv32-esp-elf- --target esp32p4 build\esp_bytebeat_console.elf
