@echo off
rem Reports what the builder would need on this PC (tools, disk space) without changing anything.
cd /d "%~dp0"
powershell -NoProfile -ExecutionPolicy Bypass -File "%~dp0Build-SecondSonAdaptive.ps1" -CheckOnly %*
echo.
pause
