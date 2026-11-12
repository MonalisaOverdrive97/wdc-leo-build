@echo off
rem Compila o projeto minimo (recomp\project) com clang-cl + Ninja: runtime + C gerado + main.
rem Uso: tools\recomp\build_project.cmd [configure|build|all] [trace]
rem   trace : usa recomp\RecompiledFuncsTrace (gerado com `gen_wdc_toml.py --trace`) e o diretorio build-trace

call "C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\VC\Auxiliary\Build\vcvars64.bat" >nul
if errorlevel 1 exit /b 1
set "PATH=C:\Program Files\LLVM\bin;%PATH%"

set "SRC=C:\WDC_Recomp3\recomp\project"
set "BLD=%SRC%\build"
set "TRACEOPT=-DWDC_TRACE=OFF"
if /i "%~2"=="trace" (
    set "BLD=%SRC%\build-trace"
    set "TRACEOPT=-DWDC_TRACE=ON"
)
set "STEP=%~1"
if "%STEP%"=="" set "STEP=all"

if /i "%STEP%"=="build" goto :build
cmake -S "%SRC%" -B "%BLD%" -G Ninja -DCMAKE_BUILD_TYPE=Release -DCMAKE_C_COMPILER=clang-cl -DCMAKE_CXX_COMPILER=clang-cl %TRACEOPT%
if errorlevel 1 exit /b 1
if /i "%STEP%"=="configure" exit /b 0

:build
cmake --build "%BLD%" --target wdc_recomp
exit /b %errorlevel%
