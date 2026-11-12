@echo off
rem Compila ultramodern + librecomp (N64ModernRuntime) com clang-cl + Ninja.
rem O CMakeLists do runtime usa -Wno-unused-parameter, que o cl.exe da MSVC rejeita (D8021); clang-cl aceita.
rem Uso: tools\recomp\build_runtime.cmd [configure|build|all]   (padrao: all)

call "C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\VC\Auxiliary\Build\vcvars64.bat" >nul
if errorlevel 1 exit /b 1
set "PATH=C:\Program Files\LLVM\bin;%PATH%"

set "RT=C:\WDC_Recomp3\thirdparty\N64ModernRuntime"
set "BLD=%RT%\build-clang"
set "STEP=%~1"
if "%STEP%"=="" set "STEP=all"

if /i "%STEP%"=="build" goto :build
cmake -S "%RT%" -B "%BLD%" -G Ninja -DCMAKE_BUILD_TYPE=Release -DCMAKE_C_COMPILER=clang-cl -DCMAKE_CXX_COMPILER=clang-cl
if errorlevel 1 exit /b 1
if /i "%STEP%"=="configure" exit /b 0

:build
cmake --build "%BLD%" --target ultramodern librecomp
exit /b %errorlevel%
