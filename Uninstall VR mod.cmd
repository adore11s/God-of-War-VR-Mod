@echo off
rem ===========================================================================
rem  GOD OF WAR VR MOD - UNINSTALLER
rem
rem  Moves the mod's own files (version.dll, and if you want gowvr.ini and
rem  gowvr_logs) and this GOW_VR_MOD folder to the Recycle Bin. Nothing is deleted
rem  for good, and no game file (GoW.exe, your saves, ...) is ever touched.
rem
rem  The work is done by tools\gowvr_setup.ps1 - read it to see every step.
rem ===========================================================================
setlocal
rem  run from the game folder: a folder that is some window's current folder cannot be moved
cd /d "%~dp0.."
rem  one line: this file is moved to the Recycle Bin while it runs
powershell -NoProfile -ExecutionPolicy Bypass -File "%~dp0tools\gowvr_setup.ps1" -Mode Uninstall & exit /b 0
