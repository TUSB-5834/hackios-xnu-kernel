@echo off
chcp 65001 >nul
setlocal
cd /d "%~dp0"
title dada - iOS Real-time Logs
echo ====================================================
echo     dada iOS 16.5 实时日志采集
echo ====================================================
echo.
echo 请保持当前 iOS 开机并连接 USB。
echo 检测到受支持的 xnuloader 且 iOS 正在运行后开始采集。
echo 日志将保存到：%~dp0log-out\日期时间
echo 按 Q 结束电脑采集，手机会继续运行。
echo.
powershell.exe -NoLogo -NoProfile -ExecutionPolicy Bypass -File "%~dp0Start-iOS.ps1" -Mode Logs -LogDirectory "%~dp0log-out" -ShowLog %*
set "capture_result=%errorlevel%"
echo.
if not "%capture_result%"=="0" echo 采集未完成，请查看提示和 log-out 目录。
pause
exit /b %capture_result%
