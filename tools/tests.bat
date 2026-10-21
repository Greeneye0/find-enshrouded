@echo off
rem Offline tests (chained hooks, hotkey parsing, proxy forwarding). Run build.bat first.
rem Objects go to build\tests so build.bat's link of build\*.obj never picks them up.
setlocal
cd /d "%~dp0.."
call "C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\VC\Auxiliary\Build\vcvars64.bat" >nul 2>&1
if not exist build\tests mkdir build\tests
cl /nologo /std:c++20 /EHsc /Fobuild\tests\ tools\tests.cpp src\scan.cpp src\config.cpp /Fe:build\tests\tests.exe || exit /b 1
cd build\tests
.\tests.exe || exit /b 1
