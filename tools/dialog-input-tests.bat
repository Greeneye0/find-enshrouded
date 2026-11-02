@echo off
setlocal
cd /d "%~dp0.."
call "C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\VC\Auxiliary\Build\vcvars64.bat" >nul 2>&1
if not exist build\tests mkdir build\tests
cl /nologo /std:c++20 /O2 /EHsc /MT /W4 tools\dialog-input-tests.cpp /Fobuild\tests\dialog-input-tests.obj /Febuild\tests\dialog-input-tests.exe
if not "%errorlevel%"=="0" exit /b 1
build\tests\dialog-input-tests.exe %*
if not "%errorlevel%"=="0" exit /b 1
