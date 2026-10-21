@echo off
setlocal
cd /d "%~dp0.."
call "C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\VC\Auxiliary\Build\vcvars64.bat" >nul || exit /b 1
if not exist build-hot\tests mkdir build-hot\tests
cl /nologo /std:c++20 /MT /W4 /EHsc /D_CRT_SECURE_NO_WARNINGS /Fobuild-hot\tests\ tools\hot-tests.cpp /Fe:build-hot\tests\hot-tests.exe user32.lib /link /EXPORT:FindRuntimeInit || exit /b 1
build-hot\tests\hot-tests.exe || exit /b 1
