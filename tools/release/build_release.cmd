@echo off
rem Build de distribuicao (Leo Build): CRT estatica (/MT), aplicativo de janela (sem console), sem caminhos de desenvolvimento.
rem Saida: recomp\project\build-release\wdc_recomp.exe   (empacote com: python tools\release\make_package.py)
call "C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\VC\Auxiliary\Build\vcvars64.bat" >nul
if errorlevel 1 exit /b 1
set "PATH=C:\Program Files\LLVM\bin;%PATH%"
set "SRC=C:\WDC_Recomp3\recomp\project"
set "BLD=%SRC%\build-release"
cmake -S "%SRC%" -B "%BLD%" -G Ninja -DCMAKE_BUILD_TYPE=Release -DCMAKE_C_COMPILER=clang-cl -DCMAKE_CXX_COMPILER=clang-cl -DWDC_RELEASE=ON -DWDC_TRACE=OFF
if errorlevel 1 exit /b 1
cmake --build "%BLD%" --target wdc_recomp
exit /b %errorlevel%
