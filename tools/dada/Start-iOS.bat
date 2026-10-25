@echo off
chcp 65001 >nul
setlocal
cd /d "%~dp0"
title dada - iOS 16.5 Temporary Boot
if not "%~1"=="" goto direct
:menu
cls
echo ====================================================
echo       hackios-dada-dev1.0  /  iOS 16.5 20F66
echo ====================================================
echo.
echo   1. 临时启动 UEFI 并加载 iOS 16.5
echo   2. 采集当前运行的 iOS 实时日志
echo   3. 检查 fastboot / xnuloader 连接
echo   4. 完整校验镜像和 fastboot 文件
echo   0. 退出
echo.
echo   启动仅使用 RAM，不刷写或清除手机分区。
echo   首次校验和上传 System 可能需要数分钟。
echo.
choice /c 12340 /n /m "请选择 [1/2/3/4/0]: "
if errorlevel 5 goto done
if errorlevel 4 goto verify
if errorlevel 3 goto check
if errorlevel 2 goto logs
goto start
:start
echo 正在检测 fastboot 设备，检测通过后才能开始启动...
powershell.exe -NoLogo -NoProfile -ExecutionPolicy Bypass -File "%~dp0Start-iOS.ps1" -Mode Check
if errorlevel 1 goto result
powershell.exe -NoLogo -NoProfile -ExecutionPolicy Bypass -File "%~dp0Start-iOS.ps1" -Mode Start
goto result
:logs
powershell.exe -NoLogo -NoProfile -ExecutionPolicy Bypass -File "%~dp0Start-iOS.ps1" -Mode Logs -LogDirectory "%~dp0log-out" -ShowLog
goto result
:check
powershell.exe -NoLogo -NoProfile -ExecutionPolicy Bypass -File "%~dp0Start-iOS.ps1" -Mode Check
goto result
:verify
powershell.exe -NoLogo -NoProfile -ExecutionPolicy Bypass -File "%~dp0Start-iOS.ps1" -Mode Verify
goto result
:direct
powershell.exe -NoLogo -NoProfile -ExecutionPolicy Bypass -File "%~dp0Start-iOS.ps1" %*
set "launch_result=%errorlevel%"
echo.
pause
exit /b %launch_result%
:result
set "launch_result=%errorlevel%"
echo.
if not "%launch_result%"=="0" echo 操作未完成，请查看上面的错误和 logs 目录。
pause
goto menu
:done
endlocal
exit /b 0
