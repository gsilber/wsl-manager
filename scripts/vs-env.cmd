@echo off
rem Sets up the Visual Studio x64 C++ build environment (cl, link, rc, cmake, ninja)
rem in the calling script's environment.  Usage:  call scripts\vs-env.cmd || exit /b 1
if defined VCToolsInstallDir exit /b 0

set "VS_INSTALLER=%ProgramFiles(x86)%\Microsoft Visual Studio\Installer"
if not exist "%VS_INSTALLER%\vswhere.exe" (
    echo Visual Studio Installer not found. Install Visual Studio or the Build Tools with the C++ workload.
    exit /b 1
)
rem vcvars64.bat runs vswhere itself and expects to find it on PATH.
set "PATH=%VS_INSTALLER%;%PATH%"

set "VS_DIR="
for /f "usebackq tokens=*" %%i in (`vswhere.exe -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath`) do set "VS_DIR=%%i"
if not defined VS_DIR (
    echo No Visual Studio installation with the C++ x64 tools was found.
    exit /b 1
)
call "%VS_DIR%\VC\Auxiliary\Build\vcvars64.bat" >nul
