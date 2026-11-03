@echo off
rem Builds WSL Manager with the Visual Studio C++ tools, CMake and Ninja.
rem Usage: build.cmd [Release|Debug]   (default: Release)
rem Set WSLMGR_BUILD_DIR to build somewhere other than .\build (for example when
rem Controlled Folder Access protects this folder from the compiler).
setlocal

set "CONFIG=%~1"
if "%CONFIG%"=="" set "CONFIG=Release"

set "VSWHERE=%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe"
if not exist "%VSWHERE%" (
    echo Visual Studio Installer not found. Install Visual Studio or the Build Tools with the C++ workload.
    exit /b 1
)
for /f "usebackq tokens=*" %%i in (`"%VSWHERE%" -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath`) do set "VSDIR=%%i"
if not defined VSDIR (
    echo No Visual Studio installation with the C++ x64 tools was found.
    exit /b 1
)

call "%VSDIR%\VC\Auxiliary\Build\vcvars64.bat" >nul || exit /b 1

set "BUILD_DIR=%~dp0build\%CONFIG%"
if defined WSLMGR_BUILD_DIR set "BUILD_DIR=%WSLMGR_BUILD_DIR%\%CONFIG%"
cmake -S "%~dp0." -B "%BUILD_DIR%" -G Ninja -DCMAKE_BUILD_TYPE=%CONFIG% || exit /b 1
cmake --build "%BUILD_DIR%" || exit /b 1

echo.
echo Built: %BUILD_DIR%\wslmgr.exe and %BUILD_DIR%\wslmgr-gui.exe
