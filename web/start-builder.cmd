@echo off
rem Double-click to open the web builder on this PC. Close the window to stop it.
powershell -NoProfile -ExecutionPolicy Bypass -File "%~dp0start-builder.ps1" %*
pause
