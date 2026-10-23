@echo off
rem Builds the DEV variant into bin-dev\loader\find.dll (+ mod.json) for the Greeneye Loader: adds
rem the dev bridge exports (FindQueueSearch, FindNativeStatus), dev logging and the @dev-*
rem searches. Hot reload itself needs no special build (the loader does it; tools\hot-reload.ps1).
rem Never ship it: package only build.bat's output.
setlocal
cd /d "%~dp0"
call "C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\VC\Auxiliary\Build\vcvars64.bat" >nul || exit /b 1
if not exist build-dev mkdir build-dev
if not exist bin-dev\loader mkdir bin-dev\loader
ml64 /nologo /c /Fo build-dev\proxy_stubs.obj src\proxy_stubs.asm || exit /b 1
rc /nologo /fo build-dev\version.res src\version.rc || exit /b 1
cl /nologo /std:c++20 /O2 /MT /W4 /EHsc /DUNICODE /D_CRT_SECURE_NO_WARNINGS /DFIND_DEV /Fobuild-dev\ /c src\*.cpp || exit /b 1
link /nologo /DLL /OUT:bin-dev\loader\find.dll /DEF:src\wsock32.def build-dev\*.obj build-dev\version.res user32.lib kernel32.lib crypt32.lib || exit /b 1
copy /y mod.json bin-dev\loader\mod.json >nul || exit /b 1
echo Built bin-dev\loader\find.dll (DEV)
