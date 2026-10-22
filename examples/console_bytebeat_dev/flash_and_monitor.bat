@echo off
echo ========================================
echo ESP32 Bytebeat - Flash and Monitor
echo ========================================
echo.
echo This will flash the firmware to COM4 and open monitor
echo Close any open monitor sessions first!
echo.
pause

cd /d %~dp0

echo Activating ESP-IDF environment...
call C:\esp\v6.0.2\esp-idf\export.bat

echo.
echo Flashing to COM4...
idf.py -p COM4 flash monitor

pause
