@echo off
powershell -ExecutionPolicy Bypass -File "%~dp0download_sdk.ps1"
if %ERRORLEVEL% neq 0 (
    echo.
    echo Download FAILED. See error message above.
    pause
)
pause