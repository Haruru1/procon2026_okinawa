@echo off
setlocal DisableDelayedExpansion
powershell.exe -NoLogo -NoProfile -ExecutionPolicy Bypass -File "%~dp0run_fuel_test.ps1" -Interactive
set "RESULT=%ERRORLEVEL%"
echo.
echo Exit code: %RESULT%
pause
exit /b %RESULT%
