@echo off
rem ===========================================================================
rem  GOD OF WAR - VR MOD
rem
rem  Put this whole GOW_VR_MOD folder inside your God of War folder (the one
rem  containing GoW.exe) and run this file. It installs the mod (or updates it),
rem  applies the three game settings VR needs, and starts the game.
rem
rem  The work is done by tools\gowvr_setup.ps1 - read it to see every step.
rem ===========================================================================
setlocal
cd /d "%~dp0"
powershell -NoProfile -ExecutionPolicy Bypass -File "%~dp0tools\gowvr_setup.ps1" -Mode Play
exit /b %errorlevel%
