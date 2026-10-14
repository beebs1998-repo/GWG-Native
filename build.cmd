@echo off
setlocal
rem ---------------------------------------------------------------------------
rem Builds the 32-bit launcher and the injected shim with MSVC.
rem Requires Visual Studio 2022 with the x86 toolset (vcvarsall x86).
rem ---------------------------------------------------------------------------

set "ROOT=%~dp0"
pushd "%ROOT%"

rem The Direct3D 8 SDK headers are deliberately not redistributed with this
rem repository - see deps\README.md. Check for them before doing anything
rem expensive, so a fresh clone gets a sentence rather than a compiler error.
if not exist deps\dx8\d3d8.h (
    echo [FAIL] deps\dx8\d3d8.h is missing.
    echo        The Direct3D 8 SDK headers are not included in this repository.
    echo        See deps\README.md for what is needed and where to get it.
    popd
    exit /b 1
)

set "VCVARS=C:\Program Files\Microsoft Visual Studio\2022\Enterprise\VC\Auxiliary\Build\vcvarsall.bat"
if not exist "%VCVARS%" (
    set "VCVARS=C:\Program Files\Microsoft Visual Studio\2022\Community\VC\Auxiliary\Build\vcvarsall.bat"
)
if not exist "%VCVARS%" (
    set "VCVARS=C:\Program Files\Microsoft Visual Studio\2022\Professional\VC\Auxiliary\Build\vcvarsall.bat"
)
if not exist "%VCVARS%" (
    set "VCVARS=C:\Program Files\Microsoft Visual Studio\2022\BuildTools\VC\Auxiliary\Build\vcvarsall.bat"
)

if not exist "%VCVARS%" (
    echo [FAIL] Could not find vcvarsall.bat - edit this script to point at your VS install.
    popd
    exit /b 1
)

call "%VCVARS%" x86 >nul
if errorlevel 1 (
    echo [FAIL] vcvarsall x86 failed.
    popd
    exit /b 1
)

if not exist bin      mkdir bin
if not exist build\shim     mkdir build\shim
if not exist build\launcher mkdir build\launcher

echo [1/2] building shim ^(gwgshim32.dll^)
cl /nologo /W3 /O2 /MT /GS- /LD /Isrc\shim /Ideps\dx8 ^
   /Fobuild\shim\ src\shim\dllmain.cpp src\shim\hooks.cpp ^
   src\shim\iat.cpp src\shim\io_board.cpp src\shim\log.cpp src\shim\arcade_input.cpp src\shim\patches.cpp src\shim\trace.cpp src\shim\present.cpp src\shim\devprobe.cpp src\shim\rotate.cpp ^
   /Febin\gwgshim32.dll /link /DLL /OUT:bin\gwgshim32.dll user32.lib gdi32.lib
if errorlevel 1 (
    echo [FAIL] shim build failed.
    popd
    exit /b 1
)

echo [2/2] building launcher ^(gwg-native.exe^)
cl /nologo /W3 /O2 /MT /GS- /EHsc /c /Fobuild\launcher\ src\launcher\main.cpp
if errorlevel 1 (
    echo [FAIL] launcher compile failed.
    popd
    exit /b 1
)

link /nologo /OUT:gwg-native.exe build\launcher\main.obj
if errorlevel 1 (
    echo [FAIL] launcher link failed.
    popd
    exit /b 1
)

echo.
echo [OK ] gwg-native.exe and bin\gwgshim32.dll are ready.
echo       Run  gwg-native.exe  to start the game.
popd
exit /b 0