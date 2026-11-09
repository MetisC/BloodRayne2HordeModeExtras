@echo off
setlocal
pushd "%~dp0" || exit /b 1
set "VCVARS=C:\Program Files\Microsoft Visual Studio\2022\Community\VC\Auxiliary\Build\vcvarsall.bat"
if not defined DevEnvDir call "%VCVARS%" x86 >nul
if errorlevel 1 goto failed
set "BR2_BUILD=dinput8"
set "BR2_FLAGS="
set "BR2_OBJECT=br2mod"
if /I "%~1"=="test" (
    set "BR2_BUILD=dinput8-combat-test"
    set "BR2_FLAGS=/DBR2_COMBAT_TEST"
    set "BR2_OBJECT=br2mod-combat-test"
)
cl /nologo /O2 /MD /W3 /LD %BR2_FLAGS% /Fo%BR2_OBJECT%.obj br2mod.cpp /link /DEF:dinput8.def /OUT:%BR2_BUILD%.dll /MAP:%BR2_BUILD%.map /IMPLIB:%BR2_OBJECT%.lib
if errorlevel 1 goto failed
echo Built %BR2_BUILD%.dll. Close rayne2.exe before deploying and compare hashes.
popd
exit /b 0
:failed
echo Build FAILED. Do not deploy the previous DLL.
popd
exit /b 1
