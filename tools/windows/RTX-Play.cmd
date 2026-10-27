@echo off
powershell -NoProfile -ExecutionPolicy Bypass -File "%~dp0_ht2rtx_tools\Play.ps1" %*
if errorlevel 1 pause
