@echo off
setlocal
echo ============================================
echo ESP Bytebeat Console - ONE-CLICK FLASH
echo ============================================
echo.
echo This script will:
echo   1. Clean build (delete old files)
echo   2. Build from scratch
echo   3. Flash to ESP32
echo   4. Open serial monitor
echo.
echo Perfect for quick testing!
echo ============================================
echo.

REM Determine COM port (1st argument, or %ESPPORT%, or default to COM4)
set PORT=%1
if "%PORT%"=="" set PORT=%ESPPORT%

if not "%PORT%"=="" goto :port_ready
echo No COM port specified. Defaulting to COM4.
echo (Hint: You can specify a port: FLASH_AND_RUN.bat COM3)
set PORT=COM4

:port_ready
echo Target Port: %PORT%
echo.

where idf.py >nul 2>nul
if %ERRORLEVEL% EQU 0 goto :use_env

if not exist "C:\esp\v6.0.2\esp-idf" goto :no_idf

set IDF_PATH=C:\esp\v6.0.2\esp-idf
set ESP_IDF_VERSION=6.0.2
set IDF_PYTHON_ENV_PATH=C:\Espressif\tools\python\v6.0.2\venv
set PATH=C:\Espressif\tools\ninja\1.12.1;C:\Espressif\tools\idf-exe\1.0.3;C:\Espressif\tools\ccache\4.8.3\ccache-4.8.3-windows-x86_64;C:\Espressif\tools\xtensa-esp-elf-gdb\14.2_20240403\xtensa-esp-elf-gdb\bin;C:\Espressif\tools\riscv32-esp-elf-gdb\14.2_20240403\riscv32-esp-elf-gdb\bin;C:\Espressif\tools\xtensa-esp-elf\esp-15.2.0_20251204\xtensa-esp-elf\bin;C:\Espressif\tools\riscv32-esp-elf\esp-15.2.0_20251204\riscv32-esp-elf\bin;C:\Espressif\tools\esp32ulp-elf\2.38_20240113\esp32ulp-elf\bin;C:\Espressif\tools\openocd-esp32\v0.12.0-esp32-20240318\openocd-esp32\bin;%PATH%
set PYTHON=C:\Espressif\tools\python\v6.0.2\venv\Scripts\python.exe
set IDF_PY=C:\esp\v6.0.2\esp-idf\tools\idf.py

echo [1/3] Cleaning old build files...
%PYTHON% %IDF_PY% fullclean
if %ERRORLEVEL% NEQ 0 goto :failed

echo.
echo [2/3] Building from scratch...
%PYTHON% %IDF_PY% build
if %ERRORLEVEL% NEQ 0 goto :failed

echo.
echo [3/3] Flashing to %PORT% and opening monitor...
echo.
echo ============================================
echo TIP: Press Ctrl+] to exit monitor
echo ============================================
echo.
%PYTHON% %IDF_PY% -p %PORT% flash monitor

if %ERRORLEVEL% NEQ 0 goto :monitor_error

echo.
echo ============================================
echo Monitor closed.
echo ============================================
echo.
echo Next Steps:
echo.
echo 1. Send FORMULA commands manually:
echo    - Reconnect monitor: idf.py -p %PORT% monitor
echo    - Send: FORMULA:sin(t/100)
echo.
echo 2. Use Python Tool (recommended):
echo    - Open new terminal
echo    - cd ..\..\tools_py
echo    - python bytebeat_tool.py
echo.
echo 3. Explore 2422 formulas in library:
echo    - tools_py\library.json
echo.
goto :done

:monitor_error
echo.
echo ============================================
echo Monitor exited with error
echo ============================================
goto :done

:use_env
echo Using ESP-IDF from current environment...
echo.

echo [1/3] Cleaning old build files...
idf.py fullclean
if %ERRORLEVEL% NEQ 0 goto :failed

echo.
echo [2/3] Building from scratch...
idf.py build
if %ERRORLEVEL% NEQ 0 goto :failed

echo.
echo [3/3] Flashing to %PORT% and opening monitor...
echo.
echo ============================================
echo TIP: Press Ctrl+] to exit monitor
echo ============================================
echo.
idf.py -p %PORT% flash monitor

if %ERRORLEVEL% NEQ 0 goto :monitor_error

echo.
echo ============================================
echo Monitor closed.
echo ============================================
echo.
echo Next Steps:
echo.
echo 1. Send FORMULA commands manually:
echo    - Reconnect monitor: idf.py -p %PORT% monitor
echo    - Send: FORMULA:sin(t/100)
echo.
echo 2. Use Python Tool (recommended):
echo    - Open new terminal
echo    - cd ..\..\tools_py
echo    - python bytebeat_tool.py
echo.
echo 3. Explore 2422 formulas in library:
echo    - tools_py\library.json
echo.
goto :done

:monitor_error
echo.
echo ============================================
echo Monitor exited with error
echo ============================================
goto :done

:no_idf
echo ERROR: ESP-IDF not found in PATH or at C:\esp\v6.0.2\esp-idf.
echo Please run export.bat or open an ESP-IDF Terminal.
exit /b 1

:failed
echo.
echo ============================================
echo BUILD FAILED - Check errors above
echo ============================================
exit /b 1

:done
endlocal
