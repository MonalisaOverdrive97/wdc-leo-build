@echo off
call "C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\VC\Auxiliary\Build\vcvars64.bat" >nul
set "PATH=C:\Program Files\LLVM\bin;%PATH%"
cd /d C:\WDC_Recomp3
clang-cl /nologo /std:c++20 /EHsc -D_CRT_SECURE_NO_WARNINGS -I recomp\project\src recomp\project\test\test_input_map.cpp recomp\project\src\wdc_input.cpp recomp\project\src\wdc_config.cpp recomp\project\src\wdc_joy.cpp user32.lib dinput8.lib dxguid.lib /Fe:recomp\project\test\test_input_map.exe /Fo:recomp\project\test\ && recomp\project\test\test_input_map.exe
if errorlevel 1 exit /b 1
clang-cl /nologo /std:c++20 /EHsc -D_CRT_SECURE_NO_WARNINGS -I recomp\project\src recomp\project\test\test_input.cpp recomp\project\src\wdc_input.cpp recomp\project\src\wdc_config.cpp recomp\project\src\wdc_joy.cpp user32.lib dinput8.lib dxguid.lib /Fe:recomp\project\test\test_input.exe /Fo:recomp\project\test\ && recomp\project\test\test_input.exe
