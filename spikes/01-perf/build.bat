@echo off
rem Storm - Spike 01 build script.
rem Single translation unit, no project file. Run from anywhere.
rem
rem Note on quoting: several paths involved here contain "(x86)", and bare
rem parentheses inside a parenthesised cmd block terminate it early. Every
rem block below is therefore written flat, with goto instead of if/else.

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

cl /nologo /std:c++17 /EHsc /O2 /W3 /MD /DNDEBUG /DUNICODE /D_UNICODE ^
   /Fe:build\spike01.exe /Fo:build\ ^
   src\main.cpp ^
   /link user32.lib d3d12.lib dxgi.lib dxguid.lib
if errorlevel 1 goto :compile_failed

rem DXC is loaded at runtime, so both DLLs must sit beside the executable.
rem dxil.dll is the validator/signer; without it the driver rejects the shaders.
set "SDKBIN=%WindowsSdkVerBinPath%x64"
if exist "%SDKBIN%\dxcompiler.dll" copy /Y "%SDKBIN%\dxcompiler.dll" build\ >nul
if not exist "build\dxcompiler.dll" echo WARNING: dxcompiler.dll not found under "%SDKBIN%"
if exist "%SDKBIN%\dxil.dll" copy /Y "%SDKBIN%\dxil.dll" build\ >nul
if not exist "build\dxil.dll" echo WARNING: dxil.dll not found under "%SDKBIN%"

echo.
echo Built build\spike01.exe
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
