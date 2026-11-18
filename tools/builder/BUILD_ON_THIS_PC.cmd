@echo off
rem Second Son Adaptive - one-click Windows builder. Everything goes to E:\SecondSonAdaptive.
rem It checks what is installed, asks before installing anything, builds the pinned sources,
rem runs the tests and packages the result. Log: E:\SecondSonAdaptive\Logs
cd /d "%~dp0"
powershell -NoProfile -ExecutionPolicy Bypass -File "%~dp0Build-SecondSonAdaptive.ps1" %*
echo.
pause
