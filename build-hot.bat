@echo off
setlocal
cd /d "%~dp0"
call "C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\VC\Auxiliary\Build\vcvars64.bat" >nul || exit /b 1
if not exist build-hot\runtime mkdir build-hot\runtime
if not exist build-hot\loader mkdir build-hot\loader
if not exist bin-hot mkdir bin-hot
cl /nologo /std:c++20 /O2 /MT /W4 /EHsc /DUNICODE /D_CRT_SECURE_NO_WARNINGS /DFIND_HOT_RUNTIME /DFIND_DEV /Fobuild-hot\runtime\ /c src\*.cpp || exit /b 1
link /nologo /DLL /OUT:bin-hot\Find.runtime.dll build-hot\runtime\*.obj user32.lib kernel32.lib crypt32.lib || exit /b 1
ml64 /nologo /c /Fo build-hot\loader\proxy_stubs.obj src\proxy_stubs.asm || exit /b 1
cl /nologo /std:c++20 /O2 /MT /W4 /EHsc /DUNICODE /D_CRT_SECURE_NO_WARNINGS /Fobuild-hot\loader\ /c loader\main.cpp src\proxy.cpp || exit /b 1
link /nologo /Brepro /DLL /OUT:bin-hot\wsock32.dll /DEF:src\wsock32.def build-hot\loader\*.obj user32.lib kernel32.lib || exit /b 1
echo Built bin-hot\wsock32.dll and bin-hot\Find.runtime.dll
