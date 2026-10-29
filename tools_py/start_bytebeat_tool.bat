@echo off
setlocal

set "TOOL_DIR=%~dp0"
set "PROJECT_ROOT=%TOOL_DIR%.."
set "VENV_DIR=%TOOL_DIR%.venv"
set "VENV_PY=%VENV_DIR%\Scripts\python.exe"
set "REQUIREMENTS=%TOOL_DIR%requirements.txt"
set "REQUIREMENTS_MARKER=%VENV_DIR%\requirements.sha256"

pushd "%PROJECT_ROOT%" || goto :error

if not exist "%VENV_PY%" goto :create_venv
goto :check_requirements

:create_venv
where py >nul 2>nul
if not errorlevel 1 (
    py -3 -c "import sys; raise SystemExit(0 if sys.version_info >= (3, 10) else 1)"
    if errorlevel 1 goto :python_version_error
    py -3 -m venv "%VENV_DIR%"
    if errorlevel 1 goto :venv_error
    goto :install_requirements
)

where python >nul 2>nul
if errorlevel 1 goto :python_missing
python -c "import sys; raise SystemExit(0 if sys.version_info >= (3, 10) else 1)"
if errorlevel 1 goto :python_version_error
python -m venv "%VENV_DIR%"
if errorlevel 1 goto :venv_error
goto :install_requirements

:check_requirements
"%VENV_PY%" -c "import sys; raise SystemExit(0 if sys.version_info >= (3, 10) else 1)"
if errorlevel 1 goto :python_version_error
if not exist "%REQUIREMENTS_MARKER%" goto :install_requirements
"%VENV_PY%" -c "import hashlib, pathlib, sys; requirements=pathlib.Path(sys.argv[1]); marker=pathlib.Path(sys.argv[2]); sys.exit(0 if marker.is_file() and marker.read_text(encoding='ascii') == hashlib.sha256(requirements.read_bytes()).hexdigest() else 1)" "%REQUIREMENTS%" "%REQUIREMENTS_MARKER%"
if errorlevel 1 goto :install_requirements
"%VENV_PY%" -c "import PySide6, serial, serial.tools.list_ports" >nul 2>nul
if errorlevel 1 goto :install_requirements
goto :launch

:install_requirements
echo Installing Bytebeat Tool dependencies into its local virtual environment...
"%VENV_PY%" -m pip install -r "%REQUIREMENTS%"
if errorlevel 1 goto :dependency_error
"%VENV_PY%" -c "import hashlib, pathlib, sys; requirements=pathlib.Path(sys.argv[1]); marker=pathlib.Path(sys.argv[2]); marker.write_text(hashlib.sha256(requirements.read_bytes()).hexdigest(), encoding='ascii')" "%REQUIREMENTS%" "%REQUIREMENTS_MARKER%"
if errorlevel 1 goto :marker_error

:launch
"%VENV_PY%" "%TOOL_DIR%bytebeat_tool.py" %*
set "RESULT=%ERRORLEVEL%"
popd
exit /b %RESULT%

:python_missing
echo Python 3.10 or newer was not found. Install Python from https://www.python.org/downloads/
echo and enable the Python launcher or add Python to PATH, then run this file again.
goto :failure

:python_version_error
echo Python 3.10 or newer is required. Install a supported Python version and try again.
goto :failure

:venv_error
echo Could not create the local Python virtual environment.
goto :failure

:dependency_error
echo Dependency installation failed. Check your internet connection and try again.
goto :failure

:marker_error
echo Dependencies were installed, but the setup marker could not be written.
goto :failure

:failure
set "RESULT=1"
popd
pause
exit /b %RESULT%

:error
echo Could not open the project directory.
pause
exit /b 1
