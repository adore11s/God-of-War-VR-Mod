@echo off
rem ===========================================================================
rem  GOD OF WAR VR MOD - INSTALL (run it once)
rem
rem  Put this whole GOW_VR_MOD folder inside your God of War folder (the one
rem  containing GoW.exe) and run this file. It copies the mod (version.dll) next
rem  to GoW.exe. After that just start the game as usual - VR starts by itself.
rem
rem  The work is done by tools\gowvr_setup.ps1 - read it to see every step.
rem ===========================================================================
setlocal
powershell -NoProfile -ExecutionPolicy Bypass -File "%~dp0tools\gowvr_setup.ps1" -Mode Install & exit /b 0
