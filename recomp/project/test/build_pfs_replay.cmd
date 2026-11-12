@echo off
call "C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\VC\Auxiliary\Build\vcvars64.bat" >nul
set "PATH=C:\Program Files\LLVM\bin;%PATH%"
cd /d C:\WDC_Recomp3
clang-cl /nologo /std:c++20 /EHsc /W3 -D_CRT_SECURE_NO_WARNINGS -I recomp\project\src recomp\project\test\pfs_replay.cpp recomp\project\src\wdc_pfs.cpp /Fe:recomp\project\test\pfs_replay.exe /Fo:recomp\project\test\
