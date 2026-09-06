@echo off
rem Storm build. The project file is the source of truth; this is a convenience
rem wrapper so a build does not require opening the IDE.
rem
rem Usage:  build.bat [Debug|Release]      (default Release)
rem
rem Note on quoting: several paths here contain "(x86)", and bare parentheses
rem inside a parenthesised cmd block terminate it early. Every branch below is
rem written flat, with goto instead of if/else.

setlocal
pushd "%~dp0"

set "CONFIG=%~1"
if "%CONFIG%"=="" set "CONFIG=Release"

set "VSWHERE=%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe"
if not exist "%VSWHERE%" goto :no_vswhere

set "MSBUILD="
for /f "usebackq delims=" %%i in (`^""%VSWHERE%" -latest -products "*" -requires Microsoft.Component.MSBuild -find MSBuild\**\Bin\MSBuild.exe^"`) do set "MSBUILD=%%i"
if not defined MSBUILD goto :no_msbuild

"%MSBUILD%" Storm.vcxproj /nologo /v:minimal /p:Configuration=%CONFIG% /p:Platform=x64
if errorlevel 1 goto :build_failed

echo.
echo Built build\%CONFIG%\Storm.scr
echo   run full screen:  build\%CONFIG%\Storm.scr /s
echo   configuration:    build\%CONFIG%\Storm.scr /c
popd
endlocal
exit /b 0

:no_vswhere
echo ERROR: vswhere.exe not found. Visual Studio 2022 is required.
popd ^& exit /b 1

:no_msbuild
echo ERROR: MSBuild not found in the Visual Studio installation.
popd ^& exit /b 1

:build_failed
echo.
echo BUILD FAILED
popd ^& exit /b 1
