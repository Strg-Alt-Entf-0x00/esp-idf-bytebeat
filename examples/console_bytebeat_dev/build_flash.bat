@echo off
setlocal
echo ===================================
echo Bytebeat Dev - Build & Flash
echo ===================================
echo.

set PORT=%1
if "%PORT%"=="" set PORT=COM4

echo Target Port: %PORT%
echo.

where idf.py >nul 2>nul
if %ERRORLEVEL% EQU 0 goto :use_env

if not exist "C:\esp\v6.0.2\esp-idf" goto :no_idf

set IDF_PATH=C:\esp\v6.0.2\esp-idf
set PYTHON=C:\Espressif\tools\python\v6.0.2\venv\Scripts\python.exe
set IDF_PY=C:\esp\v6.0.2\esp-idf\tools\idf.py

echo [1/2] Building...
%PYTHON% %IDF_PY% build
if %ERRORLEVEL% NEQ 0 exit /b 1

echo.
echo [2/2] Flashing to %PORT%...
%PYTHON% %IDF_PY% -p %PORT% flash monitor
goto :done

:use_env
echo [1/2] Building...
idf.py build
if %ERRORLEVEL% NEQ 0 exit /b 1

echo.
echo [2/2] Flashing to %PORT%...
idf.py -p %PORT% flash monitor
goto :done

:no_idf
echo ERROR: ESP-IDF not found
exit /b 1

:done
endlocal
