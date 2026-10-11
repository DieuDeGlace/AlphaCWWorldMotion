@echo off
setlocal
cd /d "%~dp0"
if not exist build\Mods mkdir build\Mods
if not exist build\tests mkdir build\tests
pushd src
cl /nologo /std:c++17 /O2 /MT /EHsc /DUNICODE /D_UNICODE /LD AlphaWorldMotion.cpp /Fe..\build\Mods\AlphaWorldMotion.dll /link user32.lib
if errorlevel 1 exit /b 1
cl /nologo /std:c++17 /O2 /MT /EHsc MotionRulesTest.cpp /Fe..\build\tests\MotionRulesTest.exe
if errorlevel 1 exit /b 1
cl /nologo /std:c++17 /O2 /MT /EHsc /DUNICODE /D_UNICODE MotionGpuTest.cpp /Fe..\build\tests\MotionGpuTest.exe /link user32.lib d3d9.lib
set "result=%ERRORLEVEL%"
popd
exit /b %result%
