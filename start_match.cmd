@echo off
setlocal EnableExtensions DisableDelayedExpansion
title Procon 2026 - Start Match

set "ROOT=%~dp0"
set "TOKEN_FILE=%ROOT%private\procon_token.txt"
set "CLIENT=%ROOT%competition\solver\contest\match_client.py"
set "ENGINE=%ROOT%competition\solver\build\contest_solver.exe"
set "RUNS=%ROOT%runs"
set "SUPPLY_COUNT=1"

echo.
echo ==========================================
echo   Procon 2026 - Production Match Launcher
echo ==========================================
echo.

where py >nul 2>&1
if errorlevel 1 (
    echo [ERROR] Python launcher "py" was not found.
    goto :fail
)

if not exist "%TOKEN_FILE%" (
    echo [ERROR] Token file was not found:
    echo         "%TOKEN_FILE%"
    goto :fail
)

if not exist "%CLIENT%" (
    echo [ERROR] match_client.py was not found:
    echo         "%CLIENT%"
    goto :fail
)

if not exist "%ENGINE%" (
    echo [ERROR] contest_solver.exe was not found:
    echo         "%ENGINE%"
    echo.
    echo Build it first:
    echo   py -3.12 -B .\build.py engine
    goto :fail
)

if not exist "%RUNS%" mkdir "%RUNS%"
if errorlevel 1 (
    echo [ERROR] Could not create runs directory.
    goto :fail
)

set "PROCON_TOKEN="
set /p "PROCON_TOKEN="<"%TOKEN_FILE%"
if not defined PROCON_TOKEN (
    echo [ERROR] Token file is empty.
    goto :fail
)

set "BASE_URL="
set /p "BASE_URL=Contest server URL: "
if not defined BASE_URL (
    echo [ERROR] Server URL is empty.
    goto :fail
)

echo.
echo [1] Start a new match
echo [2] Resume the latest production match
echo.
choice /c 12 /n /m "Select [1/2]: "
if errorlevel 2 goto :resume
goto :newmatch

:newmatch
for /f "usebackq delims=" %%I in (`powershell -NoProfile -Command "Get-Date -Format yyyyMMdd_HHmmss"`) do set "STAMP=%%I"
if not defined STAMP (
    echo [ERROR] Could not create timestamp.
    goto :fail
)
set "RUN_DIR=%RUNS%\match_%STAMP%"
goto :confirm

:resume
set "RUN_DIR="
for /f "delims=" %%D in ('dir /b /ad /o-d "%RUNS%\match_*" 2^>nul') do (
    if not defined RUN_DIR set "RUN_DIR=%RUNS%\%%D"
)
if not defined RUN_DIR (
    echo [ERROR] No runs\match_* directory exists to resume.
    goto :fail
)
goto :confirm

:confirm
echo.
echo ------------------------------------------
echo Server URL   : %BASE_URL%
echo Supply count : 1 (fixed)
echo Run directory: %RUN_DIR%
echo Token        : loaded (hidden)
echo ------------------------------------------
echo.
choice /c YN /n /m "Start with these settings? [Y/N]: "
if errorlevel 2 goto :cancel

echo.
echo [START] match_client.py
echo.

py -3.12 "%CLIENT%" ^
  --base-url "%BASE_URL%" ^
  --allow-remote ^
  --run-dir "%RUN_DIR%" ^
  --supply-count 1 ^
  --engine "%ENGINE%"

set "EXITCODE=%ERRORLEVEL%"
set "PROCON_TOKEN="

echo.
if "%EXITCODE%"=="0" (
    echo [DONE] match_client.py exited normally.
) else (
    echo [ERROR] match_client.py exited with code %EXITCODE%.
    echo         Check the run directory and journal/stderr files.
)
echo.
pause
exit /b %EXITCODE%

:cancel
set "PROCON_TOKEN="
echo.
echo Cancelled.
pause
exit /b 0

:fail
set "PROCON_TOKEN="
echo.
pause
exit /b 1
