@echo off
rem ===========================================================================
rem  GOD OF WAR VR MOD - COPY MY SAVE (for a bug report)
rem
rem  Makes a COPY of your God of War saves as a zip file on your Desktop
rem  (GoW_VR_save_<date>.zip) that you can attach to a bug report. It works for
rem  the Steam, Epic and GOG copies of the game. Your saves are only READ - they
rem  are never changed, moved or deleted. Close the game first.
rem
rem  The work is done by tools\gowvr_setup.ps1 - read it to see every step.
rem ===========================================================================
setlocal
powershell -NoProfile -ExecutionPolicy Bypass -File "%~dp0tools\gowvr_setup.ps1" -Mode Save & exit /b 0
