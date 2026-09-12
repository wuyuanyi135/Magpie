@echo off
REM ============================================================
REM  Magpie 编译依赖补齐脚本（以管理员身份运行）
REM  作用：在已装的 VS2022 Community 上显式添加 Magpie 编译
REM        必需的三个核心子组件。
REM  GUI 直接勾"使用 C++ 的桌面开发"工作负荷时，其下的
REM  MSVC 编译器 / Win11 SDK / CMake 默认不勾选，会导致
REM  cl.exe 和 SDK 26100 缺失。本脚本用 --add 强制安装。
REM ============================================================

set "VSINSTALLER=C:\Program Files (x86)\Microsoft Visual Studio\Installer\vs_installer.exe"
set "INSTALLPATH=C:\Program Files\Microsoft Visual Studio\2022\Community"

if not exist "%VSINSTALLER%" (
    echo [错误] 找不到 VS Installer：%VSINSTALLER%
    echo 请确认 Visual Studio Installer 已安装。
    pause
    exit /b 1
)

echo 正在以管理员权限添加 Magpie 编译依赖（MSVC 编译器 + Win11 SDK 26100 + CMake）...
"%VSINSTALLER%" modify ^
    --installPath "%INSTALLPATH%" ^
    --add Microsoft.VisualStudio.Component.VC.Tools.x86.x64 ^
    --add Microsoft.VisualStudio.Component.Windows11SDK.26100 ^
    --add Microsoft.VisualStudio.Component.VC.CMake.Project ^
    --passive --norestart

echo.
echo 退出码：%ERRORLEVEL%
if %ERRORLEVEL%==0 (
    echo [完成] 依赖已添加。请回到 WorkBuddy 让我重新探测并开始编译。
) else (
    echo [失败] 安装器返回非零退出码，请以管理员身份重试，或检查网络。
)
pause
