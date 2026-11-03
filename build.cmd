@echo off
rem Builds WSL Manager with the Visual Studio C++ tools, CMake and Ninja.
rem Usage: build.cmd [Release|Debug]   (default: Release)
rem Set WSLMGR_BUILD_DIR to build somewhere other than .\build.
setlocal

set "CONFIG=%~1"
if "%CONFIG%"=="" set "CONFIG=Release"

call "%~dp0scripts\vs-env.cmd" || exit /b 1

set "BUILD_DIR=%~dp0build\%CONFIG%"
if defined WSLMGR_BUILD_DIR set "BUILD_DIR=%WSLMGR_BUILD_DIR%\%CONFIG%"
cmake -S "%~dp0." -B "%BUILD_DIR%" -G Ninja -DCMAKE_BUILD_TYPE=%CONFIG% || exit /b 1
cmake --build "%BUILD_DIR%" || exit /b 1

echo.
echo Built: %BUILD_DIR%\wslmgr.exe and %BUILD_DIR%\wslmgr-gui.exe
