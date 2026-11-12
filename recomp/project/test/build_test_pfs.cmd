@echo off
call "C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\VC\Auxiliary\Build\vcvars64.bat" >nul
set "PATH=C:\Program Files\LLVM\bin;%PATH%"
cd /d C:\WDC_Recomp3
clang-cl /nologo /std:c++20 /EHsc /W3 -I recomp\project\src recomp\project\test\test_pfs.cpp recomp\project\src\wdc_pfs.cpp /Fe:recomp\project\test\test_pfs.exe /Fo:recomp\project\test\ && recomp\project\test\test_pfs.exe
