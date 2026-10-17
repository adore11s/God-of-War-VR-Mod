@echo off
rem ===========================================================================
rem  GOD OF WAR VR MOD - UNINSTALLER
rem
rem  Removes the mod from the game folder, puts back the settings.ini values it
rem  changed, and deletes this GOW_VR_MOD folder. Your saves are never touched.
rem
rem  The work is done by tools\gowvr_setup.ps1 - read it to see every step.
rem ===========================================================================
setlocal
rem  run from the game folder: a folder that is some window's current folder cannot be deleted
cd /d "%~dp0.."
powershell -NoProfile -ExecutionPolicy Bypass -File "%~dp0tools\gowvr_setup.ps1" -Mode Uninstall
exit /b 0
