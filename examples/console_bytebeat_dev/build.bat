@echo off
setlocal
echo ===================================
echo Bytebeat Dev - Build
echo ===================================
echo.

where idf.py >nul 2>nul
if %ERRORLEVEL% EQU 0 goto :use_env

if not exist "C:\esp\v6.0.2\esp-idf" goto :no_idf

set IDF_PATH=C:\esp\v6.0.2\esp-idf
set PYTHON=C:\Espressif\tools\python\v6.0.2\venv\Scripts\python.exe
set IDF_PY=C:\esp\v6.0.2\esp-idf\tools\idf.py

echo Building...
%PYTHON% %IDF_PY% build
goto :done

:use_env
echo Building...
idf.py build
goto :done

:no_idf
echo ERROR: ESP-IDF not found
exit /b 1

:done
endlocal
