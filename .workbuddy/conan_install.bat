@echo off
set "PATH=C:\Users\wuyua\.workbuddy\binaries\python\envs\default\Scripts;C:\Program Files\Microsoft Visual Studio\2022\Community\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin;C:\Program Files\Microsoft Visual Studio\2022\Community\Common7\IDE\CommonExtensions\Microsoft\CMake\Ninja;%PATH%"
set "HTTPS_PROXY="
set "HTTP_PROXY="
set "ALL_PROXY="
cd /d "C:\Users\wuyua\Project\Magpie\src\_ConanDeps"
conan install "C:\Users\wuyua\Project\Magpie\src\Magpie\conanfile.txt" ^
  -pr:a=msvc.profile ^
  --output-folder "C:\Users\wuyua\Project\Magpie\obj\x64\Release\_ConanDeps\Magpie" ^
  -s:a build_type=Release -s:a arch=x86_64 -s:a compiler.version=193 -s:a compiler.cppstd=17 ^
  -c:a user.magpie:msbuild_version=17.10.4 ^
  -c:a "tools.build:cxxflags=['/GR-','/GL']" ^
  -c:a "tools.build:cflags=['/GR-','/GL']" ^
  --build=missing --update
echo CONAN_EXIT=%ERRORLEVEL%
