@echo off
rem Builds the per-user Windows installer: build\package\WslManager-<version>-x64.msi
rem Uses its own Release build in build\package, so it can run while the programs
rem in build\Release are open. WiX comes from the repo's dotnet tool manifest.
setlocal

call "%~dp0scripts\vs-env.cmd" || exit /b 1

set "BUILD_DIR=%~dp0build\package"
cmake -S "%~dp0." -B "%BUILD_DIR%" -G Ninja -DCMAKE_BUILD_TYPE=Release || exit /b 1
cmake --build "%BUILD_DIR%" || exit /b 1

set /p VERSION=<"%BUILD_DIR%\version.txt"
set "MSI=%BUILD_DIR%\WslManager-%VERSION%-x64.msi"

pushd "%~dp0"
dotnet tool restore >nul || (popd & exit /b 1)
rem The welcome/licence UI comes from the WiX UI extension (cached in .wix\).
dotnet tool run wix extension add WixToolset.UI.wixext/5.0.2 >nul || (popd & exit /b 1)
dotnet tool run wix build installer\Package.wxs -arch x64 -ext WixToolset.UI.wixext ^
    -bindpath installer -d "Version=%VERSION%" -d "BinDir=%BUILD_DIR%" -o "%MSI%" || (popd & exit /b 1)
popd

echo.
echo Built: %MSI%
