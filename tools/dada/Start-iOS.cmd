@echo off
chcp 65001 >nul
cd /d "%~dp0"
title dada - iOS 16.5 Temporary Boot
powershell.exe -NoLogo -NoProfile -ExecutionPolicy Bypass -File "%~dp0Start-iOS.ps1" %*
set "launch_result=%errorlevel%"
echo.
if not "%launch_result%"=="0" echo 启动未完成，请查看上面的错误和 logs 目录。
pause
exit /b %launch_result%
