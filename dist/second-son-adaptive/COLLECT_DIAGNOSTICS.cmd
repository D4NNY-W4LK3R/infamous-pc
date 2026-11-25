@echo off
rem Packs everything needed to diagnose a problem into Build<number>_Diagnostics.zip next to
rem this file (diagnostics.zip for builds without BUILD.txt):
rem emulator logs, the optimizer's reports and knowledge, input and mouse settings, system info.
rem Nothing is uploaded anywhere; send the zip yourself if you want help.
setlocal
cd /d "%~dp0"
set "B="
if exist "BUILD.txt" set /p B=<"BUILD.txt"
set "Z=diagnostics.zip"
if defined B set "Z=Build%B%_Diagnostics.zip"
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
if exist "BUILD.txt" copy /y "BUILD.txt" "%D%\" >nul
systeminfo > "%D%\systeminfo.txt" 2>nul
rem Windows records why a program ended when the emulator could not log it (exception code and
rem module of an application crash, 1000; hangs, 1002): the last ones of shadPS4.
powershell -NoProfile -Command "Get-WinEvent -FilterHashtable @{LogName='Application'; Id=1000,1001,1002} -MaxEvents 200 -ErrorAction SilentlyContinue | Where-Object { $_.Message -match 'shadPS4' } | Select-Object -First 8 | Format-List TimeCreated,Id,Message" > "%D%\app_errors.txt" 2>&1
rem The display driver's own records of the card stopping (build 50): Windows resetting it after
rem a timeout (Display 4101) and the NVIDIA driver's errors (nvlddmkm 13, 14, 153 and others).
powershell -NoProfile -Command "Get-WinEvent -FilterHashtable @{LogName='System'; ProviderName='nvlddmkm','Display'} -MaxEvents 20 -ErrorAction SilentlyContinue | Format-List TimeCreated,ProviderName,Id,Message" > "%D%\gpu_driver_events.txt" 2>&1
rem Hardware-accelerated GPU scheduling (HwSchMode 2 = on, 1 = off; missing = off).
reg query "HKLM\SYSTEM\CurrentControlSet\Control\GraphicsDrivers" /v HwSchMode > "%D%\gpu_scheduling.txt" 2>&1
powershell -NoProfile -Command "Get-CimInstance Win32_VideoController | Select-Object Name,DriverVersion,AdapterRAM | Format-List | Out-File -Encoding utf8 '%D%\gpu.txt'; Get-PSDrive -PSProvider FileSystem | Select-Object Name,@{n='FreeGB';e={[math]::Round($_.Free/1GB,1)}} | Format-Table | Out-File -Encoding utf8 '%D%\drives.txt'"
if exist "%Z%" del /q "%Z%"
powershell -NoProfile -Command "Compress-Archive -Path '%D%\*' -DestinationPath '%~dp0%Z%' -Force"
rmdir /s /q "%D%"
echo.
echo Wrote %~dp0%Z%
pause
endlocal
