@echo off
rem ESP32 Bytebeat Project Backup Script
rem Excludes: .vscode, .documents, build artifacts, .md, .log files
setlocal
set SCRIPT_DIR=%~dp0
set PYLAUNCHER=

rem Find Python launcher
where py >nul 2>&1
if %errorlevel%==0 (
  set PYLAUNCHER=py
) else (
  where python >nul 2>&1
  if %errorlevel%==0 set PYLAUNCHER=python
)

if "%PYLAUNCHER%"=="" (
  echo ERROR: Python not found. Install Python 3 and make sure it's on PATH.
  pause
  endlocal
  exit /b 1
)

rem Run the backup script
"%PYLAUNCHER%" "%SCRIPT_DIR%python-scripts\esp_bytebeat_backup.py" %*

if %errorlevel% neq 0 (
  echo.
  echo Backup failed with error code %errorlevel%
  pause
  endlocal
  exit /b %errorlevel%
)

echo.
echo Press any key to close...
pause >nul
endlocal
