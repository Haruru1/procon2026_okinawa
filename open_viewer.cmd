@echo off
setlocal EnableExtensions DisableDelayedExpansion
title Procon 2026 - Open Viewer

set "ROOT=%~dp0"
set "VIEWER=%ROOT%viewer\App\result_viewer.exe"
set "RUNS=%ROOT%runs"

echo.
echo ==========================================
echo   Procon 2026 - Open Latest Viewer
echo ==========================================
echo.

if not exist "%VIEWER%" (
    echo [ERROR] result_viewer.exe was not found:
    echo         "%VIEWER%"
    echo.
    echo Build it first:
    echo   py -3.12 -B .\build.py gui
    goto :fail
)

if not exist "%RUNS%" (
    echo [ERROR] runs directory was not found:
    echo         "%RUNS%"
    goto :fail
)

set "JOURNAL="
set "LATEST_RUN="

for /f "usebackq delims=" %%J in (`powershell -NoProfile -Command "$d = Get-ChildItem -LiteralPath '%RUNS%' -Directory ^| Sort-Object LastWriteTime -Descending ^| Where-Object { Test-Path -LiteralPath (Join-Path $_.FullName 'journal.json') } ^| Select-Object -First 1; if ($d) { Write-Output $d.FullName }"`) do (
    set "LATEST_RUN=%%J"
)

if not defined LATEST_RUN (
    echo [ERROR] No run containing journal.json was found under:
    echo         "%RUNS%"
    goto :fail
)

set "JOURNAL=%LATEST_RUN%\journal.json"

echo Opening:
echo   %JOURNAL%
echo.
echo Viewer controls:
echo   F5      Reload the same journal
echo   Left/Right  Change day
echo.

start "" "%VIEWER%" "%JOURNAL%"
if errorlevel 1 (
    echo [ERROR] Failed to start result_viewer.exe.
    goto :fail
)

exit /b 0

:fail
echo.
pause
exit /b 1
