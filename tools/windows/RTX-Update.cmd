@echo off
powershell -NoProfile -ExecutionPolicy Bypass -File "%~dp0_ht2rtx_tools\Update.ps1" %*
echo.
pause
