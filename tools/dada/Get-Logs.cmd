@echo off
chcp 65001 >nul
cd /d "%~dp0"
title dada - iOS Live Logs
powershell.exe -NoLogo -NoProfile -ExecutionPolicy Bypass -File "%~dp0Start-iOS.ps1" -Mode Logs %*
set "launch_result=%errorlevel%"
pause
exit /b %launch_result%
