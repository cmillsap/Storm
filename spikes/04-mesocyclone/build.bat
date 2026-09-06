@echo off
rem Storm - Spike 03 build script. Pure CPU, no graphics dependencies.

setlocal
set "ROOT=%~dp0"
pushd "%ROOT%"

set "VSWHERE=%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe"
if not exist "%VSWHERE%" goto :no_vswhere

set "VSPATH="
for /f "usebackq delims=" %%i in (`^""%VSWHERE%" -latest -products "*" -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath^"`) do set "VSPATH=%%i"
if not defined VSPATH goto :no_toolset

call "%VSPATH%\VC\Auxiliary\Build\vcvars64.bat" >nul
if errorlevel 1 goto :no_vcvars

if not exist build mkdir build

cl /nologo /std:c++17 /EHsc /O2 /fp:fast /openmp /W3 /MD /DNDEBUG ^
   /Fe:build\spike04.exe /Fo:build\ src\main.cpp
if errorlevel 1 goto :compile_failed

echo.
echo Built build\spike04.exe
popd
endlocal
exit /b 0

:no_vswhere
echo ERROR: vswhere.exe not found. Visual Studio 2022 is required.
popd & exit /b 1
:no_toolset
echo ERROR: no Visual Studio install with the C++ desktop toolset was found.
popd & exit /b 1
:no_vcvars
echo ERROR: vcvars64.bat failed.
popd & exit /b 1
:compile_failed
echo.
echo BUILD FAILED
popd & exit /b 1
