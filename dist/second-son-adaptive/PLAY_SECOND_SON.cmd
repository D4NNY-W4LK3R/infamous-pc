@echo off
rem Second Son Adaptive - launcher.
rem Keeps everything (saves, settings, shader cache, logs, analyser reports) in the "user"
rem folder next to this file, so nothing is written to C:. The first start asks for the game
rem folder (the one with eboot.bin) and remembers it in game_path.txt.
setlocal EnableExtensions
cd /d "%~dp0"
if not exist "user" mkdir "user"

set "GAME_DIR="
if not "%~1"=="" set "GAME_DIR=%~1"
if not defined GAME_DIR if exist "game_path.txt" set /p GAME_DIR=<"game_path.txt"

:ask
if not defined GAME_DIR (
    echo.
    echo Paste the path of your inFAMOUS Second Son folder - the one that contains eboot.bin
    echo ^(base game + update 1.07 merged^), then press Enter:
    set /p GAME_DIR=^>
)
set "GAME_DIR=%GAME_DIR:"=%"
if not exist "%GAME_DIR%\eboot.bin" (
    echo.
    echo No eboot.bin in "%GAME_DIR%". Try again.
    set "GAME_DIR="
    goto ask
)
> "game_path.txt" echo %GAME_DIR%

echo Starting inFAMOUS Second Son from "%GAME_DIR%"
echo   F1 analyser + cheats ^| F7 mouse look on/off ^| - / = mouse speed ^| F10 FPS ^| F11 fullscreen
start "" "%~dp0shadPS4.exe" --show-fps -g "%GAME_DIR%\eboot.bin"
endlocal
