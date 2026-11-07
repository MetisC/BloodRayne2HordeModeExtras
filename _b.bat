@echo off
call "C:\Program Files\Microsoft Visual Studio\2022\Community\VC\Auxiliary\Build\vcvarsall.bat" x86 >nul
cl /nologo /O2 /MD /W3 /LD br2mod.cpp /link /DEF:dinput8.def /OUT:dinput8.dll /MAP:dinput8.map
