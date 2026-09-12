@echo off
setlocal
call "C:\Program Files\Microsoft Visual Studio\2022\Community\Common7\Tools\VsDevCmd.bat" -arch=amd64 -host_arch=amd64
set "PATH=C:\Users\wuyua\.workbuddy\binaries\python\envs\default\Scripts;%PATH%"
rem 移除大小写重复的代理变量，避免 MSBuild 环境字典冲突
set "HTTPS_PROXY="
set "HTTP_PROXY="
set "ALL_PROXY="
set "SOLUTIONDIR=C:\Users\wuyua\Project\Magpie\"
set "MSBUILD=C:\Program Files\Microsoft Visual Studio\2022\Community\MSBuild\Current\Bin\MSBuild.exe"
set "CFG=/p:Configuration=Release /p:Platform=x64 /p:PlatformToolset=v143 /p:SolutionDir=%SOLUTIONDIR% /v:minimal /m /nologo"

rem conan install 已通过 conan_install.bat 手动完成（生成 conandeps.props）
rem 此处不再走 MSBuild 的 _ConanDeps 目标，因为 MSBuild 的 Exec 子进程拿不到 conan 的 PATH

echo === build Magpie ===
"%MSBUILD%" "C:\Users\wuyua\Project\Magpie\src\Magpie\Magpie.vcxproj" %CFG% /fl /flp:logfile="C:\Users\wuyua\Project\Magpie\.workbuddy\build.log" /flp:verbosity=minimal
if errorlevel 1 goto :fail

echo === BUILD OK ===
goto :eof

:fail
echo === BUILD FAILED ===
exit /b 1
endlocal
