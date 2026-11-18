@echo off
rem Packs everything needed to diagnose a problem into diagnostics.zip next to this file:
rem emulator logs, the optimizer's reports and knowledge, input and mouse settings, system info.
rem Nothing is uploaded anywhere; send the zip yourself if you want help.
setlocal
cd /d "%~dp0"
set "D=%~dp0diagnostics"
if exist "%D%" rmdir /s /q "%D%"
mkdir "%D%"
if exist "user\log" xcopy /e /i /q /y "user\log" "%D%\log" >nul
if exist "user\adaptive" (
    mkdir "%D%\adaptive"
    copy /y "user\adaptive\*.txt" "%D%\adaptive\" >nul 2>nul
    copy /y "user\adaptive\*.ini" "%D%\adaptive\" >nul 2>nul
    if exist "user\adaptive\reports" xcopy /e /i /q /y "user\adaptive\reports" "%D%\adaptive\reports" >nul
)
if exist "user\input_config" xcopy /e /i /q /y "user\input_config" "%D%\input_config" >nul
if exist "user\mouse-aim.ini" copy /y "user\mouse-aim.ini" "%D%\" >nul
if exist "user\custom_configs" xcopy /e /i /q /y "user\custom_configs" "%D%\custom_configs" >nul
if exist "SHA256.txt" copy /y "SHA256.txt" "%D%\" >nul
if exist "SOURCE_PINS.txt" copy /y "SOURCE_PINS.txt" "%D%\" >nul
systeminfo > "%D%\systeminfo.txt" 2>nul
powershell -NoProfile -Command "Get-CimInstance Win32_VideoController | Select-Object Name,DriverVersion,AdapterRAM | Format-List | Out-File -Encoding utf8 '%D%\gpu.txt'; Get-PSDrive -PSProvider FileSystem | Select-Object Name,@{n='FreeGB';e={[math]::Round($_.Free/1GB,1)}} | Format-Table | Out-File -Encoding utf8 '%D%\drives.txt'"
if exist "diagnostics.zip" del /q "diagnostics.zip"
powershell -NoProfile -Command "Compress-Archive -Path '%D%\*' -DestinationPath '%~dp0diagnostics.zip' -Force"
rmdir /s /q "%D%"
echo.
echo Wrote %~dp0diagnostics.zip
pause
endlocal
