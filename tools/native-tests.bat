@echo off
setlocal
cd /d "%~dp0.."
call "C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\VC\Auxiliary\Build\vcvars64.bat" >nul 2>&1
if not exist build\tests mkdir build\tests
cl /nologo /std:c++20 /O2 /MT /W4 /EHsc /Fobuild\tests\native-tests.obj tools\native-tests.cpp /Fe:build\tests\native-tests.exe || exit /b 1
build\tests\native-tests.exe || exit /b 1
