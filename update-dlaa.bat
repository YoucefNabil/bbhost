@echo off
rem Updates the DLAA fork to the newest bbhost, builds it and installs it (tools\win\update_dlaa.ps1, docs\dlaa.md).
rem   update-dlaa.bat -InstallDir D:\bbhost [-DllDir D:\dlaa-dlls] [-Force] [-Setup] [-NoPush]
powershell -NoProfile -ExecutionPolicy Bypass -File "%~dp0tools\win\update_dlaa.ps1" %*
pause
