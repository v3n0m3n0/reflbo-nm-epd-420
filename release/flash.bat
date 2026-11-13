@echo off
setlocal enabledelayedexpansion
title Flash reflbo-nm-epd-420 Release Firmware

set "PYTHONIOENCODING=utf-8"
set "PYTHONUTF8=1"
chcp 65001 >nul 2>&1

set "PORT=%~1"
if "%PORT%"=="" (
    set /p PORT="Enter COM port (e.g. COM15): "
)
if "%PORT%"=="" (
    echo [ERROR] No COM port specified.
    exit /b 1
)

powershell -NoProfile -ExecutionPolicy Bypass -File "%~dp0flash.ps1" -Port "%PORT%" %2
exit /b %ERRORLEVEL%
