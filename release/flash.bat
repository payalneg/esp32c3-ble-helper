@echo off
REM Windows flasher -- double-click in Explorer. Runs flash.py from the same folder.
cd /d "%~dp0"
where py >nul 2>&1
if %errorlevel%==0 (
    py flash.py
    goto :end
)
where python >nul 2>&1
if %errorlevel%==0 (
    python flash.py
    goto :end
)
echo Python not found. Install it: https://www.python.org/downloads/
echo During install, tick "Add Python to PATH".
pause
:end
