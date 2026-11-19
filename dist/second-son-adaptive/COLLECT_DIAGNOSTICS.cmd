@echo off
rem Packs everything needed to diagnose a problem into diagnostics.zip next to this file:
rem emulator logs, the optimizer's reports and knowledge, input and mouse settings, system info.
rem Nothing is uploaded anywhere; send the zip yourself if you want help.
setlocal
cd /d "%~dp0"
set "D=%~dp0diagnostics"
if exist "%D%" rmdir /s /q "%D%"
mkdir "%D%"
rem The data folder shared by every build (next to this build folder), else the portable one.
set "U=%~dp0user"
if not exist "%~dp0portable.txt" if exist "%~dp0..\SecondSonAdaptive-data\user" set "U=%~dp0..\SecondSonAdaptive-data\user"
echo %U%> "%D%\data_folder.txt"
if exist "%U%\log" xcopy /e /i /q /y "%U%\log" "%D%\log" >nul
if exist "%U%\adaptive" (
    mkdir "%D%\adaptive"
    copy /y "%U%\adaptive\*.txt" "%D%\adaptive\" >nul 2>nul
    copy /y "%U%\adaptive\*.ini" "%D%\adaptive\" >nul 2>nul
    if exist "%U%\adaptive\reports" xcopy /e /i /q /y "%U%\adaptive\reports" "%D%\adaptive\reports" >nul
)
if exist "%U%\input_config" xcopy /e /i /q /y "%U%\input_config" "%D%\input_config" >nul
if exist "%U%\mouse-aim.ini" copy /y "%U%\mouse-aim.ini" "%D%\" >nul
if exist "%U%\custom_configs" xcopy /e /i /q /y "%U%\custom_configs" "%D%\custom_configs" >nul
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
