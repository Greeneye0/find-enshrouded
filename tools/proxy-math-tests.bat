@echo off
setlocal
cd /d "%~dp0.."
call "C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\VC\Auxiliary\Build\vcvars64.bat" >nul || exit /b 1
if not exist build-hot\tests mkdir build-hot\tests
cl /nologo /std:c++20 /MT /W4 /EHsc /Fobuild-hot\tests\ tools\proxy-math-tests.cpp /Fe:build-hot\tests\proxy-math-tests.exe || exit /b 1
build-hot\tests\proxy-math-tests.exe || exit /b 1
