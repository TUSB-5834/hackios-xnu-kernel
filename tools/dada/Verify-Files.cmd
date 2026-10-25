@echo off
chcp 65001 >nul
cd /d "%~dp0"
powershell.exe -NoLogo -NoProfile -ExecutionPolicy Bypass -File "%~dp0Start-iOS.ps1" -Mode Verify
set "launch_result=%errorlevel%"
pause
exit /b %launch_result%
